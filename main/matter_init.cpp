#include "matter_init.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <platform/PlatformManager.h>

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <esp_ot_config.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

static const char *TAG = "matter_init";
static matter_event_cb_t s_event_cb = nullptr;

extern "C" void matter_post_event(matter_event_t event) {
    if (s_event_cb) s_event_cb(event);
}

static void on_pase_complete(CHIP_ERROR err) {
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = (err == CHIP_NO_ERROR) ? MATTER_EVENT_PASE_SUCCESS : MATTER_EVENT_PASE_FAILED;
    s_event_cb(ev);
}

static void on_commissioning_success(chip::ScopedNodeId peer_id) {
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_COMMISSION_SUCCESS;
    ev.node_id = peer_id.GetNodeId();
    ESP_LOGI(TAG, "Commissioning success: node_id=0x%llx", (unsigned long long)ev.node_id);
    s_event_cb(ev);
}

static void on_commissioning_failure(
    chip::ScopedNodeId peer_id, CHIP_ERROR error,
    chip::Controller::CommissioningStage stage,
    std::optional<chip::Credentials::AttestationVerificationResult> additional_err_info) {
    ESP_LOGE(TAG, "Commissioning failed: node_id=0x%llx", (unsigned long long)peer_id.GetNodeId());
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_COMMISSION_FAILED;
    ev.node_id = peer_id.GetNodeId();
    s_event_cb(ev);
}

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg) {
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
            event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
            static bool s_thread_br_init = false;
            if (!s_thread_br_init) {
                ESP_LOGI(TAG, "WiFi got IP, initializing Thread BR");
                esp_openthread_set_backbone_netif(
                    esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
                esp_openthread_lock_acquire(portMAX_DELAY);
                esp_openthread_border_router_init();
                esp_openthread_lock_release();
                s_thread_br_init = true;
            }
        }
        break;
#endif
    default:
        break;
    }
}

esp_err_t matter_init(matter_event_cb_t cb) {
    s_event_cb = cb;

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    esp_openthread_platform_config_t ot_config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_config);
    ESP_LOGI(TAG, "OpenThread platform configured (UART RCP)");
#endif

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return err;
    }
    ESP_LOGI(TAG, "Matter stack started");

    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        err = esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Controller client init failed: %d", err);
            return err;
        }
        err = esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Commissioner setup failed: %d", err);
            return err;
        }

        esp_matter::controller::pairing_command_callbacks_t pairing_cbs = {};
        pairing_cbs.pase_callback = on_pase_complete;
        pairing_cbs.commissioning_success_callback = on_commissioning_success;
        pairing_cbs.commissioning_failure_callback = on_commissioning_failure;
        esp_matter::controller::pairing_command::get_instance().set_callbacks(pairing_cbs);
    }

    ESP_LOGI(TAG, "Commissioner ready");
    if (s_event_cb) {
        matter_event_t ev = {};
        ev.type = MATTER_EVENT_STACK_READY;
        s_event_cb(ev);
    }
    return ESP_OK;
}
