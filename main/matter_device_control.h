#pragma once

#include "device_manager.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*matter_device_state_cb_t)(uint64_t node_id);

void      matter_device_set_state_cb(matter_device_state_cb_t cb);

// On/Off
esp_err_t matter_device_send_on(uint64_t node_id, uint16_t ep);
esp_err_t matter_device_send_off(uint64_t node_id, uint16_t ep);
esp_err_t matter_device_send_toggle(uint64_t node_id, uint16_t ep);

// Level control
esp_err_t matter_device_send_level(
    uint64_t node_id, uint16_t ep, uint8_t level);

// Color control
esp_err_t matter_device_send_color_temp(
    uint64_t node_id, uint16_t ep, uint16_t mireds);
esp_err_t matter_device_send_hue_sat(
    uint64_t node_id, uint16_t ep, uint8_t hue, uint8_t sat);

// Door lock
esp_err_t matter_device_send_lock(uint64_t node_id, uint16_t ep);
esp_err_t matter_device_send_unlock(uint64_t node_id, uint16_t ep);

// Thermostat
esp_err_t matter_device_send_setpoint(
    uint64_t node_id, uint16_t ep, int16_t heat, int16_t cool);
esp_err_t matter_device_send_thermo_mode(
    uint64_t node_id, uint16_t ep, uint8_t mode);

// Window covering
esp_err_t matter_device_send_cover_pos(
    uint64_t node_id, uint16_t ep, uint8_t pct);
esp_err_t matter_device_send_cover_open(
    uint64_t node_id, uint16_t ep);
esp_err_t matter_device_send_cover_close(
    uint64_t node_id, uint16_t ep);
esp_err_t matter_device_send_cover_stop(
    uint64_t node_id, uint16_t ep);

// Fan
esp_err_t matter_device_send_fan_mode(
    uint64_t node_id, uint16_t ep, uint8_t mode);

// Type-aware subscribe
esp_err_t matter_device_subscribe(
    uint64_t node_id, uint16_t ep, device_category_t cat);

// Legacy: subscribe to on/off only
esp_err_t matter_device_subscribe_onoff(
    uint64_t node_id, uint16_t ep);
esp_err_t matter_device_subscribe_all(void);
esp_err_t matter_device_subscribe_wifi(void);
esp_err_t matter_device_subscribe_thread(void);
esp_err_t matter_device_subscribe_thread_delayed(void);

/**
 * Read device info (endpoints + basic information + device type)
 * from a newly commissioned device. Posts
 * MATTER_EVENT_DEVICE_INFO_READY when done.
 */
esp_err_t matter_device_read_info(uint64_t node_id);

#ifdef __cplusplus
}
#endif
