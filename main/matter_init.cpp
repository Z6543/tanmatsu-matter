#include "matter_commission.h"
#include "matter_init.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <platform/PlatformManager.h>

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <esp_netif.h>
#include <esp_openthread.h>
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <esp_ot_config.h>
#include <openthread/dataset.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

static const char *TAG = "matter_init";
static matter_event_cb_t s_event_cb = nullptr;

extern "C" void matter_post_event(matter_event_t event) {
    if (s_event_cb) s_event_cb(event);
}

static void on_pase_complete(CHIP_ERROR err) {
    if (err != CHIP_NO_ERROR) {
        matter_commission_cancel_timeout();
    }
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = (err == CHIP_NO_ERROR) ? MATTER_EVENT_PASE_SUCCESS : MATTER_EVENT_PASE_FAILED;
    s_event_cb(ev);
}

static void on_commissioning_success(chip::ScopedNodeId peer_id) {
    matter_commission_cancel_timeout();
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
    matter_commission_cancel_timeout();
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_COMMISSION_FAILED;
    ev.node_id = peer_id.GetNodeId();
    s_event_cb(ev);
}

#if CONFIG_OPENTHREAD_BORDER_ROUTER
static bool s_thread_br_init = false;

static void init_thread_border_router() {
    if (s_thread_br_init) return;
    ESP_LOGI(TAG, "Initializing Thread border router");
    esp_openthread_set_backbone_netif(
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_border_router_init();

    otInstance *instance = esp_openthread_get_instance();
    if (!otDatasetIsCommissioned(instance)) {
        ESP_LOGI(TAG, "No active Thread dataset, creating from "
                 "sdkconfig defaults");
        esp_openthread_auto_start(NULL);
    } else {
        ESP_LOGI(TAG, "Thread dataset already commissioned");
    }

    esp_openthread_lock_release();
    s_thread_br_init = true;
}
#endif

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg) {
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
            event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
            init_thread_border_router();
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

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    // WiFi may already have an IP if connected before Matter started.
    // The IP_EVENT_STA_GOT_IP event won't be re-delivered, so check now.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi already has IP, initializing BR now");
            init_thread_border_router();
        }
    }
#endif

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

esp_err_t matter_get_thread_active_dataset_hex(
    char *out, size_t out_len) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    otOperationalDatasetTlvs tlvs;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError ot_err = otDatasetGetActiveTlvs(
        esp_openthread_get_instance(), &tlvs);
    esp_openthread_lock_release();

    if (ot_err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "No active Thread dataset (ot_err=%d)", ot_err);
        return ESP_ERR_NOT_FOUND;
    }

    size_t hex_len = tlvs.mLength * 2 + 1;
    if (out_len < hex_len) {
        ESP_LOGE(TAG, "Dataset buffer too small: need %u, have %u",
                 (unsigned)hex_len, (unsigned)out_len);
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < tlvs.mLength; i++) {
        snprintf(out + i * 2, 3, "%02x", tlvs.mTlvs[i]);
    }
    out[tlvs.mLength * 2] = '\0';
    return ESP_OK;
#else
    (void)out;
    (void)out_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
