#include "matter_commission.h"
#include "matter_init.h"

#include <cstring>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mdns.h>
#include <wifi_connection.h>
#include <controller/CommissioningDelegate.h>
#include <credentials/attestation_verifier/DeviceAttestationDelegate.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>
#include <lib/support/ThreadOperationalDataset.h>
#include <platform/PlatformManager.h>

static const char *TAG = "matter_comm";

#define COMMISSION_TIMEOUT_US (90ULL * 1000000ULL)

extern "C" void matter_post_event(matter_event_t event);

using chip::Controller::CommissioningParameters;
using chip::Controller::DeviceCommissioner;
using chip::Controller::DiscoveryType;
using chip::Credentials::AttestationVerificationResult;
using chip::Credentials::DeviceAttestationDelegate;
using chip::Credentials::DeviceAttestationVerifier;

// Attestation delegate that continues commissioning on failure
// but notifies the app about the security warning.
class IgnoreAttestationDelegate : public DeviceAttestationDelegate {
public:
    chip::Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override {
        return chip::MakeOptional(static_cast<uint16_t>(120));
    }

    void OnDeviceAttestationCompleted(
        DeviceCommissioner *commissioner,
        chip::DeviceProxy *device,
        const DeviceAttestationVerifier::AttestationDeviceInfo &info,
        AttestationVerificationResult result) override {

        if (result != AttestationVerificationResult::kSuccess) {
            ESP_LOGW(TAG, "Attestation failed (err %hu), continuing",
                     static_cast<uint16_t>(result));
            matter_event_t ev = {};
            ev.type = MATTER_EVENT_ATTESTATION_WARNING;
            matter_post_event(ev);
        }

        auto err =
            commissioner->ContinueCommissioningAfterDeviceAttestation(
                device, AttestationVerificationResult::kSuccess);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "ContinueCommissioning failed: "
                     "%" CHIP_ERROR_FORMAT, err.Format());
        }
    }
};

static IgnoreAttestationDelegate s_attestation_delegate;

static esp_err_t get_wifi_creds(
    char *ssid, size_t ssid_len, char *pwd, size_t pwd_len) {
    wifi_config_t cfg = {};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    strncpy(ssid, (const char *)cfg.sta.ssid, ssid_len - 1);
    strncpy(pwd, (const char *)cfg.sta.password, pwd_len - 1);
    return ESP_OK;
}

static DeviceCommissioner *get_commissioner() {
    return esp_matter::controller::matter_controller_client::get_instance()
        .get_commissioner();
}

static esp_err_t check_idle_and_register() {
    auto *comm = get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGE(TAG, "Another pairing is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    comm->RegisterPairingDelegate(
        &esp_matter::controller::pairing_command::get_instance());
    return ESP_OK;
}

// Check that an IP interface is up. On-network commissioning
// requires mDNS discovery which needs a working IP interface.
// Accepts WiFi STA or Ethernet.
static esp_err_t require_ip(void) {
    const char *keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
    for (int i = 0; i < 2; i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(
            keys[i]);
        if (!netif) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No network interface with IP address, "
             "cannot discover devices on the network");
    return ESP_ERR_INVALID_STATE;
}

static int hex_char_to_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(
    const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0) return -1;
    size_t byte_len = hex_len / 2;
    if (byte_len > out_len) return -1;
    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_char_to_nibble(hex[i * 2]);
        int lo = hex_char_to_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)byte_len;
}

// Determine if discovery hints indicate BLE or SoftAP transport
static bool hints_need_wifi_creds(uint8_t hints) {
    return (hints & DISC_HINT_BLE) || (hints & DISC_HINT_SOFTAP);
}

static DiscoveryType hints_to_discovery_type(uint8_t hints) {
    if (hints_need_wifi_creds(hints)) return DiscoveryType::kAll;
    return DiscoveryType::kDiscoveryNetworkOnly;
}

// Thread dataset buffer shared between build_params and caller
// (must remain valid through PairDevice call).
static uint8_t s_thread_dataset_buf[254];
static int s_thread_dataset_len = 0;

// Build CommissioningParameters with attestation delegate and
// optionally WiFi/Thread credentials based on discovery hints.
// Returns the (possibly adjusted) hints via out_hints.
static esp_err_t build_params(
    CommissioningParameters &params, uint8_t *out_hints) {
    uint8_t hints = *out_hints;
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);

    if (hints_need_wifi_creds(hints)) {
        char ssid[33] = {};
        char pwd[65] = {};
        esp_err_t err =
            get_wifi_creds(ssid, sizeof(ssid), pwd, sizeof(pwd));
        if (err != ESP_OK) {
            // WiFi not available (e.g. Ethernet mode). If
            // on-network discovery is also enabled, drop BLE/SoftAP
            // and continue with on-network only.
            if (hints & DISC_HINT_ON_NET) {
                ESP_LOGW(TAG, "WiFi creds unavailable, using "
                         "on-network discovery only");
                hints &= ~(DISC_HINT_BLE | DISC_HINT_SOFTAP);
                *out_hints = hints;
            } else {
                ESP_LOGE(TAG, "Failed to get WiFi credentials");
                return err;
            }
        } else {
            chip::ByteSpan nameSpan(
                reinterpret_cast<const uint8_t *>(ssid),
                strlen(ssid));
            chip::ByteSpan pwdSpan(
                reinterpret_cast<const uint8_t *>(pwd),
                strlen(pwd));
            params.SetWiFiCredentials(
                chip::Controller::WiFiCredentials(
                    nameSpan, pwdSpan));
        }
    }

    // When BLE discovery is enabled, also provide Thread
    // credentials so the auto-commissioner can provision Thread
    // devices discovered via BLE.
    if (hints & DISC_HINT_BLE) {
        char hex_buf[509];
        if (matter_get_thread_active_dataset_hex(
                hex_buf, sizeof(hex_buf)) == ESP_OK) {
            s_thread_dataset_len = hex_to_bytes(
                hex_buf, s_thread_dataset_buf,
                sizeof(s_thread_dataset_buf));
            if (s_thread_dataset_len > 0) {
                params.SetThreadOperationalDataset(chip::ByteSpan(
                    s_thread_dataset_buf, s_thread_dataset_len));
                ESP_LOGI(TAG, "Thread dataset attached for "
                         "BLE commissioning (%d bytes)",
                         s_thread_dataset_len);
            }
        } else {
            ESP_LOGW(TAG, "No Thread dataset from border router; "
                     "Thread devices may fail commissioning");
        }
    }

    return ESP_OK;
}

// Extract discovery hints from a parsed SetupPayload
static uint8_t hints_from_payload(const chip::SetupPayload &payload) {
    if (!payload.rendezvousInformation.HasValue()) {
        return DISC_HINT_ON_NET;
    }
    return payload.rendezvousInformation.Value().Raw();
}

// Inject attestation delegate into the commissioner's current
// params. Used for the on_network discovery flow where we don't
// control CommissioningParameters creation.
static void inject_attestation_delegate() {
    auto *comm = get_commissioner();
    CommissioningParameters params = comm->GetCommissioningParameters();
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    comm->UpdateCommissioningParameters(params);
}

// --- Commission timeout ---

static esp_timer_handle_t s_timeout_timer = nullptr;
static uint64_t s_timeout_node_id = 0;

static void commission_timeout_work(intptr_t arg) {
    uint64_t node_id = static_cast<uint64_t>(arg);
    ESP_LOGW(TAG, "Commission timeout (90s) for node 0x%llx",
             (unsigned long long)node_id);

    auto *comm = get_commissioner();
    comm->StopPairing(node_id);
    // StopPairing does not invoke the pairing delegate callback, so
    // the delegate stays registered and blocks the next commission.
    // Unregister it explicitly so check_idle_and_register() succeeds.
    comm->RegisterPairingDelegate(nullptr);

    matter_event_t ev = {};
    ev.type = MATTER_EVENT_COMMISSION_TIMEOUT;
    ev.node_id = node_id;
    matter_post_event(ev);
}

static void commission_timeout_cb(void *arg) {
    // Schedule on the CHIP platform task which has a large stack.
    // The esp_timer task stack is too small for StopPairing cleanup.
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        commission_timeout_work,
        static_cast<intptr_t>(s_timeout_node_id));
}

static void start_commission_timeout(uint64_t node_id) {
    if (!s_timeout_timer) {
        esp_timer_create_args_t args = {};
        args.callback = commission_timeout_cb;
        args.name = "comm_timeout";
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_timeout_timer));
    }
    esp_timer_stop(s_timeout_timer);
    s_timeout_node_id = node_id;
    esp_timer_start_once(s_timeout_timer, COMMISSION_TIMEOUT_US);
}

void matter_commission_cancel_timeout(void) {
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
    }
}

// --- mDNS diagnostics ---

static void diag_mdns_browse(void) {
    esp_netif_ip_info_t ip = {};
    bool found = false;
    const char *iface = "?";
    const char *keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
    const char *names[] = {"WiFi STA", "Ethernet"};
    for (int i = 0; i < 2 && !found; i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(
            keys[i]);
        if (!netif) continue;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            found = true;
            iface = names[i];
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "DIAG: no network interface with IP");
        return;
    }
    ESP_LOGI(TAG, "DIAG: %s IP=" IPSTR, iface, IP2STR(&ip.ip));

    // Synchronous mDNS browse for commissionable devices
    mdns_result_t *results = nullptr;
    ESP_LOGI(TAG, "DIAG: mDNS query _matterc._udp (5s)...");
    esp_err_t err = mdns_query_ptr(
        "_matterc", "_udp", 5000, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DIAG: mdns_query_ptr failed: %d", err);
    }
    int count = 0;
    for (mdns_result_t *r = results; r; r = r->next) {
        ESP_LOGI(TAG, "DIAG: found [%d] instance=%s host=%s "
                 "port=%u",
                 count, r->instance_name ? r->instance_name : "?",
                 r->hostname ? r->hostname : "?", r->port);
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                ESP_LOGI(TAG, "DIAG:   addr=" IPSTR,
                         IP2STR(&a->addr.u_addr.ip4));
            } else {
                ESP_LOGI(TAG, "DIAG:   addr=" IPV6STR,
                         IPV62STR(a->addr.u_addr.ip6));
            }
        }
        count++;
    }
    if (count == 0) {
        ESP_LOGW(TAG, "DIAG: no _matterc._udp services found "
                 "via mDNS");
    }
    mdns_query_results_free(results);

    // Check delegated services (registered via border router
    // SRP proxy from Thread devices)
    mdns_result_t *delegated = nullptr;
    mdns_lookup_delegated_service(
        nullptr, "_matterc", "_udp", 20, &delegated);
    count = 0;
    for (mdns_result_t *r = delegated; r; r = r->next) {
        ESP_LOGI(TAG, "DIAG: delegated [%d] instance=%s "
                 "host=%s port=%u",
                 count, r->instance_name ? r->instance_name : "?",
                 r->hostname ? r->hostname : "?", r->port);
        count++;
    }
    if (count == 0) {
        ESP_LOGI(TAG, "DIAG: no delegated _matterc services");
    }
    mdns_query_results_free(delegated);
}

// --- Public API ---

esp_err_t matter_commission_on_network(
    uint64_t node_id, uint32_t pincode) {
    ESP_LOGI(TAG, "Pairing on-network: node=0x%llx pin=%lu",
             (unsigned long long)node_id, (unsigned long)pincode);
    ESP_RETURN_ON_ERROR(require_ip(), TAG, "No network");
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    inject_attestation_delegate();
    esp_err_t err = esp_matter::controller::pairing_on_network(
        node_id, pincode);
    if (err == ESP_OK) start_commission_timeout(node_id);
    return err;
}

esp_err_t matter_commission_disc_pass(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator,
    uint8_t discovery_hints) {

    // Default to on-network if no hints provided
    if (discovery_hints == 0) discovery_hints = DISC_HINT_ON_NET;

    ESP_LOGI(TAG, "Pairing disc+pass: node=0x%llx pin=%lu disc=%u "
             "hints=0x%02x", (unsigned long long)node_id,
             (unsigned long)pincode, discriminator, discovery_hints);

    if (discovery_hints & DISC_HINT_ON_NET) {
        ESP_RETURN_ON_ERROR(
            require_ip(), TAG, "No network");
    }

    chip::SetupPayload payload;
    payload.setUpPINCode = pincode;
    payload.discriminator.SetLongValue(discriminator);
    payload.version = 0;
    payload.rendezvousInformation.SetValue(
        chip::RendezvousInformationFlags(discovery_hints));

    std::string manual_code;
    chip::ManualSetupPayloadGenerator generator(payload);
    if (generator.payloadDecimalStringRepresentation(manual_code)
        != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to generate manual pairing code");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Generated manual code: %s", manual_code.c_str());
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    CommissioningParameters params;
    ESP_RETURN_ON_ERROR(
        build_params(params, &discovery_hints), TAG, "Params");
    get_commissioner()->PairDevice(
        node_id, manual_code.c_str(), params,
        hints_to_discovery_type(discovery_hints));
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_commission_setup_code(
    uint64_t node_id, const char *code) {
    ESP_LOGI(TAG, "Pairing setup code: node=0x%llx code=%s",
             (unsigned long long)node_id, code);

    // Try to parse the code to extract discovery hints
    chip::SetupPayload payload;
    uint8_t hints = DISC_HINT_ON_NET;

    if (code[0] == 'M' && code[1] == 'T') {
        // QR code format
        chip::QRCodeSetupPayloadParser qr_parser(code);
        if (qr_parser.populatePayload(payload) == CHIP_NO_ERROR) {
            hints = hints_from_payload(payload);
            ESP_LOGI(TAG, "QR code parsed: hints=0x%02x", hints);
        }
    } else {
        // Manual numeric codes don't encode rendezvous info.
        // Try BLE in addition to on-network so we can commission
        // Thread devices that aren't reachable via mDNS.
        chip::ManualSetupPayloadParser manual_parser(code);
        if (manual_parser.populatePayload(payload) == CHIP_NO_ERROR) {
            hints = DISC_HINT_BLE | DISC_HINT_ON_NET;
            ESP_LOGI(TAG, "Manual code parsed: hints=0x%02x "
                     "(BLE+on-network)", hints);
        }
    }

    // On-network discovery requires a working IP interface.
    // When BLE is also available, degrade gracefully to BLE-only.
    if (hints & DISC_HINT_ON_NET) {
        if (require_ip() != ESP_OK) {
            if (hints & DISC_HINT_BLE) {
                hints &= ~DISC_HINT_ON_NET;
                ESP_LOGW(TAG, "No IP, using BLE-only discovery");
            } else {
                ESP_LOGE(TAG, "No IP for on-network discovery");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    if (hints & DISC_HINT_ON_NET) {
        diag_mdns_browse();
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    CommissioningParameters params;
    ESP_RETURN_ON_ERROR(
        build_params(params, &hints), TAG, "Params");
    get_commissioner()->PairDevice(
        node_id, code, params, hints_to_discovery_type(hints));
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_commission_ble_wifi(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator) {
    char ssid[33] = {};
    char pwd[65] = {};
    esp_err_t err = get_wifi_creds(ssid, sizeof(ssid), pwd, sizeof(pwd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi credentials");
        return err;
    }
    ESP_LOGI(TAG, "Pairing BLE+WiFi: node=0x%llx pin=%lu disc=%u "
             "ssid=%s", (unsigned long long)node_id,
             (unsigned long)pincode, discriminator, ssid);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    chip::RendezvousParameters rendezvous =
        chip::RendezvousParameters()
            .SetSetupPINCode(pincode)
            .SetDiscriminator(discriminator)
            .SetPeerAddress(chip::Transport::PeerAddress::BLE());

    chip::ByteSpan nameSpan(
        reinterpret_cast<const uint8_t *>(ssid), strlen(ssid));
    chip::ByteSpan pwdSpan(
        reinterpret_cast<const uint8_t *>(pwd), strlen(pwd));
    CommissioningParameters params;
    params.SetWiFiCredentials(
        chip::Controller::WiFiCredentials(nameSpan, pwdSpan));
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    get_commissioner()->PairDevice(node_id, rendezvous, params);
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_commission_ble_wifi_code(
    uint64_t node_id, const char *code) {
    ESP_LOGI(TAG, "Pairing BLE+WiFi code: node=0x%llx code=%s",
             (unsigned long long)node_id, code);

    chip::SetupPayload payload;
    bool parsed = false;
    if (code[0] == 'M' && code[1] == 'T') {
        chip::QRCodeSetupPayloadParser parser(code);
        parsed = (parser.populatePayload(payload) == CHIP_NO_ERROR);
    } else {
        chip::ManualSetupPayloadParser parser(code);
        parsed = (parser.populatePayload(payload) == CHIP_NO_ERROR);
    }
    if (!parsed) {
        ESP_LOGE(TAG, "Failed to parse setup code");
        return ESP_ERR_INVALID_ARG;
    }

    char ssid[33] = {};
    char pwd[65] = {};
    esp_err_t err =
        get_wifi_creds(ssid, sizeof(ssid), pwd, sizeof(pwd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi credentials");
        return err;
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    chip::ByteSpan nameSpan(
        reinterpret_cast<const uint8_t *>(ssid), strlen(ssid));
    chip::ByteSpan pwdSpan(
        reinterpret_cast<const uint8_t *>(pwd), strlen(pwd));
    CommissioningParameters params;
    params.SetWiFiCredentials(
        chip::Controller::WiFiCredentials(nameSpan, pwdSpan));
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);

    get_commissioner()->PairDevice(
        node_id, code, params, DiscoveryType::kAll);
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_commission_ble_thread(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator,
    const char *dataset_hex) {
    ESP_LOGI(TAG, "Pairing BLE+Thread: node=0x%llx pin=%lu disc=%u",
             (unsigned long long)node_id,
             (unsigned long)pincode, discriminator);

    uint8_t dataset_buf[chip::Thread::kSizeOperationalDataset];
    int dataset_len = 0;

    bool have_explicit = dataset_hex && dataset_hex[0] != '\0';
    if (have_explicit) {
        dataset_len = hex_to_bytes(
            dataset_hex, dataset_buf, sizeof(dataset_buf));
        if (dataset_len <= 0) {
            ESP_LOGE(TAG, "Invalid Thread dataset hex string");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        char hex_buf[509];
        esp_err_t err = matter_get_thread_active_dataset_hex(
            hex_buf, sizeof(hex_buf));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No Thread dataset provided and border "
                     "router has no active dataset");
            return ESP_ERR_NOT_FOUND;
        }
        dataset_len = hex_to_bytes(
            hex_buf, dataset_buf, sizeof(dataset_buf));
        if (dataset_len <= 0) {
            ESP_LOGE(TAG, "Failed to decode border router dataset");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Using active dataset from border router");
    }

    chip::Thread::OperationalDataset dataset;
    if (dataset.Init(chip::ByteSpan(dataset_buf, dataset_len))
        != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to parse Thread dataset TLV");
        return ESP_ERR_INVALID_ARG;
    }

    char net_name[chip::Thread::kSizeNetworkName + 1] = {};
    if (dataset.GetNetworkName(net_name) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Thread network: %s", net_name);
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    chip::RendezvousParameters rendezvous =
        chip::RendezvousParameters()
            .SetSetupPINCode(pincode)
            .SetDiscriminator(discriminator)
            .SetPeerAddress(chip::Transport::PeerAddress::BLE());

    CommissioningParameters params;
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    params.SetThreadOperationalDataset(
        chip::ByteSpan(dataset_buf, dataset_len));

    get_commissioner()->PairDevice(node_id, rendezvous, params);
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_commission_ble_thread_code(
    uint64_t node_id, const char *code, const char *dataset_hex) {
    ESP_LOGI(TAG, "Pairing BLE+Thread code: node=0x%llx code=%s",
             (unsigned long long)node_id, code);

    chip::SetupPayload payload;
    bool parsed = false;
    if (code[0] == 'M' && code[1] == 'T') {
        chip::QRCodeSetupPayloadParser parser(code);
        parsed = (parser.populatePayload(payload) == CHIP_NO_ERROR);
    } else {
        chip::ManualSetupPayloadParser parser(code);
        parsed = (parser.populatePayload(payload) == CHIP_NO_ERROR);
    }
    if (!parsed) {
        ESP_LOGE(TAG, "Failed to parse setup code");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t dataset_buf[chip::Thread::kSizeOperationalDataset];
    int dataset_len = 0;

    bool have_explicit = dataset_hex && dataset_hex[0] != '\0';
    if (have_explicit) {
        dataset_len = hex_to_bytes(
            dataset_hex, dataset_buf, sizeof(dataset_buf));
        if (dataset_len <= 0) {
            ESP_LOGE(TAG, "Invalid Thread dataset hex string");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        char hex_buf[509];
        esp_err_t err = matter_get_thread_active_dataset_hex(
            hex_buf, sizeof(hex_buf));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No Thread dataset provided and border "
                     "router has no active dataset");
            return ESP_ERR_NOT_FOUND;
        }
        dataset_len = hex_to_bytes(
            hex_buf, dataset_buf, sizeof(dataset_buf));
        if (dataset_len <= 0) {
            ESP_LOGE(TAG, "Failed to decode border router dataset");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Using active dataset from border router");
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    CommissioningParameters params;
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    params.SetThreadOperationalDataset(
        chip::ByteSpan(dataset_buf, dataset_len));

    get_commissioner()->PairDevice(
        node_id, code, params, DiscoveryType::kAll);
    start_commission_timeout(node_id);
    return ESP_OK;
}

esp_err_t matter_device_unpair(uint64_t node_id) {
    ESP_LOGI(TAG, "Unpair: node=0x%llx", (unsigned long long)node_id);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::unpair_device(node_id);
}
