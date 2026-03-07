#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp_lvgl.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

#include "device_manager.h"
#include "matter_commission.h"
#include "matter_device_control.h"
#include "matter_init.h"
#include "ui_screens.h"

static char const TAG[] = "main";

static esp_lcd_panel_handle_t       display_lcd_panel    = NULL;
static esp_lcd_panel_io_handle_t    display_lcd_panel_io = NULL;
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format;
static lcd_rgb_data_endian_t        display_data_endian;
static QueueHandle_t                input_event_queue = NULL;

static void on_matter_event(matter_event_t event) {
    ui_post_event(event);
}

static void on_device_state_changed(uint64_t node_id, bool on_off) {
    lvgl_lock();
    ui_update_device_state(node_id, on_off);
    lvgl_unlock();
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t bsp_config = {};
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_config));

    res = bsp_display_get_panel(&display_lcd_panel);
    ESP_ERROR_CHECK(res);
    bsp_display_get_panel_io(&display_lcd_panel_io);
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    lvgl_init(display_h_res, display_v_res, display_color_format, display_lcd_panel, display_lcd_panel_io,
              input_event_queue);

    ESP_LOGI(TAG, "Initializing device manager");
    device_manager_init();

    ESP_LOGI(TAG, "Initializing UI");
    lvgl_lock();
    ui_screens_init();
    lvgl_unlock();

    // Initialize esp_hosted co-processor transport (must be before any WiFi calls)
    ESP_LOGI(TAG, "Initializing WiFi co-processor");
    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi remote");
    }

    // Initialize WiFi stack and connect to stored network
    // WiFi is managed by the tanmatsu wifi-manager, not by Matter
    // (CONFIG_ENABLE_WIFI_STATION=n prevents Matter from re-initializing WiFi)
    ESP_LOGI(TAG, "Connecting to WiFi");
    wifi_connection_init_stack();
    if (wifi_connect_try_all() == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connection failed, commissioning may not work");
    }

    ESP_LOGI(TAG, "Initializing Matter stack");
    matter_init(on_matter_event);

    matter_device_set_state_cb(on_device_state_changed);
    matter_device_subscribe_all();

    ESP_LOGI(TAG, "Application ready");
}
