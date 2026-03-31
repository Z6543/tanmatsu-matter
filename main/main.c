#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp_lvgl.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

#include "device_manager.h"
#include "zh4ck_w5500_ethernet.h"
#include "zh4ck_usb_keyboard.h"
#include "matter_commission.h"
#include "matter_device_control.h"
#include "matter_init.h"
#include "sdcard.h"
#include "ui_screens.h"

static char const TAG[] = "main";

// esp-hosted-tanmatsu calls hosted_reset_slave_callback() to reset the C6 radio.
// tanmatsu-wifi provides the implementation as hosted_sdio_reset_slave_callback().
// Bridge the naming gap here (user-code callback per the Kconfig documentation).
extern esp_err_t hosted_sdio_reset_slave_callback(void);
int hosted_reset_slave_callback(void) { return (int)hosted_sdio_reset_slave_callback(); }

static esp_lcd_panel_handle_t       display_lcd_panel    = NULL;
static esp_lcd_panel_io_handle_t    display_lcd_panel_io = NULL;
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format;
static lcd_rgb_data_endian_t        display_data_endian;
static QueueHandle_t                input_event_queue = NULL;

static interface_mode_t s_mode = INTERFACE_MODE_NONE;

static void start_thread_br_and_subscribe(void) {
    if (!matter_thread_available()) return;
    esp_err_t err = matter_start_thread_br();
    if (err == ESP_OK) {
        matter_event_t br_ev = {};
        br_ev.type = MATTER_EVENT_THREAD_BR_STARTED;
        ui_post_event(br_ev);
        matter_device_subscribe_thread_delayed();
    }
}

static void on_matter_event(matter_event_t event) {
    ui_post_event(event);
    if (event.type == MATTER_EVENT_STACK_READY) {
        if (s_mode == INTERFACE_MODE_WIFI ||
            s_mode == INTERFACE_MODE_ETHERNET) {
            matter_device_subscribe_wifi();
        } else {
            // Thread mode: start border router, subscribe later
            start_thread_br_and_subscribe();
        }

        if (device_manager_count() > 0) {
            matter_device_start_reconnect_timer();
        }
    }
}

static void on_device_state_changed(uint64_t node_id) {
    lvgl_lock();
    ui_update_device_state(node_id);
    lvgl_unlock();
}

static void on_wifi_got_ip(
    void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    ESP_LOGI(TAG, "WiFi reconnected");

    if (s_mode == INTERFACE_MODE_WIFI) {
        ESP_LOGI(TAG, "Re-subscribing to WiFi devices");
        matter_device_subscribe_wifi();
    } else {
        // Thread mode: WiFi provides the backbone
        start_thread_br_and_subscribe();
    }

    if (device_manager_count() > 0) {
        matter_device_start_reconnect_timer();
    }
}

static void on_eth_got_ip(
    void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    ESP_LOGI(TAG, "Ethernet got IP, subscribing to devices");
    matter_device_subscribe_wifi();
    if (device_manager_count() > 0) {
        matter_device_start_reconnect_timer();
    }
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES ||
        res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t bsp_config = {};
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_config));

    res = bsp_display_get_panel(&display_lcd_panel);
    ESP_ERROR_CHECK(res);
    bsp_display_get_panel_io(&display_lcd_panel_io);
    res = bsp_display_get_parameters(
        &display_h_res, &display_v_res,
        &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    lvgl_init(display_h_res, display_v_res, display_color_format,
              display_lcd_panel, display_lcd_panel_io);
    zh4ck_usb_keyboard_init(input_event_queue);

    // Mount SD card (optional, for screenshots)
    esp_err_t sd_res = sdcard_init();
    if (sd_res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available: %s",
                 esp_err_to_name(sd_res));
    }

    ESP_LOGI(TAG, "Initializing device manager");
    device_manager_init();

    // Initialize UI and determine interface mode.
    // If no mode is persisted, shows selection screen and blocks
    // until the user picks WiFi or Thread.
    ESP_LOGI(TAG, "Initializing UI");
    lvgl_lock();
    ui_screens_init();
    lvgl_unlock();

    ESP_LOGI(TAG, "Waiting for interface mode selection...");
    s_mode = ui_wait_for_mode_selection();

    ESP_LOGI(TAG, "Interface mode: %s",
             s_mode == INTERFACE_MODE_WIFI ? "WiFi" :
             s_mode == INTERFACE_MODE_THREAD ? "Thread" : "Ethernet");

    if (s_mode == INTERFACE_MODE_ETHERNET) {
        // Ethernet-only: no WiFi co-processor, no Thread
        // Need netif and event loop (normally set up by WiFi stack)
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        ESP_LOGI(TAG, "Initializing W5500 Ethernet");
        esp_err_t eth_res = ethernet_init();
        if (eth_res != ESP_OK) {
            ESP_LOGE(TAG, "Ethernet init failed: %s",
                     esp_err_to_name(eth_res));
        }
        ESP_LOGI(TAG, "Using Ethernet — skipping WiFi/Thread");
        if (!ethernet_connected()) {
            ESP_LOGW(TAG, "Ethernet not connected yet, "
                     "commissioning may not work until link is up");
        }
    } else {
        // Initialize esp_hosted co-processor transport
        // (needed for both WiFi and Thread — Thread uses WiFi as
        // backbone for the border router)
        ESP_LOGI(TAG, "Initializing WiFi co-processor");
        if (wifi_remote_initialize() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi remote");
        }

        // Initialize WiFi stack and connect
        ESP_LOGI(TAG, "Connecting to WiFi");
        wifi_connection_init_stack();
        if (wifi_connect_try_all() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected");
        } else {
            if (s_mode == INTERFACE_MODE_WIFI) {
                ESP_LOGW(TAG, "WiFi connection failed, "
                         "commissioning may not work");
            } else {
                ESP_LOGW(TAG, "WiFi connection failed, "
                         "Thread border router needs WiFi backbone");
            }
        }
    }

    bool use_thread = (s_mode == INTERFACE_MODE_THREAD);
    ESP_LOGI(TAG, "Initializing Matter stack (thread=%s)",
             use_thread ? "yes" : "no");
    matter_init(on_matter_event, use_thread);

    matter_device_set_state_cb(on_device_state_changed);

    if (s_mode == INTERFACE_MODE_ETHERNET) {
        esp_event_handler_register(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, on_eth_got_ip, NULL);
    } else {
        esp_event_handler_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_got_ip, NULL);
    }

    // Build app screens now that mode is known
    lvgl_lock();
    ui_build_app_screens();
    lvgl_unlock();

    ESP_LOGI(TAG, "Application ready");
}
