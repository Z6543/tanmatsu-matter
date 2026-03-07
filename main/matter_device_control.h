#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*matter_device_state_cb_t)(uint64_t node_id, bool on_off);

void      matter_device_set_state_cb(matter_device_state_cb_t cb);
esp_err_t matter_device_send_on(uint64_t node_id, uint16_t endpoint_id);
esp_err_t matter_device_send_off(uint64_t node_id, uint16_t endpoint_id);
esp_err_t matter_device_send_toggle(uint64_t node_id, uint16_t endpoint_id);
esp_err_t matter_device_subscribe_onoff(uint64_t node_id, uint16_t endpoint_id);
esp_err_t matter_device_subscribe_all(void);

#ifdef __cplusplus
}
#endif
