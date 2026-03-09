#include "matter_init.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <platform/PlatformManager.h>

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
    default:
        break;
    }
}

esp_err_t matter_init(matter_event_cb_t cb) {
    s_event_cb = cb;

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
