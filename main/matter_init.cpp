#include "matter_commission.h"
#include "matter_init.h"
#include "paa_certs_embedded.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attestation_trust_store.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <platform/PlatformManager.h>

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <driver/uart.h>
#include <esp_netif.h>
#include <esp_openthread.h>
#include <esp_openthread_border_router.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_spinel.h>
#include <esp_ot_config.h>
#include <mdns.h>
#include <openthread/dataset.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

static const char *TAG = "matter_init";
static EmbeddedAttestationTrustStore s_embedded_paa_store;
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
static bool s_thread_available = false;
static bool s_thread_br_init = false;

// Build an HDLC-encoded Spinel frame. Returns frame length.
static int build_hdlc_frame(
    const uint8_t *data, int data_len,
    uint8_t *out, int out_len) {

    uint16_t crc = 0xFFFF;
    for (int i = 0; i < data_len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408
                            : crc >> 1;
        }
    }
    crc = ~crc;

    int pos = 0;
    if (pos >= out_len) return -1;
    out[pos++] = 0x7E;
    for (int i = 0; i < data_len; i++) {
        uint8_t byte = data[i];
        if (byte == 0x7E || byte == 0x7D) {
            if (pos + 1 >= out_len) return -1;
            out[pos++] = 0x7D;
            out[pos++] = byte ^ 0x20;
        } else {
            if (pos >= out_len) return -1;
            out[pos++] = byte;
        }
    }
    for (int f = 0; f < 2; f++) {
        uint8_t byte = (f == 0) ? (uint8_t)(crc & 0xFF)
                                : (uint8_t)(crc >> 8);
        if (byte == 0x7E || byte == 0x7D) {
            if (pos + 1 >= out_len) return -1;
            out[pos++] = 0x7D;
            out[pos++] = byte ^ 0x20;
        } else {
            if (pos >= out_len) return -1;
            out[pos++] = byte;
        }
    }
    if (pos >= out_len) return -1;
    out[pos++] = 0x7E;
    return pos;
}

// Probe the RCP co-processor over UART by sending a Spinel
// PROP_VALUE_GET for PROTOCOL_VERSION and checking for a
// response. NOOP (0x00) is intentionally ignored by the RCP,
// so we must use a real query. Retries up to 3 times.
static bool probe_rcp_uart(void) {
    const uart_port_t port = UART_NUM_1;
    uart_config_t cfg = {
        .baud_rate = 460800,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(
        port, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RCP probe: UART install failed: %d", err);
        return false;
    }
    uart_param_config(port, &cfg);
    uart_set_pin(port, GPIO_NUM_53, GPIO_NUM_54,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Spinel PROP_VALUE_GET(PROTOCOL_VERSION):
    //   header=0x81 (TID=1), cmd=0x02, prop=0x01
    const uint8_t spinel[] = {0x81, 0x02, 0x01};
    uint8_t frame[16];
    int frame_len = build_hdlc_frame(
        spinel, sizeof(spinel), frame, sizeof(frame));

    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        uart_flush_input(port);
        uart_write_bytes(port, frame, frame_len);
        uart_wait_tx_done(port, pdMS_TO_TICKS(100));

        uint8_t buf[64];
        int len = uart_read_bytes(
            port, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "RCP probe attempt %d: got %d bytes",
                 attempt + 1, len);
        if (len > 0) {
            found = true;
        }
    }

    uart_driver_delete(port);
    return found;
}

static void post_thread_br_error(const char *message) {
    ESP_LOGE(TAG, "Thread BR error: %s", message);
    if (!s_event_cb) return;
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_THREAD_BR_ERROR;
    strncpy(ev.msg, message, MATTER_EVENT_MSG_LEN - 1);
    s_event_cb(ev);
}

static void on_rcp_failure(void) {
    post_thread_br_error(
        "Thread radio (RCP) not responding.\n"
        "Flash the C6 with ot_rcp firmware.");
}

static void on_rcp_compat_error(void) {
    post_thread_br_error(
        "Thread radio firmware version mismatch.\n"
        "Update the C6 with a compatible ot_rcp build.");
}

static void on_rcp_reset_failure(void) {
    post_thread_br_error(
        "Failed to reset the Thread radio co-processor.\n"
        "Check that the C6 has ot_rcp firmware installed.");
}

static void init_thread_border_router() {
    if (s_thread_br_init) return;
    ESP_LOGI(TAG, "Initializing Thread border router");

    // mDNS must be initialized before the border router so the SRP
    // server can forward Thread device registrations to mDNS.
    // mdns_init() is idempotent — safe even if Matter inits it later.
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("tanmatsu-br"));

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        esp_openthread_lock_release();
        post_thread_br_error(
            "Border router init failed.\n"
            "Ensure the C6 has ot_rcp firmware.");
        return;
    }

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
    default:
        break;
    }
}

esp_err_t matter_init(matter_event_cb_t cb, bool use_thread) {
    s_event_cb = cb;

#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (use_thread) {
        // Probe the RCP co-processor before configuring OpenThread.
        // If absent, skip OT config so esp_matter::start() won't
        // crash in SpinelDriver::ResetCoprocessor().
        s_thread_available = probe_rcp_uart();
        if (!s_thread_available) {
            ESP_LOGW(TAG, "Thread radio (RCP) not detected on "
                     "UART, proceeding without Thread support");
        } else {
            esp_netif_t *backbone = esp_netif_get_handle_from_ifkey(
                "WIFI_STA_DEF");
            if (backbone) {
                esp_openthread_set_backbone_netif(backbone);
            } else {
                ESP_LOGW(TAG, "WiFi STA netif not found, border "
                         "router may not work");
            }

            esp_openthread_register_rcp_failure_handler(
                on_rcp_failure);
            esp_openthread_set_compatibility_error_callback(
                on_rcp_compat_error);
            esp_openthread_set_coprocessor_reset_failure_callback(
                on_rcp_reset_failure);

            esp_openthread_platform_config_t ot_config = {
                .radio_config =
                    ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
                .host_config =
                    ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
                .port_config =
                    ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
            };
            set_openthread_platform_config(&ot_config);
            ESP_LOGI(TAG,
                "OpenThread platform configured (UART RCP)");
        }
    } else {
        ESP_LOGI(TAG, "WiFi mode — skipping Thread/RCP init");
        s_thread_available = false;
    }
#else
    (void)use_thread;
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
        chip::Credentials::set_custom_attestation_trust_store(
            &s_embedded_paa_store);
        ESP_LOGI(TAG, "PAA trust store: %zu embedded certs",
                 embedded_paa_certs_count());
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

bool matter_thread_available(void) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    return s_thread_available;
#else
    return false;
#endif
}

esp_err_t matter_get_thread_active_dataset_hex(
    char *out, size_t out_len) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (!s_thread_available) return ESP_ERR_NOT_SUPPORTED;
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

esp_err_t matter_start_thread_br(void) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (!s_thread_available) return ESP_ERR_NOT_SUPPORTED;
    if (s_thread_br_init) {
        ESP_LOGI(TAG, "Thread border router already running");
        return ESP_OK;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey(
        "WIFI_STA_DEF");
    if (!sta) {
        ESP_LOGE(TAG, "WiFi STA netif not found");
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "WiFi not connected, cannot start BR");
        return ESP_ERR_INVALID_STATE;
    }

    init_thread_border_router();
    return s_thread_br_init ? ESP_OK : ESP_FAIL;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t matter_stop_thread_br(void) {
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (!s_thread_available) return ESP_ERR_NOT_SUPPORTED;
    if (!s_thread_br_init) {
        ESP_LOGI(TAG, "Thread border router not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping Thread border router");

    esp_openthread_lock_acquire(portMAX_DELAY);

    otInstance *instance = esp_openthread_get_instance();
    otThreadSetEnabled(instance, false);
    otIp6SetEnabled(instance, false);

    esp_err_t err = esp_openthread_border_router_deinit();
    esp_openthread_lock_release();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Border router deinit failed: %d", err);
        return err;
    }

    s_thread_br_init = false;
    ESP_LOGI(TAG, "Thread border router stopped");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
