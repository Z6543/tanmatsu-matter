// SPDX-FileCopyrightText: 2025 Orange-Murker
// SPDX-License-Identifier: MIT

#include "bsp_lvgl.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "core/lv_group.h"
#include "display/lv_display.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/lcd_types.h"
#include "indev/lv_indev.h"
#include "lv_conf_internal.h"
#include "lv_init.h"
#include "misc/lv_area.h"
#include "misc/lv_color.h"
#include "misc/lv_timer.h"
#include "misc/lv_types.h"
#include "portmacro.h"

#ifdef CONFIG_BSP_LVGL_DSI_DISPLAY
#include "esp_lcd_mipi_dsi.h"
#endif

// Use the PPA on the P4 and software rotation on others
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "driver/ppa.h"
#include "hal/ppa_types.h"
static ppa_client_handle_t ppa_srm_handle;
#else
#include "draw/lv_draw_buf.h"
#include "draw/sw/lv_draw_sw_utils.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/lock.h>
#include <unistd.h>

static char const TAG[] = "bsp-lvgl";

#define LVGL_TICK_PERIOD_MS 2

#define LVGL_TASK_STACK_SIZE (16 * 1024)
#define LVGL_TE_TASK_STACK_SIZE (4 * 1024)

// LVGL library is not thread-safe, this example will call LVGL APIs from different tasks, so use a mutex to protect it
static _lock_t lvgl_api_lock;

static uint8_t*  rotation_buffer = NULL;
static size_t    draw_buffer_sz;
static lv_area_t rotated_area;

static esp_lcd_panel_handle_t panel_handle;

static QueueHandle_t input_queue = NULL;

static lv_display_t* display = NULL;

static SemaphoreHandle_t tearing_effect_semaphore = NULL;
static SemaphoreHandle_t ppa_done_semaphore = NULL;

void lvgl_lock() {
    _lock_acquire(&lvgl_api_lock);
}

void lvgl_unlock() {
    _lock_release(&lvgl_api_lock);
}

lv_display_t* lvgl_get_display(void) {
    return display;
}

static lv_display_rotation_t bsp_display_rotation_to_lvgl(bsp_display_rotation_t rotation) {
    switch (rotation) {
        case BSP_DISPLAY_ROTATION_0:
            return LV_DISPLAY_ROTATION_0;
        case BSP_DISPLAY_ROTATION_90:
            return LV_DISPLAY_ROTATION_90;
        case BSP_DISPLAY_ROTATION_180:
            return LV_DISPLAY_ROTATION_180;
        case BSP_DISPLAY_ROTATION_270:
            return LV_DISPLAY_ROTATION_270;
    }

    return BSP_DISPLAY_ROTATION_0;
}

lv_display_rotation_t lvgl_get_default_rotation() {
    return bsp_display_rotation_to_lvgl(bsp_display_get_default_rotation());
}

lv_display_rotation_t lvgl_rotation_relative_to_default(lv_display_rotation_t rotation) {
    lv_display_rotation_t default_rotation = lvgl_get_default_rotation();

    rotation = (rotation + default_rotation) % 4;

    return rotation;
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
bool rotation_done_cb(ppa_client_handle_t ppa_handle, ppa_event_data_t* ppa_event_data, void* user_data) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(ppa_done_semaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    return false;
}
#endif

void lvgl_flush_cb(lv_display_t* disp, lv_area_t const* area, uint8_t* px_map) {
    lv_color_format_t     cf       = lv_display_get_color_format(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);

    rotated_area = *area;
    lv_display_rotate_area(lv_display_get_default(), &rotated_area);

#ifdef CONFIG_IDF_TARGET_ESP32P4
    int32_t rotated_w = lv_area_get_width(&rotated_area);
    int32_t rotated_h = lv_area_get_height(&rotated_area);

    ppa_srm_color_mode_t ppa_srm_colour = PPA_SRM_COLOR_MODE_RGB565;
    switch (cf) {
        case LV_COLOR_FORMAT_RGB565:
            ppa_srm_colour = PPA_SRM_COLOR_MODE_RGB565;
            break;
        case LV_COLOR_FORMAT_RGB888:
            ppa_srm_colour = PPA_SRM_COLOR_MODE_RGB888;
            break;
        case LV_COLOR_FORMAT_ARGB8888:
            ppa_srm_colour = PPA_SRM_COLOR_MODE_ARGB8888;
            break;
        case LV_COLOR_FORMAT_I420:
            ppa_srm_colour = PPA_SRM_COLOR_MODE_YUV420;
            break;
        case LV_COLOR_FORMAT_I444:
            ppa_srm_colour = PPA_SRM_COLOR_MODE_YUV444;
            break;
        default:
            ESP_LOGE(TAG, "Trying to render a colour format unsupported by the PPA");
            return;
            break;
    }

    ppa_srm_rotation_angle_t ppa_srm_angle = PPA_SRM_ROTATION_ANGLE_0;
    switch (rotation) {
        case LV_DISPLAY_ROTATION_0:
            ppa_srm_angle = PPA_SRM_ROTATION_ANGLE_0;
            break;
        case LV_DISPLAY_ROTATION_90:
            ppa_srm_angle = PPA_SRM_ROTATION_ANGLE_90;
            break;
        case LV_DISPLAY_ROTATION_180:
            ppa_srm_angle = PPA_SRM_ROTATION_ANGLE_180;
            break;
        case LV_DISPLAY_ROTATION_270:
            ppa_srm_angle = PPA_SRM_ROTATION_ANGLE_270;
            break;
    }

    ppa_srm_oper_config_t ppa_srm_config = {
        .in =
            {
                .buffer         = px_map,
                .pic_w          = w,
                .pic_h          = h,
                .block_w        = w,
                .block_h        = h,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm         = ppa_srm_colour,
            },
        .out =
            {
                .buffer         = rotation_buffer,
                .buffer_size    = draw_buffer_sz,
                .pic_w          = rotated_w,
                .pic_h          = rotated_h,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm         = ppa_srm_colour,
            },
        .rotation_angle = ppa_srm_angle,
        .scale_x        = 1,
        .scale_y        = 1,
        .mirror_x       = false,
        .mirror_y       = false,
        .rgb_swap       = false,
#ifdef CONFIG_BSP_LVGL_BIG_ENDIAN_COLOUR
        .byte_swap = true,
#else
        .byte_swap = false,
#endif
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };

    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_srm_handle, &ppa_srm_config));

    // Wait for PPA to finish, then draw from task context (not ISR)
    xSemaphoreTake(ppa_done_semaphore, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(panel_handle, rotated_area.x1, rotated_area.y1,
                              rotated_area.x2 + 1, rotated_area.y2 + 1,
                              rotation_buffer);
#else
    uint32_t w_stride = lv_draw_buf_width_to_stride(w, cf);
    uint32_t h_stride = lv_draw_buf_width_to_stride(h, cf);

    if (rotation != LV_DISPLAY_ROTATION_0) {
        switch (rotation) {
            case LV_DISPLAY_ROTATION_0:
                break;
            case LV_DISPLAY_ROTATION_90:
                lv_draw_sw_rotate(px_map, rotation_buffer, w, h, w_stride, h_stride, rotation, cf);
                break;
            case LV_DISPLAY_ROTATION_180:
                lv_draw_sw_rotate(px_map, rotation_buffer, w, h, w_stride, w_stride, rotation, cf);
                break;
            case LV_DISPLAY_ROTATION_270:
                lv_draw_sw_rotate(px_map, rotation_buffer, w, h, w_stride, h_stride, rotation, cf);
                break;
        }

        px_map = rotation_buffer;
    }

    int offsetx1 = rotated_area.x1;
    int offsetx2 = rotated_area.x2;
    int offsety1 = rotated_area.y1;
    int offsety2 = rotated_area.y2;

#ifdef CONFIG_BSP_LVGL_BIG_ENDIAN_COLOUR
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(&rotated_area));
#endif

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
#endif
}

static void increase_lvgl_tick(void* arg) {
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void* arg) {
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        lvgl_lock();
        time_till_next_ms = lv_timer_handler();
        lvgl_unlock();

        if (time_till_next_ms == LV_NO_TIMER_READY) {
            time_till_next_ms = LV_DEF_REFR_PERIOD;
        }

        // in case of task watch dog timeout, set the minimal delay to 10ms
        if (time_till_next_ms < 10) {
            time_till_next_ms = 10;
        }
        usleep(1000 * time_till_next_ms);
    }
}

#ifdef CONFIG_BSP_LVGL_DSI_DISPLAY

static bool IRAM_ATTR notify_lvgl_flush_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                                              void* user_ctx) {
    lv_display_t* disp = (lv_display_t*)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

#else

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t* edata,
                                    void* user_ctx) {
    lv_display_t* disp = (lv_display_t*)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

#endif

static void tearing_effect_flush_task(void* arg) {
    while (1) {
        xSemaphoreTake(tearing_effect_semaphore, portMAX_DELAY);
        lv_display_t* disp = (lv_display_t*)arg;
        lv_display_flush_ready(disp);
    }
}

static void read_keyboard(lv_indev_t* indev, lv_indev_data_t* data) {
    bsp_input_event_t event;

    UBaseType_t messages_waiting = uxQueueMessagesWaiting(input_queue);

    if (messages_waiting > 1) {
        data->continue_reading = true;
    }

    if (messages_waiting >= 1) {
        if (xQueueReceive(input_queue, &event, 0) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    switch (event.args_navigation.key) {
                        case BSP_INPUT_NAVIGATION_KEY_UP:
                            data->key   = LV_KEY_UP;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_DOWN:
                            data->key   = LV_KEY_DOWN;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_LEFT:
                            data->key   = LV_KEY_LEFT;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                            data->key   = LV_KEY_RIGHT;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_RETURN:
                            data->key   = LV_KEY_ENTER;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A:
                            data->key   = LV_KEY_ENTER;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_ESC:
                            data->key   = LV_KEY_ESC;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_GAMEPAD_B:
                            data->key   = LV_KEY_ESC;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_TAB:
                            if (event.args_navigation.modifiers & BSP_INPUT_MODIFIER_SHIFT) {
                                data->key = LV_KEY_PREV;
                            } else {
                                data->key = LV_KEY_NEXT;
                            }
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                            data->key   = LV_KEY_BACKSPACE;
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F1:
                            data->key   = LV_KEY_HOME;  // F1 shortcut
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F2:
                            data->key   = LV_KEY_END;   // F2 shortcut
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_F3:
                            data->key   = BSP_KEY_F3;  // screenshot
                            data->state = event.args_navigation.state;
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
                        case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
                        case BSP_INPUT_NAVIGATION_KEY_SPACE_R:
                        case BSP_INPUT_NAVIGATION_KEY_F4:
                        case BSP_INPUT_NAVIGATION_KEY_F5:
                        case BSP_INPUT_NAVIGATION_KEY_F6:
                        case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                        case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                        case BSP_INPUT_NAVIGATION_KEY_SUPER:
                            break;  // ignored, no warning
                        default:
                            ESP_LOGW(TAG, "Unhandled navigation event");
                            break;
                    }
                    break;
                case INPUT_EVENT_TYPE_KEYBOARD:
                    data->state = LV_INDEV_STATE_PRESSED;
                    data->key   = *((uint32_t*)event.args_keyboard.utf8);
                    break;
                case INPUT_EVENT_TYPE_ACTION:
                    break;
                case INPUT_EVENT_TYPE_SCANCODE:
                    break;
                default:
                    ESP_LOGW(TAG, "Unhandled input event of type %u", event.type);
                    break;
            }
        }
    }
}

void lvgl_init(int32_t hres, int32_t vres, lcd_color_rgb_pixel_format_t colour_fmt,
               esp_lcd_panel_handle_t lcd_panel_handle, esp_lcd_panel_io_handle_t lcd_panel_io_handle,
               QueueHandle_t input_event_queue) {
    panel_handle = lcd_panel_handle;
    input_queue  = input_event_queue;

    lv_init();

    display = lv_display_create(hres, vres);

    ESP_LOGI(TAG, "Display %lux%lu", hres, vres);
    switch (colour_fmt) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
            ESP_LOGI(TAG, "Using RGB565");
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
            ESP_LOGI(TAG, "Using RGB888");
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB666:
            ESP_LOGE(TAG, "RGB666 not supported");
            break;
    }

    lv_display_set_rotation(display, lvgl_get_default_rotation());

    uint32_t cache_line_size = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);

    draw_buffer_sz = hres * (vres / 10) * sizeof(lv_color_t);
    void* buf1     = heap_caps_malloc(draw_buffer_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(buf1);
    void* buf2 = heap_caps_malloc(draw_buffer_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(buf2);
    rotation_buffer = heap_caps_aligned_calloc(cache_line_size, 1, draw_buffer_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(rotation_buffer);

#ifdef CONFIG_IDF_TARGET_ESP32P4
    ppa_client_config_t ppa_srm_config = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 5,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ppa_done_semaphore = xSemaphoreCreateBinary();

    ppa_event_callbacks_t ppa_srm_callbacks = {
        .on_trans_done = rotation_done_cb,
    };

    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(ppa_srm_handle, &ppa_srm_callbacks));
#endif

    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // Set the callback which can copy the rendered image to an area of the display
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    // Check if the tearing effect is available
    if (bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING) == ESP_OK &&
        bsp_display_get_tearing_effect_semaphore(&tearing_effect_semaphore) == ESP_OK) {
        xTaskCreate(tearing_effect_flush_task, "LVGL TE Flush", LVGL_TE_TASK_STACK_SIZE, display, 5, NULL);
    } else {
#ifdef CONFIG_BSP_LVGL_DSI_DISPLAY
        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_color_trans_done = notify_lvgl_flush_ready,
        };

        ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel_handle, &cbs, display));
#else
        if (lcd_panel_io_handle) {
            esp_lcd_panel_io_callbacks_t cbs = {
                .on_color_trans_done = notify_lvgl_flush_ready,
            };

            ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(lcd_panel_io_handle, &cbs, display));
        }
#endif
    }

    esp_timer_create_args_t const lvgl_tick_timer_args = {.callback = &increase_lvgl_tick, .name = "lvgl_tick"};
    esp_timer_handle_t            lvgl_tick_timer      = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, 2, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_keyboard);

    lv_group_t* group = lv_group_create();
    lv_indev_set_group(indev, group);
    lv_group_set_default(group);
}
