#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MATTER_DEVICE_MAX       5
#define MATTER_DEVICE_NAME_LEN  32

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    char     name[MATTER_DEVICE_NAME_LEN];
    bool     on_off;
    bool     reachable;
} matter_device_t;

esp_err_t device_manager_init(void);
int       device_manager_count(void);
const matter_device_t *device_manager_get(int index);
matter_device_t       *device_manager_get_mut(int index);
const matter_device_t *device_manager_find(uint64_t node_id);
matter_device_t       *device_manager_find_mut(uint64_t node_id);
esp_err_t device_manager_add(uint64_t node_id, uint16_t endpoint_id, const char *name);
esp_err_t device_manager_remove(uint64_t node_id);
esp_err_t device_manager_rename(uint64_t node_id, const char *name);
esp_err_t device_manager_save(void);
uint64_t  device_manager_next_node_id(void);

#ifdef __cplusplus
}
#endif
