#include "camera_qr.h"

#include "esp_cache.h"
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
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp_lvgl.h"

#include "quirc/quirc.h"
#include "quirc/quirc_internal.h"

#include <string.h>

static const char *TAG = "camera_qr";

#define CAM_W              800
#define CAM_H              640
#define CAM_LANE_RATE_MBPS 200
#define CAM_FORMAT_NAME    "MIPI_2lane_24Minput_RAW8_800x640_50fps"
#define CAM_SCCB_FREQ_HZ   10000
#define CAM_BPP            2

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

    uint8_t                *frame_buf;
    size_t                  frame_buf_size;
    uint16_t               *disp_buf;
    SemaphoreHandle_t       frame_done;
    volatile uint32_t       isr_count;
} camera_qr_ctx_t;

static camera_qr_ctx_t s_ctx;

// ---- ISR callback (IRAM) ----

static bool IRAM_ATTR on_trans_finished(
    esp_cam_ctlr_handle_t handle,
    esp_cam_ctlr_trans_t *trans, void *user_data)
{
    camera_qr_ctx_t *ctx = (camera_qr_ctx_t *)user_data;
    ctx->isr_count++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &woken);
    return woken == pdTRUE;
}

// ---- Sensor init on existing BSP I2C bus ----

static esp_err_t sensor_init_on_bsp_bus(void)
{
    ESP_LOGI(TAG, "[DBG] sensor_init_on_bsp_bus: entry");

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_primary_bus_get_handle(&bus),
                        TAG, "Failed to get BSP I2C bus handle");
    ESP_LOGI(TAG, "[DBG] Got I2C bus handle: %p", bus);

    esp_cam_sensor_config_t cam_cfg = {
        .sccb_handle = NULL,
        .reset_pin   = -1,
        .pwdn_pin    = -1,
        .xclk_pin    = -1,
    };

    esp_cam_sensor_device_t *cam = NULL;

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        ESP_LOGI(TAG, "[DBG] Trying sensor detect: addr=0x%02x port=%d",
                 (unsigned)p->sccb_addr, (int)p->port);

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
                ESP_LOGE(TAG, "Detected sensor is not MIPI CSI");
                cam = NULL;
            }
            break;
        }
        esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
        cam_cfg.sccb_handle = NULL;
    }

    if (!cam) {
        ESP_LOGE(TAG, "No MIPI CSI camera sensor detected");
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_sensor_format_array_t fmt_array = {};
    esp_cam_sensor_query_format(cam, &fmt_array);
    ESP_LOGI(TAG, "[DBG] Sensor has %d formats", (int)fmt_array.count);

    const esp_cam_sensor_format_t *target_fmt = NULL;
    for (int i = 0; i < (int)fmt_array.count; i++) {
        ESP_LOGI(TAG, "[DBG]   format[%d]: '%s'", i,
                 fmt_array.format_array[i].name);
        if (strcmp(fmt_array.format_array[i].name, CAM_FORMAT_NAME) == 0) {
            target_fmt = &fmt_array.format_array[i];
        }
    }
    if (!target_fmt) {
        ESP_LOGE(TAG, "Format '%s' not found", CAM_FORMAT_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(
        esp_cam_sensor_set_format(cam, target_fmt),
        TAG, "Failed to set camera format");

    s_ctx.sccb_io = cam_cfg.sccb_handle;
    s_ctx.sensor  = cam;
    ESP_LOGI(TAG, "[DBG] Sensor configured (stream NOT started yet)");
    return ESP_OK;
}

// ---- Grayscale conversion ----

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
    if (s_ctx.preview_canvas) {
        lv_obj_invalidate(s_ctx.preview_canvas);
    }

    if (s_ctx.result_ready && s_ctx.result_cb) {
        camera_qr_result_cb_t cb = s_ctx.result_cb;
        s_ctx.result_cb  = NULL;
        s_ctx.poll_timer = NULL;
        lv_timer_delete(timer);
        s_ctx.stop_flag = true;
        cb(s_ctx.result_buf);
    }
}

// ---- Camera receive + QR decode task ----

static void camera_qr_task(void *arg)
{
    ESP_LOGI(TAG, "[DBG] Task started on core %d", (int)xPortGetCoreID());

    struct quirc *qr = quirc_new();
    if (!qr) {
        ESP_LOGE(TAG, "Failed to allocate quirc decoder");
        goto cleanup;
    }
    if (quirc_resize(qr, CAM_W, CAM_H) != 0) {
        ESP_LOGE(TAG, "Failed to resize quirc decoder");
        goto cleanup;
    }
    ESP_LOGI(TAG, "[DBG] quirc ready, frame_buf=%p disp_buf=%p",
             s_ctx.frame_buf, s_ctx.disp_buf);

    // Build the transaction struct for esp_cam_ctlr_receive
    esp_cam_ctlr_trans_t trans = {
        .buffer = s_ctx.frame_buf,
        .buflen = s_ctx.frame_buf_size,
    };

    uint32_t frame_count = 0;
    while (!s_ctx.stop_flag) {
        // Queue our buffer for the next frame via the driver's receive API.
        // This blocks until the queue has space (queue_items=1).
        esp_err_t recv_err = esp_cam_ctlr_receive(
            s_ctx.cam_handle, &trans, pdMS_TO_TICKS(2000));
        if (recv_err != ESP_OK) {
            ESP_LOGW(TAG, "[DBG] receive returned %s, isr_count=%lu",
                     esp_err_to_name(recv_err),
                     (unsigned long)s_ctx.isr_count);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Wait for on_trans_finished ISR to signal our buffer is ready
        if (xSemaphoreTake(s_ctx.frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "[DBG] No frame in 2s! isr=%lu hwm=%lu",
                     (unsigned long)s_ctx.isr_count,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            continue;
        }

        frame_count++;

        // Cache sync — CPU must see fresh DMA data
        esp_cache_msync(s_ctx.frame_buf, s_ctx.frame_buf_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        const uint16_t *frame = (const uint16_t *)s_ctx.frame_buf;

        if (frame_count <= 5 || (frame_count % 100) == 0) {
            ESP_LOGI(TAG, "[DBG] Frame %lu isr=%lu hwm=%lu px=[0x%04x "
                     "0x%04x 0x%04x 0x%04x] mid=[0x%04x 0x%04x]",
                     (unsigned long)frame_count,
                     (unsigned long)s_ctx.isr_count,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                     frame[0], frame[1], frame[2], frame[3],
                     frame[(CAM_W * CAM_H) / 2],
                     frame[(CAM_W * CAM_H) / 2 + 1]);
        }

        // Copy center crop to display buffer
        const uint16_t *src = frame + CAM_CROP_ROW_OFFSET * CAM_W;
        memcpy(s_ctx.disp_buf, src,
               (size_t)CAM_QR_DISP_W * CAM_QR_DISP_H * CAM_BPP);

        if (s_ctx.stop_flag) break;

        // Convert full frame to grayscale for quirc
        uint8_t *gray = quirc_begin(qr, NULL, NULL);
        for (int i = 0; i < CAM_W * CAM_H; i++) {
            gray[i] = rgb565_to_gray(frame[i]);
        }

        // Contrast stretch: map [min..max] → [0..255] to maximize
        // dynamic range for quirc's adaptive threshold. Helps with
        // low-contrast / washed-out images from the camera.
        {
            uint8_t g_min = 255, g_max = 0;
            int total = CAM_W * CAM_H;
            for (int i = 0; i < total; i++) {
                if (gray[i] < g_min) g_min = gray[i];
                if (gray[i] > g_max) g_max = gray[i];
            }
            uint8_t range = g_max - g_min;
            if (range > 0 && range < 200) {
                for (int i = 0; i < total; i++) {
                    gray[i] = (uint8_t)(
                        ((uint16_t)(gray[i] - g_min) * 255) / range);
                }
            }
        }

        // Debug: grayscale image statistics (every 50 frames)
        if ((frame_count % 50) == 1) {
            uint8_t g_min = 255, g_max = 0;
            uint32_t g_sum = 0;
            uint32_t hist_lo = 0, hist_mid = 0, hist_hi = 0;
            int total = CAM_W * CAM_H;
            for (int i = 0; i < total; i++) {
                uint8_t g = gray[i];
                if (g < g_min) g_min = g;
                if (g > g_max) g_max = g;
                g_sum += g;
                if (g < 85) hist_lo++;
                else if (g < 170) hist_mid++;
                else hist_hi++;
            }
            uint8_t g_mean = (uint8_t)(g_sum / total);
            int cx = CAM_W / 2, cy = CAM_H / 2;
            ESP_LOGI(TAG, "[DBG] Gray stats frame %lu (%dx%d): "
                     "min=%u max=%u mean=%u contrast=%u "
                     "hist[0-84]=%lu [85-169]=%lu [170-255]=%lu",
                     (unsigned long)frame_count, CAM_W, CAM_H,
                     g_min, g_max, g_mean,
                     (unsigned)(g_max - g_min),
                     (unsigned long)hist_lo,
                     (unsigned long)hist_mid,
                     (unsigned long)hist_hi);
            ESP_LOGI(TAG, "[DBG] Center pixels: "
                     "[%u %u %u %u %u] "
                     "[%u %u %u %u %u] "
                     "[%u %u %u %u %u]",
                     gray[(cy-1)*CAM_W+cx-2],
                     gray[(cy-1)*CAM_W+cx-1],
                     gray[(cy-1)*CAM_W+cx],
                     gray[(cy-1)*CAM_W+cx+1],
                     gray[(cy-1)*CAM_W+cx+2],
                     gray[cy*CAM_W+cx-2],
                     gray[cy*CAM_W+cx-1],
                     gray[cy*CAM_W+cx],
                     gray[cy*CAM_W+cx+1],
                     gray[cy*CAM_W+cx+2],
                     gray[(cy+1)*CAM_W+cx-2],
                     gray[(cy+1)*CAM_W+cx-1],
                     gray[(cy+1)*CAM_W+cx],
                     gray[(cy+1)*CAM_W+cx+1],
                     gray[(cy+1)*CAM_W+cx+2]);
        }

        quirc_end(qr);

        int n_codes = quirc_count(qr);
        if (n_codes > 0 || (frame_count % 50) == 1) {
            ESP_LOGI(TAG, "[DBG] quirc frame %lu: codes=%d "
                     "regions=%d capstones=%d grids=%d",
                     (unsigned long)frame_count, n_codes,
                     qr->num_regions,
                     qr->num_capstones,
                     qr->num_grids);
        }

        for (int ci = 0; ci < n_codes && !s_ctx.stop_flag; ci++) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(qr, ci, &code);
            quirc_decode_error_t dec_err = quirc_decode(&code, &data);
            if (dec_err != QUIRC_SUCCESS) {
                ESP_LOGW(TAG, "[DBG] quirc decode[%d] failed: %s "
                         "(size=%d ver=%d)",
                         ci, quirc_strerror(dec_err),
                         code.size, data.version);
                continue;
            }

            const char *payload = (const char *)data.payload;
            ESP_LOGI(TAG, "[DBG] QR payload: %.64s", payload);
            if (strncmp(payload, "MT:", 3) == 0) {
                strncpy(s_ctx.result_buf, payload,
                        sizeof(s_ctx.result_buf) - 1);
                s_ctx.result_buf[sizeof(s_ctx.result_buf) - 1] = '\0';
                s_ctx.result_ready = true;
                s_ctx.stop_flag    = true;
                ESP_LOGI(TAG, "Matter QR detected: %s", s_ctx.result_buf);
            }
        }

        // Yield to let other tasks (incl. idle/watchdog) run
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "[DBG] Frame loop exited, total=%lu",
             (unsigned long)frame_count);

cleanup:
    if (qr) quirc_destroy(qr);

    ESP_LOGI(TAG, "[DBG] Stopping CSI");
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

    ESP_LOGI(TAG, "[DBG] Powering off camera");
    tanmatsu_coprocessor_handle_t coprocessor = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&coprocessor) == ESP_OK
        && coprocessor) {
        tanmatsu_coprocessor_set_camera_gpio0(coprocessor, false);
    }

    heap_caps_free(s_ctx.frame_buf);
    s_ctx.frame_buf = NULL;
    heap_caps_free(s_ctx.disp_buf);
    s_ctx.disp_buf = NULL;
    if (s_ctx.frame_done) {
        vSemaphoreDelete(s_ctx.frame_done);
        s_ctx.frame_done = NULL;
    }

    s_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

// ---- Public API ----

esp_err_t camera_qr_start(camera_qr_result_cb_t on_result,
                           lv_obj_t *preview_canvas)
{
    ESP_LOGI(TAG, "[DBG] camera_qr_start: entry");

    if (s_ctx.task_handle) {
        ESP_LOGW(TAG, "camera_qr already running");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.result_cb      = on_result;
    s_ctx.preview_canvas = preview_canvas;
    s_ctx.frame_done     = xSemaphoreCreateBinary();
    if (!s_ctx.frame_done) return ESP_ERR_NO_MEM;

    // Allocate DMA frame buffer (cache-aligned, PSRAM)
    s_ctx.frame_buf_size = (size_t)CAM_W * CAM_H * CAM_BPP;
    size_t alignment = 0;
    esp_err_t err = esp_cache_get_alignment(0, &alignment);
    if (err != ESP_OK || alignment == 0) alignment = 64;

    ESP_LOGI(TAG, "[DBG] Alloc frame_buf: %zu bytes, align=%zu, "
             "free PSRAM=%lu",
             s_ctx.frame_buf_size, alignment,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_ctx.frame_buf = (uint8_t *)heap_caps_aligned_calloc(
        alignment, 1, s_ctx.frame_buf_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto fail_free;
    }
    ESP_LOGI(TAG, "[DBG] frame_buf=%p", s_ctx.frame_buf);

    // Display buffer
    size_t disp_size = (size_t)CAM_QR_DISP_W * CAM_QR_DISP_H * CAM_BPP;
    s_ctx.disp_buf = (uint16_t *)heap_caps_aligned_calloc(
        alignment, 1, disp_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.disp_buf) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        goto fail_free;
    }
    ESP_LOGI(TAG, "[DBG] disp_buf=%p (%zu bytes)", s_ctx.disp_buf, disp_size);

    lv_canvas_set_buffer(preview_canvas, s_ctx.disp_buf,
                         CAM_QR_DISP_W, CAM_QR_DISP_H,
                         LV_COLOR_FORMAT_RGB565);

    // Power on camera
    tanmatsu_coprocessor_handle_t coprocessor = NULL;
    err = bsp_tanmatsu_coprocessor_get_handle(&coprocessor);
    if (err != ESP_OK || !coprocessor) {
        ESP_LOGE(TAG, "Failed to get coprocessor handle");
        goto fail_free;
    }
    ESP_LOGI(TAG, "[DBG] Powering on camera...");
    err = tanmatsu_coprocessor_set_camera_gpio0(coprocessor, true);
    if (err != ESP_OK) goto fail_free;
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms for power rail + sensor startup

    // Init sensor (detect + configure format, but do NOT start stream yet)
    err = sensor_init_on_bsp_bus();
    if (err != ESP_OK) goto fail_camera_off;

    // CSI controller — keep backup buffer so the ISR can silently
    // drop frames when we haven't queued our buffer yet (50fps sensor
    // vs ~5fps processing rate).
    ESP_LOGI(TAG, "[DBG] Creating CSI controller...");
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
    ESP_LOGI(TAG, "[DBG] CSI created: %p", s_ctx.cam_handle);

    // Register only on_trans_finished — no on_get_new_trans.
    // Buffer is provided via esp_cam_ctlr_receive (queue-based).
    // This avoids the ISR doing cache sync on every 50fps frame.
    esp_cam_ctlr_evt_cbs_t csi_cbs = {
        .on_trans_finished = on_trans_finished,
    };
    err = esp_cam_ctlr_register_event_callbacks(
        s_ctx.cam_handle, &csi_cbs, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI callback register failed: %s",
                 esp_err_to_name(err));
        goto fail_csi;
    }

    err = esp_cam_ctlr_enable(s_ctx.cam_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI enable failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }
    ESP_LOGI(TAG, "[DBG] CSI enabled");

    // ISP: RAW8 -> RGB565
    ESP_LOGI(TAG, "[DBG] Creating ISP processor...");
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
    ESP_LOGI(TAG, "[DBG] ISP enabled");

    // Now start sensor stream — AFTER CSI+ISP are ready
    ESP_LOGI(TAG, "[DBG] Starting sensor stream...");
    int stream_en = 1;
    err = esp_cam_sensor_ioctl(s_ctx.sensor, ESP_CAM_SENSOR_IOC_S_STREAM,
                               &stream_en);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sensor stream: %s",
                 esp_err_to_name(err));
        goto fail_isp;
    }

    // Start CSI — first frame goes to backup buffer (no callback).
    // The task will queue our buffer via esp_cam_ctlr_receive.
    ESP_LOGI(TAG, "[DBG] Starting CSI...");
    err = esp_cam_ctlr_start(s_ctx.cam_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI start failed: %s", esp_err_to_name(err));
        goto fail_isp;
    }
    ESP_LOGI(TAG, "[DBG] CSI started — DMA active");

    // LVGL poll timer
    s_ctx.poll_timer = lv_timer_create(qr_poll_timer_cb, 200, NULL);

    // Start task on core 1 to avoid watchdog contention on core 0
    BaseType_t ret = xTaskCreatePinnedToCore(
        camera_qr_task, "cam_qr",
        32768, NULL, 5, &s_ctx.task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        err = ESP_FAIL;
        goto fail_isp;
    }

    ESP_LOGI(TAG, "[DBG] Camera QR scanner started, task=%p",
             s_ctx.task_handle);
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
    if (s_ctx.frame_done) {
        vSemaphoreDelete(s_ctx.frame_done);
        s_ctx.frame_done = NULL;
    }
    return err;
}

void camera_qr_stop(void)
{
    ESP_LOGI(TAG, "[DBG] camera_qr_stop called");
    s_ctx.result_cb = NULL;
    s_ctx.stop_flag = true;

    if (s_ctx.poll_timer) {
        lv_timer_delete(s_ctx.poll_timer);
        s_ctx.poll_timer = NULL;
    }
}
