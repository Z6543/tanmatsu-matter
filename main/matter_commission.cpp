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
#include <setup_payload/SetupPayload.h>

static const char *TAG = "matter_comm";

extern "C" void matter_post_event(matter_event_t event);

using chip::Controller::CommissioningParameters;
using chip::Controller::DeviceCommissioner;
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
            ESP_LOGE(TAG, "ContinueCommissioning failed: %" CHIP_ERROR_FORMAT,
                     err.Format());
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

// Inject attestation delegate into the commissioner's current params.
// Must be called after PairDevice or before Commission.
static void inject_attestation_delegate() {
    auto *comm = get_commissioner();
    CommissioningParameters params = comm->GetCommissioningParameters();
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    comm->UpdateCommissioningParameters(params);
}

esp_err_t matter_commission_on_network(
    uint64_t node_id, uint32_t pincode) {
    ESP_LOGI(TAG, "Pairing on-network: node=0x%llx pin=%lu",
             (unsigned long long)node_id, (unsigned long)pincode);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // Pre-inject delegate, then use SDK wrapper for discovery flow
    inject_attestation_delegate();
    return esp_matter::controller::pairing_on_network(node_id, pincode);
}

esp_err_t matter_commission_on_network_disc(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator) {
    ESP_LOGI(TAG, "Pairing on-network disc: node=0x%llx pin=%lu disc=%u",
             (unsigned long long)node_id, (unsigned long)pincode,
             discriminator);

    chip::SetupPayload payload;
    payload.setUpPINCode = pincode;
    payload.discriminator.SetLongValue(discriminator);
    payload.version = 0;
    payload.rendezvousInformation.SetValue(
        chip::RendezvousInformationFlags(
            chip::RendezvousInformationFlag::kOnNetwork));

    std::string manual_code;
    chip::ManualSetupPayloadGenerator generator(payload);
    if (generator.payloadDecimalStringRepresentation(manual_code)
        != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to generate manual pairing code");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Generated manual code: %s", manual_code.c_str());
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    auto *comm = get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGE(TAG, "Another pairing is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    comm->RegisterPairingDelegate(
        &esp_matter::controller::pairing_command::get_instance());

    CommissioningParameters params;
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    comm->PairDevice(
        node_id, manual_code.c_str(), params,
        chip::Controller::DiscoveryType::kDiscoveryNetworkOnly);
    return ESP_OK;
}

esp_err_t matter_commission_code(
    uint64_t node_id, const char *payload) {
    ESP_LOGI(TAG, "Pairing code on-network: node=0x%llx payload=%s",
             (unsigned long long)node_id, payload);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    auto *comm = get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGE(TAG, "Another pairing is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    comm->RegisterPairingDelegate(
        &esp_matter::controller::pairing_command::get_instance());

    CommissioningParameters params;
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    comm->PairDevice(
        node_id, payload, params,
        chip::Controller::DiscoveryType::kDiscoveryNetworkOnly);
    return ESP_OK;
}

esp_err_t matter_commission_code_wifi(
    uint64_t node_id, const char *payload) {
    char ssid[33] = {};
    char pwd[65] = {};
    esp_err_t err = get_wifi_creds(ssid, sizeof(ssid), pwd, sizeof(pwd));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi credentials");
        return err;
    }
    ESP_LOGI(TAG, "Pairing code WiFi: node=0x%llx payload=%s ssid=%s",
             (unsigned long long)node_id, payload, ssid);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    auto *comm = get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGE(TAG, "Another pairing is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    comm->RegisterPairingDelegate(
        &esp_matter::controller::pairing_command::get_instance());

    chip::ByteSpan nameSpan(
        reinterpret_cast<const uint8_t *>(ssid), strlen(ssid));
    chip::ByteSpan pwdSpan(
        reinterpret_cast<const uint8_t *>(pwd), strlen(pwd));
    CommissioningParameters params;
    params.SetWiFiCredentials(
        chip::Controller::WiFiCredentials(nameSpan, pwdSpan));
    params.SetDeviceAttestationDelegate(&s_attestation_delegate);
    comm->PairDevice(
        node_id, payload, params,
        chip::Controller::DiscoveryType::kAll);
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
    ESP_LOGI(TAG, "Pairing BLE+WiFi: node=0x%llx pin=%lu disc=%u ssid=%s",
             (unsigned long long)node_id, (unsigned long)pincode,
             discriminator, ssid);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    auto *comm = get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGE(TAG, "Another pairing is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    comm->RegisterPairingDelegate(
        &esp_matter::controller::pairing_command::get_instance());

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
    comm->PairDevice(node_id, rendezvous, params);
    return ESP_OK;
}

esp_err_t matter_device_unpair(uint64_t node_id) {
    ESP_LOGI(TAG, "Unpair: node=0x%llx", (unsigned long long)node_id);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::unpair_device(node_id);
}
