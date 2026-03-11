#include "matter_commission.h"
#include "matter_init.h"

#include <cstring>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_wifi.h>
#include <controller/CommissioningDelegate.h>
#include <credentials/attestation_verifier/DeviceAttestationDelegate.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>
#include <lib/support/ThreadOperationalDataset.h>

static const char *TAG = "matter_comm";

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

// Determine if discovery hints indicate BLE or SoftAP transport
static bool hints_need_wifi_creds(uint8_t hints) {
    return (hints & DISC_HINT_BLE) || (hints & DISC_HINT_SOFTAP);
}

static DiscoveryType hints_to_discovery_type(uint8_t hints) {
    if (hints_need_wifi_creds(hints)) return DiscoveryType::kAll;
    return DiscoveryType::kDiscoveryNetworkOnly;
}

// Build CommissioningParameters with attestation delegate and
// optionally WiFi credentials based on discovery hints.
static esp_err_t build_params(
    CommissioningParameters &params, uint8_t hints) {
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);

    if (hints_need_wifi_creds(hints)) {
        char ssid[33] = {};
        char pwd[65] = {};
        esp_err_t err =
            get_wifi_creds(ssid, sizeof(ssid), pwd, sizeof(pwd));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get WiFi credentials");
            return err;
        }
        chip::ByteSpan nameSpan(
            reinterpret_cast<const uint8_t *>(ssid), strlen(ssid));
        chip::ByteSpan pwdSpan(
            reinterpret_cast<const uint8_t *>(pwd), strlen(pwd));
        params.SetWiFiCredentials(
            chip::Controller::WiFiCredentials(nameSpan, pwdSpan));
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

// --- Public API ---

esp_err_t matter_commission_on_network(
    uint64_t node_id, uint32_t pincode) {
    ESP_LOGI(TAG, "Pairing on-network: node=0x%llx pin=%lu",
             (unsigned long long)node_id, (unsigned long)pincode);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    inject_attestation_delegate();
    return esp_matter::controller::pairing_on_network(
        node_id, pincode);
}

esp_err_t matter_commission_disc_pass(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator,
    uint8_t discovery_hints) {

    // Default to on-network if no hints provided
    if (discovery_hints == 0) discovery_hints = DISC_HINT_ON_NET;

    ESP_LOGI(TAG, "Pairing disc+pass: node=0x%llx pin=%lu disc=%u "
             "hints=0x%02x", (unsigned long long)node_id,
             (unsigned long)pincode, discriminator, discovery_hints);

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
        build_params(params, discovery_hints), TAG, "Params");
    get_commissioner()->PairDevice(
        node_id, manual_code.c_str(), params,
        hints_to_discovery_type(discovery_hints));
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
        // Manual numeric codes never encode rendezvous info, so
        // always try BLE + on-network to support uncommissioned
        // BLE+WiFi devices.
        chip::ManualSetupPayloadParser manual_parser(code);
        if (manual_parser.populatePayload(payload) == CHIP_NO_ERROR) {
            hints = DISC_HINT_BLE | DISC_HINT_ON_NET;
            ESP_LOGI(TAG, "Manual code parsed: hints=0x%02x", hints);
        }
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ESP_RETURN_ON_ERROR(check_idle_and_register(), TAG, "Busy");

    CommissioningParameters params;
    ESP_RETURN_ON_ERROR(build_params(params, hints), TAG, "Params");
    get_commissioner()->PairDevice(
        node_id, code, params, hints_to_discovery_type(hints));
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
    return ESP_OK;
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

esp_err_t matter_commission_ble_thread(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator,
    const char *dataset_hex) {
    ESP_LOGI(TAG, "Pairing BLE+Thread: node=0x%llx pin=%lu disc=%u",
             (unsigned long long)node_id,
             (unsigned long)pincode, discriminator);

    uint8_t dataset_buf[chip::Thread::kSizeOperationalDataset];
    int dataset_len = hex_to_bytes(
        dataset_hex, dataset_buf, sizeof(dataset_buf));
    if (dataset_len <= 0) {
        ESP_LOGE(TAG, "Invalid Thread dataset hex string");
        return ESP_ERR_INVALID_ARG;
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
    return ESP_OK;
}

esp_err_t matter_device_unpair(uint64_t node_id) {
    ESP_LOGI(TAG, "Unpair: node=0x%llx", (unsigned long long)node_id);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::unpair_device(node_id);
}
