#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MATTER_EVENT_PASE_SUCCESS,
    MATTER_EVENT_PASE_FAILED,
    MATTER_EVENT_COMMISSION_SUCCESS,
    MATTER_EVENT_COMMISSION_FAILED,
    MATTER_EVENT_STACK_READY,
    MATTER_EVENT_ATTESTATION_WARNING,
    MATTER_EVENT_COMMISSION_TIMEOUT,
} matter_event_type_t;

typedef struct {
    matter_event_type_t type;
    uint64_t            node_id;
} matter_event_t;

typedef void (*matter_event_cb_t)(matter_event_t event);

esp_err_t matter_init(matter_event_cb_t cb);

#ifdef __cplusplus
}
#endif
