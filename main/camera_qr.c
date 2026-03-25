#include "camera_qr.h"

#include "esp_check.h"
#include "bsp/i2c.h"
#include "bsp/tanmatsu.h"
#include "tanmatsu_coprocessor.h"

#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp_lvgl.h"

#include "quirc/quirc.h"

#include <string.h>

static const char *TAG = "camera_qr";

// Camera sensor resolution (OV5647 MIPI 2-lane mode)
#define CAM_W              800
#define CAM_H              640
#define CAM_LANE_RATE_MBPS 200
#define CAM_FORMAT_NAME    "MIPI_2lane_24Minput_RAW8_800x640_50fps"
#define CAM_SCCB_FREQ_HZ   10000

// Row offset to center-crop CAM_H→CAM_QR_DISP_H
#define CAM_CROP_ROW_OFFSET ((CAM_H - CAM_QR_DISP_H) / 2)

typedef struct {
    camera_qr_result_cb_t   result_cb;
    lv_obj_t               *preview_canvas;
    lv_timer_t             *poll_timer;
    TaskHandle_t            task_handle;

    volatile bool           stop_flag;
    volatile bool           result_ready;
    char                    result_buf[256];

    esp_cam_ctlr_handle_t   cam_handle;
    isp_proc_handle_t       isp_proc;
    esp_sccb_io_handle_t    sccb_io;
    esp_cam_sensor_device_t *sensor;

    uint16_t               *frame_buf;     // CAM_W * CAM_H * 2 bytes, PSRAM
    uint16_t               *disp_buf;      // CAM_QR_DISP_W * CAM_QR_DISP_H * 2 bytes, PSRAM
} camera_qr_ctx_t;

static camera_qr_ctx_t s_ctx;

// ---- Sensor init on existing BSP I2C bus ----

static esp_err_t sensor_init_on_bsp_bus(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_primary_bus_get_handle(&bus),
                        TAG, "Failed to get BSP I2C bus handle");

    esp_cam_sensor_config_t cam_cfg = {
        .sccb_handle = NULL,
        .reset_pin   = -1,
        .pwdn_pin    = -1,
        .xclk_pin    = -1,
    };

    esp_cam_sensor_device_t *cam = NULL;

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t sccb_cfg = {
            .scl_speed_hz    = CAM_SCCB_FREQ_HZ,
            .device_address  = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        esp_err_t err = sccb_new_i2c_io(bus, &sccb_cfg, &cam_cfg.sccb_handle);
        if (err != ESP_OK) continue;

        cam_cfg.sensor_port = p->port;
        cam = (*(p->detect))(&cam_cfg);
        if (cam) {
            if (p->port != ESP_CAM_SENSOR_MIPI_CSI) {
                ESP_LOGE(TAG, "Detected sensor is not a MIPI CSI sensor");
                cam = NULL;
            }
            break;
        }
        esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
        cam_cfg.sccb_handle = NULL;
    }

    if (!cam) {
        ESP_LOGE(TAG, "No MIPI CSI camera sensor detected on BSP I2C bus");
        return ESP_ERR_NOT_FOUND;
    }

    // Find and set the desired format
    esp_cam_sensor_format_array_t fmt_array = {};
    esp_cam_sensor_query_format(cam, &fmt_array);
    const esp_cam_sensor_format_t *target_fmt = NULL;
    for (int i = 0; i < (int)fmt_array.count; i++) {
        if (strcmp(fmt_array.format_array[i].name, CAM_FORMAT_NAME) == 0) {
            target_fmt = &fmt_array.format_array[i];
            break;
        }
    }
    if (!target_fmt) {
        ESP_LOGE(TAG, "Format '%s' not found on sensor", CAM_FORMAT_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(
        esp_cam_sensor_set_format(cam, target_fmt),
        TAG, "Failed to set camera format");

    int stream_en = 1;
    ESP_RETURN_ON_ERROR(
        esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_en),
        TAG, "Failed to start sensor stream");

    s_ctx.sccb_io = cam_cfg.sccb_handle;
    s_ctx.sensor  = cam;
    ESP_LOGI(TAG, "Camera sensor ready: %s", CAM_FORMAT_NAME);
    return ESP_OK;
}

// ---- Frame processing helpers ----

// Convert a single RGB565 pixel to grayscale (ITU-R BT.601 luma approximation).
static inline uint8_t rgb565_to_gray(uint16_t px)
{
    uint8_t r = (px >> 11) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    return (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
}

// ---- LVGL poll timer ----

static void qr_poll_timer_cb(lv_timer_t *timer)
{
    // Refresh preview canvas so LVGL re-reads disp_buf
    if (s_ctx.preview_canvas) {
        lv_obj_invalidate(s_ctx.preview_canvas);
    }

    if (s_ctx.result_ready && s_ctx.result_cb) {
        camera_qr_result_cb_t cb = s_ctx.result_cb;
        s_ctx.result_cb  = NULL;
        s_ctx.poll_timer = NULL;
        lv_timer_delete(timer);  // safe to self-delete in LVGL v9
        s_ctx.stop_flag = true;
        cb(s_ctx.result_buf);
    }
}

// ---- Camera frame + QR decode task ----

static bool IRAM_ATTR on_get_new_trans(
    esp_cam_ctlr_handle_t handle,
    esp_cam_ctlr_trans_t *trans, void *user_data)
{
    esp_cam_ctlr_trans_t *t = (esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = t->buffer;
    trans->buflen = t->buflen;
    return false;
}

static bool IRAM_ATTR on_trans_finished(
    esp_cam_ctlr_handle_t handle,
    esp_cam_ctlr_trans_t *trans, void *user_data)
{
    return false;
}

static void camera_qr_task(void *arg)
{
    struct quirc *qr = quirc_new();
    if (!qr) {
        ESP_LOGE(TAG, "Failed to allocate quirc decoder");
        goto cleanup;
    }
    if (quirc_resize(qr, CAM_W, CAM_H) != 0) {
        ESP_LOGE(TAG, "Failed to resize quirc decoder");
        goto cleanup;
    }

    esp_cam_ctlr_trans_t trans = {
        .buffer = s_ctx.frame_buf,
        .buflen = (size_t)CAM_W * CAM_H * 2,
    };

    while (!s_ctx.stop_flag) {
        esp_err_t err = esp_cam_ctlr_receive(
            s_ctx.cam_handle, &trans, 500);
        if (err != ESP_OK) continue;

        // Copy center crop to display buffer (no LVGL lock — torn frames acceptable)
        const uint16_t *src = s_ctx.frame_buf + CAM_CROP_ROW_OFFSET * CAM_W;
        memcpy(s_ctx.disp_buf, src,
               (size_t)CAM_QR_DISP_W * CAM_QR_DISP_H * 2);

        if (s_ctx.stop_flag) break;

        // Feed full frame to quirc as grayscale
        uint8_t *gray = quirc_begin(qr, NULL, NULL);
        for (int i = 0; i < CAM_W * CAM_H; i++) {
            gray[i] = rgb565_to_gray(s_ctx.frame_buf[i]);
        }
        quirc_end(qr);

        int n_codes = quirc_count(qr);
        for (int ci = 0; ci < n_codes && !s_ctx.stop_flag; ci++) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(qr, ci, &code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;

            const char *payload = (const char *)data.payload;
            if (strncmp(payload, "MT:", 3) == 0) {
                strncpy(s_ctx.result_buf, payload,
                        sizeof(s_ctx.result_buf) - 1);
                s_ctx.result_buf[sizeof(s_ctx.result_buf) - 1] = '\0';
                s_ctx.result_ready = true;
                s_ctx.stop_flag    = true;
                ESP_LOGI(TAG, "Matter QR detected: %s", s_ctx.result_buf);
            }
        }
    }

cleanup:
    if (qr) quirc_destroy(qr);
    // Hardware teardown (owned by this task)
    if (s_ctx.cam_handle) {
        esp_cam_ctlr_stop(s_ctx.cam_handle);
        esp_cam_ctlr_disable(s_ctx.cam_handle);
        esp_cam_ctlr_del(s_ctx.cam_handle);
        s_ctx.cam_handle = NULL;
    }
    if (s_ctx.isp_proc) {
        esp_isp_disable(s_ctx.isp_proc);
        esp_isp_del_processor(s_ctx.isp_proc);
        s_ctx.isp_proc = NULL;
    }
    if (s_ctx.sensor) {
        int stream_dis = 0;
        esp_cam_sensor_ioctl(s_ctx.sensor, ESP_CAM_SENSOR_IOC_S_STREAM,
                             &stream_dis);
        s_ctx.sensor = NULL;
    }
    if (s_ctx.sccb_io) {
        esp_sccb_del_i2c_io(s_ctx.sccb_io);
        s_ctx.sccb_io = NULL;
    }

    // Power off camera
    tanmatsu_coprocessor_handle_t coprocessor = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&coprocessor) == ESP_OK
        && coprocessor) {
        tanmatsu_coprocessor_set_camera_gpio0(coprocessor, false);
    }

    // Free buffers
    heap_caps_free(s_ctx.frame_buf);
    s_ctx.frame_buf = NULL;
    heap_caps_free(s_ctx.disp_buf);
    s_ctx.disp_buf = NULL;

    s_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

// ---- Public API ----

esp_err_t camera_qr_start(camera_qr_result_cb_t on_result, lv_obj_t *preview_canvas)
{
    if (s_ctx.task_handle) {
        ESP_LOGW(TAG, "camera_qr already running");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.result_cb     = on_result;
    s_ctx.preview_canvas = preview_canvas;

    // Allocate frame buffer in PSRAM (CAM_W * CAM_H * 2 bytes RGB565)
    size_t frame_size = (size_t)CAM_W * CAM_H * 2;
    s_ctx.frame_buf = (uint16_t *)heap_caps_aligned_calloc(
        64, 1, frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%zu bytes)", frame_size);
        return ESP_ERR_NO_MEM;
    }

    // Allocate display buffer in PSRAM (CAM_QR_DISP_W * CAM_QR_DISP_H * 2 bytes)
    size_t disp_size = (size_t)CAM_QR_DISP_W * CAM_QR_DISP_H * 2;
    s_ctx.disp_buf = (uint16_t *)heap_caps_aligned_calloc(
        64, 1, disp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.disp_buf) {
        ESP_LOGE(TAG, "Failed to allocate display buffer (%zu bytes)", disp_size);
        heap_caps_free(s_ctx.frame_buf);
        s_ctx.frame_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Wire display buffer to the LVGL canvas (called from LVGL context)
    lv_canvas_set_buffer(preview_canvas, s_ctx.disp_buf,
                         CAM_QR_DISP_W, CAM_QR_DISP_H,
                         LV_COLOR_FORMAT_RGB565);

    // Power on camera via coprocessor
    tanmatsu_coprocessor_handle_t coprocessor = NULL;
    esp_err_t err = bsp_tanmatsu_coprocessor_get_handle(&coprocessor);
    if (err != ESP_OK || !coprocessor) {
        ESP_LOGE(TAG, "Failed to get coprocessor handle: %s", esp_err_to_name(err));
        goto fail_free;
    }
    err = tanmatsu_coprocessor_set_camera_gpio0(coprocessor, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power on camera: %s", esp_err_to_name(err));
        goto fail_free;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow camera power rail to stabilise

    // Init camera sensor on BSP I2C bus
    err = sensor_init_on_bsp_bus();
    if (err != ESP_OK) goto fail_camera_off;

    // Init CSI controller: RAW8 in, RGB565 out, 2-lane, 800x640
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .h_res                  = CAM_W,
        .v_res                  = CAM_H,
        .lane_bit_rate_mbps     = CAM_LANE_RATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    err = esp_cam_new_csi_ctlr(&csi_cfg, &s_ctx.cam_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI init failed: %s", esp_err_to_name(err));
        goto fail_sensor;
    }

    esp_cam_ctlr_evt_cbs_t csi_cbs = {
        .on_get_new_trans  = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    esp_cam_ctlr_trans_t init_trans = {
        .buffer = s_ctx.frame_buf,
        .buflen = frame_size,
    };
    err = esp_cam_ctlr_register_event_callbacks(
        s_ctx.cam_handle, &csi_cbs, &init_trans);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI callback register failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }

    err = esp_cam_ctlr_enable(s_ctx.cam_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI enable failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }

    // Init ISP: RAW8 in → RGB565 out
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = CAM_W,
        .v_res                  = CAM_H,
    };
    err = esp_isp_new_processor(&isp_cfg, &s_ctx.isp_proc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISP init failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }

    err = esp_isp_enable(s_ctx.isp_proc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISP enable failed: %s", esp_err_to_name(err));
        goto fail_isp;
    }

    err = esp_cam_ctlr_start(s_ctx.cam_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI start failed: %s", esp_err_to_name(err));
        goto fail_isp;
    }

    // Start LVGL poll timer (100 ms → ~10 Hz canvas refresh + result check)
    s_ctx.poll_timer = lv_timer_create(qr_poll_timer_cb, 100, NULL);

    // Start frame decode task on core 0 (app_main runs on core 1 on P4)
    BaseType_t ret = xTaskCreatePinnedToCore(
        camera_qr_task, "cam_qr",
        16384, NULL, 5, &s_ctx.task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        err = ESP_FAIL;
        goto fail_isp;
    }

    ESP_LOGI(TAG, "Camera QR scanner started");
    return ESP_OK;

fail_isp:
    esp_isp_disable(s_ctx.isp_proc);
    esp_isp_del_processor(s_ctx.isp_proc);
    s_ctx.isp_proc = NULL;
fail_csi:
    esp_cam_ctlr_disable(s_ctx.cam_handle);
    esp_cam_ctlr_del(s_ctx.cam_handle);
    s_ctx.cam_handle = NULL;
fail_sensor:
    if (s_ctx.sensor) {
        int stream_dis = 0;
        esp_cam_sensor_ioctl(s_ctx.sensor, ESP_CAM_SENSOR_IOC_S_STREAM,
                             &stream_dis);
        s_ctx.sensor = NULL;
    }
    if (s_ctx.sccb_io) {
        esp_sccb_del_i2c_io(s_ctx.sccb_io);
        s_ctx.sccb_io = NULL;
    }
fail_camera_off:
    if (coprocessor) {
        tanmatsu_coprocessor_set_camera_gpio0(coprocessor, false);
    }
fail_free:
    heap_caps_free(s_ctx.frame_buf);
    s_ctx.frame_buf = NULL;
    heap_caps_free(s_ctx.disp_buf);
    s_ctx.disp_buf = NULL;
    return err;
}

void camera_qr_stop(void)
{
    s_ctx.result_cb = NULL;
    s_ctx.stop_flag = true;

    if (s_ctx.poll_timer) {
        lv_timer_delete(s_ctx.poll_timer);
        s_ctx.poll_timer = NULL;
    }
    // The camera task detects stop_flag and performs full hardware cleanup
}
