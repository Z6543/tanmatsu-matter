#include "matter_commission.h"

#include <cstring>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_wifi.h>

static const char *TAG = "matter_comm";

static esp_err_t get_wifi_creds(char *ssid, size_t ssid_len, char *pwd, size_t pwd_len) {
    wifi_config_t cfg = {};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    strncpy(ssid, (const char *)cfg.sta.ssid, ssid_len - 1);
    strncpy(pwd, (const char *)cfg.sta.password, pwd_len - 1);
    return ESP_OK;
}

esp_err_t matter_commission_on_network(uint64_t node_id, uint32_t pincode) {
    ESP_LOGI(TAG, "Pairing on-network: node=0x%llx pin=%lu",
             (unsigned long long)node_id, (unsigned long)pincode);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::pairing_on_network(node_id, pincode);
}

esp_err_t matter_commission_code(uint64_t node_id, const char *payload) {
    ESP_LOGI(TAG, "Pairing code on-network: node=0x%llx payload=%s",
             (unsigned long long)node_id, payload);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::pairing_code(node_id, payload);
}

esp_err_t matter_commission_code_wifi(uint64_t node_id, const char *payload) {
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
    return esp_matter::controller::pairing_code_wifi(node_id, ssid, pwd, payload);
}

esp_err_t matter_device_unpair(uint64_t node_id) {
    ESP_LOGI(TAG, "Unpair: node=0x%llx", (unsigned long long)node_id);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::unpair_device(node_id);
}
