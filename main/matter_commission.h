#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Discovery hint bitmask (matches Matter RendezvousInformationFlag)
#define DISC_HINT_SOFTAP   (1 << 0)
#define DISC_HINT_BLE      (1 << 1)
#define DISC_HINT_ON_NET   (1 << 2)

esp_err_t matter_commission_on_network(
    uint64_t node_id, uint32_t pincode);

esp_err_t matter_commission_disc_pass(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator,
    uint8_t discovery_hints);

esp_err_t matter_commission_setup_code(
    uint64_t node_id, const char *code);

esp_err_t matter_commission_ble_wifi(
    uint64_t node_id, uint32_t pincode, uint16_t discriminator);

esp_err_t matter_commission_setup_code_thread(
    uint64_t node_id, const char *code,
    const char *dataset_hex);

esp_err_t matter_device_unpair(uint64_t node_id);

#ifdef __cplusplus
}
#endif
