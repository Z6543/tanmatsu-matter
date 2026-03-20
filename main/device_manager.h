#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MATTER_DEVICE_MAX       5
#define MATTER_DEVICE_NAME_LEN  32

typedef enum {
    DEVICE_CAT_UNKNOWN = 0,
    DEVICE_CAT_ON_OFF_LIGHT,
    DEVICE_CAT_DIMMABLE_LIGHT,
    DEVICE_CAT_COLOR_TEMP_LIGHT,
    DEVICE_CAT_COLOR_LIGHT,
    DEVICE_CAT_ON_OFF_PLUG,
    DEVICE_CAT_DIMMABLE_PLUG,
    DEVICE_CAT_THERMOSTAT,
    DEVICE_CAT_DOOR_LOCK,
    DEVICE_CAT_WINDOW_COVERING,
    DEVICE_CAT_CONTACT_SENSOR,
    DEVICE_CAT_TEMP_SENSOR,
    DEVICE_CAT_HUMIDITY_SENSOR,
    DEVICE_CAT_OCCUPANCY_SENSOR,
    DEVICE_CAT_LIGHT_SENSOR,
    DEVICE_CAT_ON_OFF_SWITCH,
    DEVICE_CAT_FAN,
} device_category_t;

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    char     name[MATTER_DEVICE_NAME_LEN];
    uint32_t device_type_id;
    device_category_t category;
    bool     reachable;
    bool     is_thread;

    // Common state
    bool     on_off;

    // Dimmable lights/plugs
    uint8_t  level;

    // Color lights
    uint16_t color_temp_mireds;
    uint8_t  hue;
    uint8_t  saturation;

    // Thermostat
    int16_t  local_temp;
    int16_t  setpoint_heat;
    int16_t  setpoint_cool;
    uint8_t  thermostat_mode;

    // Door lock
    uint8_t  lock_state;

    // Window covering
    uint8_t  cover_position;
    uint8_t  cover_tilt;

    // Sensors
    int16_t  temperature;
    uint16_t humidity;
    bool     occupancy;
    bool     contact;
    uint16_t illuminance;
} matter_device_t;

device_category_t device_type_to_category(uint32_t device_type_id);

esp_err_t device_manager_init(void);
int       device_manager_count(void);
const matter_device_t *device_manager_get(int index);
matter_device_t       *device_manager_get_mut(int index);
const matter_device_t *device_manager_find(uint64_t node_id);
matter_device_t       *device_manager_find_mut(uint64_t node_id);
esp_err_t device_manager_add(
    uint64_t node_id, uint16_t endpoint_id, const char *name,
    uint32_t device_type_id, bool is_thread);
esp_err_t device_manager_remove(uint64_t node_id);
esp_err_t device_manager_rename(uint64_t node_id, const char *name);
esp_err_t device_manager_save(void);
uint64_t  device_manager_next_node_id(void);

#ifdef __cplusplus
}
#endif
