#include "device_manager.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dev_mgr";
static const char *NVS_NAMESPACE = "matter_dev";

static matter_device_t s_devices[MATTER_DEVICE_MAX];
static int s_device_count = 0;
static uint64_t s_next_node_id = 1;

device_category_t device_type_to_category(uint32_t device_type_id) {
    switch (device_type_id) {
    case 0x0100: return DEVICE_CAT_ON_OFF_LIGHT;
    case 0x0101: return DEVICE_CAT_DIMMABLE_LIGHT;
    case 0x0102: return DEVICE_CAT_COLOR_TEMP_LIGHT;
    case 0x010D: return DEVICE_CAT_COLOR_LIGHT;
    case 0x010A: return DEVICE_CAT_ON_OFF_PLUG;
    case 0x010B: return DEVICE_CAT_DIMMABLE_PLUG;
    case 0x0301: return DEVICE_CAT_THERMOSTAT;
    case 0x000A: return DEVICE_CAT_DOOR_LOCK;
    case 0x0202: return DEVICE_CAT_WINDOW_COVERING;
    case 0x0015: return DEVICE_CAT_CONTACT_SENSOR;
    case 0x0302: return DEVICE_CAT_TEMP_SENSOR;
    case 0x0307: return DEVICE_CAT_HUMIDITY_SENSOR;
    case 0x0107: return DEVICE_CAT_OCCUPANCY_SENSOR;
    case 0x0106: return DEVICE_CAT_LIGHT_SENSOR;
    case 0x0103: return DEVICE_CAT_ON_OFF_SWITCH;
    case 0x002B: return DEVICE_CAT_FAN;
    default:     return DEVICE_CAT_UNKNOWN;
    }
}

esp_err_t device_manager_init(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved devices found");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    uint8_t count = 0;
    err = nvs_get_u8(nvs, "dev_count", &count);
    if (err == ESP_OK && count <= MATTER_DEVICE_MAX) {
        for (int i = 0; i < count; i++) {
            char key[8];
            snprintf(key, sizeof(key), "dev_%d", i);
            size_t len = sizeof(matter_device_t);
            err = nvs_get_blob(nvs, key, &s_devices[i], &len);
            if (err != ESP_OK || len != sizeof(matter_device_t)) {
                ESP_LOGW(TAG, "Failed to load device %d (err=%d, "
                         "len=%u vs %u)", i, err, (unsigned)len,
                         (unsigned)sizeof(matter_device_t));
                break;
            }
            s_devices[i].reachable = false;
            s_device_count = i + 1;
        }
    }

    uint64_t next = 0;
    if (nvs_get_u64(nvs, "next_node", &next) == ESP_OK) {
        s_next_node_id = next;
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded %d devices, next node_id=%llu",
             s_device_count, (unsigned long long)s_next_node_id);
    return ESP_OK;
}

int device_manager_count(void) {
    return s_device_count;
}

const matter_device_t *device_manager_get(int index) {
    if (index < 0 || index >= s_device_count) return NULL;
    return &s_devices[index];
}

matter_device_t *device_manager_get_mut(int index) {
    if (index < 0 || index >= s_device_count) return NULL;
    return &s_devices[index];
}

const matter_device_t *device_manager_find(uint64_t node_id) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].node_id == node_id) return &s_devices[i];
    }
    return NULL;
}

matter_device_t *device_manager_find_mut(uint64_t node_id) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].node_id == node_id) return &s_devices[i];
    }
    return NULL;
}

esp_err_t device_manager_add(
    uint64_t node_id, uint16_t endpoint_id, const char *name,
    uint32_t device_type_id) {
    if (s_device_count >= MATTER_DEVICE_MAX) {
        ESP_LOGW(TAG, "Device list full");
        return ESP_ERR_NO_MEM;
    }
    if (device_manager_find(node_id)) {
        ESP_LOGW(TAG, "Device already exists");
        return ESP_ERR_INVALID_STATE;
    }
    matter_device_t *dev = &s_devices[s_device_count];
    memset(dev, 0, sizeof(*dev));
    dev->node_id = node_id;
    dev->endpoint_id = endpoint_id;
    dev->device_type_id = device_type_id;
    dev->category = device_type_to_category(device_type_id);
    dev->reachable = true;
    dev->on_off = false;
    if (name && name[0]) {
        strncpy(dev->name, name, MATTER_DEVICE_NAME_LEN - 1);
    } else {
        snprintf(dev->name, MATTER_DEVICE_NAME_LEN,
                 "Device %llu", (unsigned long long)node_id);
    }
    s_device_count++;
    ESP_LOGI(TAG, "Added device 0x%llx type=0x%04lx cat=%d",
             (unsigned long long)node_id,
             (unsigned long)device_type_id, dev->category);
    return device_manager_save();
}

esp_err_t device_manager_remove(uint64_t node_id) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].node_id == node_id) {
            for (int j = i; j < s_device_count - 1; j++) {
                s_devices[j] = s_devices[j + 1];
            }
            s_device_count--;
            memset(&s_devices[s_device_count], 0,
                   sizeof(matter_device_t));
            return device_manager_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_rename(uint64_t node_id, const char *name) {
    matter_device_t *dev = device_manager_find_mut(node_id);
    if (!dev) return ESP_ERR_NOT_FOUND;
    strncpy(dev->name, name, MATTER_DEVICE_NAME_LEN - 1);
    dev->name[MATTER_DEVICE_NAME_LEN - 1] = '\0';
    return ESP_OK;
}

esp_err_t device_manager_save(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_u8(nvs, "dev_count", (uint8_t)s_device_count);
    for (int i = 0; i < s_device_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "dev_%d", i);
        nvs_set_blob(nvs, key, &s_devices[i],
                     sizeof(matter_device_t));
    }
    for (int i = s_device_count; i < MATTER_DEVICE_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "dev_%d", i);
        nvs_erase_key(nvs, key);
    }
    nvs_set_u64(nvs, "next_node", s_next_node_id);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

uint64_t device_manager_next_node_id(void) {
    uint64_t id = s_next_node_id++;
    device_manager_save();
    return id;
}
