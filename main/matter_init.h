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
    MATTER_EVENT_DEVICE_INFO_READY,
    MATTER_EVENT_THREAD_BR_ERROR,
} matter_event_type_t;

#define MATTER_EVENT_MSG_LEN 96

typedef struct {
    matter_event_type_t type;
    uint64_t            node_id;
    uint16_t            endpoint_id;
    char                msg[MATTER_EVENT_MSG_LEN];
} matter_event_t;

typedef void (*matter_event_cb_t)(matter_event_t event);

esp_err_t matter_init(matter_event_cb_t cb);

bool matter_thread_available(void);
esp_err_t matter_start_thread_br(void);
esp_err_t matter_stop_thread_br(void);

/**
 * Get the active Thread operational dataset as a hex TLV string.
 *
 * @param out     Buffer to write the hex string into (null-terminated).
 * @param out_len Size of the output buffer. Must be at least 509 bytes
 *                (254 TLV bytes * 2 hex chars + null terminator).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no active dataset,
 *         ESP_ERR_INVALID_SIZE if the buffer is too small.
 */
esp_err_t matter_get_thread_active_dataset_hex(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
