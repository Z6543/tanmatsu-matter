#pragma once

#include "matter_init.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTERFACE_MODE_NONE = 0,
    INTERFACE_MODE_WIFI,
    INTERFACE_MODE_THREAD,
} interface_mode_t;

// Load persisted interface mode from NVS (INTERFACE_MODE_NONE if unset)
interface_mode_t ui_load_interface_mode(void);

// Save interface mode to NVS
void ui_save_interface_mode(interface_mode_t mode);

void ui_screens_init(void);
interface_mode_t ui_wait_for_mode_selection(void);
void ui_build_app_screens(void);
void ui_post_event(matter_event_t event);
void ui_update_device_state(uint64_t node_id);
void ui_show_dashboard(void);

#ifdef __cplusplus
}
#endif
