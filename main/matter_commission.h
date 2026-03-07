#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_commission_on_network(uint64_t node_id, uint32_t pincode);
esp_err_t matter_commission_code(uint64_t node_id, const char *payload);
esp_err_t matter_commission_code_wifi(uint64_t node_id, const char *payload);
esp_err_t matter_device_unpair(uint64_t node_id);

#ifdef __cplusplus
}
#endif
