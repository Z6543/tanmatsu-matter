#pragma once

#include "matter_init.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_screens_init(void);
void ui_post_event(matter_event_t event);
void ui_update_device_state(uint64_t node_id, bool on_off);
void ui_show_dashboard(void);

#ifdef __cplusplus
}
#endif
