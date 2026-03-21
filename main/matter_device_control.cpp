#include "matter_device_control.h"
#include "device_manager.h"
#include "matter_init.h"
#include "ui_screens.h"

#include <string.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_timer.h>

static const char *TAG = "matter_ctrl";

// Cluster IDs
static const uint32_t ONOFF_CLUSTER       = 0x0006;
static const uint32_t LEVEL_CLUSTER       = 0x0008;
static const uint32_t DESCRIPTOR_CLUSTER  = 0x001D;
static const uint32_t BASIC_INFO_CLUSTER  = 0x0028;
static const uint32_t BOOLEAN_STATE_CLUSTER = 0x0045;
static const uint32_t DOOR_LOCK_CLUSTER   = 0x0101;
static const uint32_t WINDOW_COVERING_CLUSTER = 0x0102;
static const uint32_t THERMOSTAT_CLUSTER  = 0x0201;
static const uint32_t FAN_CONTROL_CLUSTER = 0x0202;
static const uint32_t COLOR_CONTROL_CLUSTER = 0x0300;
static const uint32_t TEMP_MEASURE_CLUSTER = 0x0402;
static const uint32_t HUMIDITY_MEASURE_CLUSTER = 0x0405;
static const uint32_t OCCUPANCY_CLUSTER   = 0x0406;
static const uint32_t ILLUMINANCE_CLUSTER = 0x0400;

// Common attribute IDs
static const uint32_t ATTR_ONOFF           = 0x0000;
static const uint32_t ATTR_CURRENT_LEVEL   = 0x0000;
static const uint32_t ATTR_CURRENT_HUE     = 0x0000;
static const uint32_t ATTR_CURRENT_SAT     = 0x0001;
static const uint32_t ATTR_COLOR_TEMP      = 0x0007;
static const uint32_t ATTR_LOCK_STATE      = 0x0000;
static const uint32_t ATTR_COVER_LIFT_PCT  = 0x000E;
static const uint32_t ATTR_COVER_TILT_PCT  = 0x000F;
static const uint32_t ATTR_LOCAL_TEMP      = 0x0000;
static const uint32_t ATTR_COOL_SETPOINT   = 0x0011;
static const uint32_t ATTR_HEAT_SETPOINT   = 0x0012;
static const uint32_t ATTR_SYSTEM_MODE     = 0x001C;
static const uint32_t ATTR_FAN_MODE        = 0x0000;
static const uint32_t ATTR_MEASURED_VALUE   = 0x0000;
static const uint32_t ATTR_OCCUPANCY       = 0x0000;
static const uint32_t ATTR_STATE_VALUE     = 0x0000;

// Descriptor cluster attributes
static const uint32_t DESCRIPTOR_DEVICE_TYPE_LIST = 0x0000;
static const uint32_t DESCRIPTOR_PARTS_LIST = 0x0003;

// Basic Information attributes
static const uint32_t BASIC_VENDOR_NAME   = 0x0001;
static const uint32_t BASIC_PRODUCT_NAME  = 0x0003;
static const uint32_t BASIC_NODE_LABEL    = 0x0005;

// On/Off commands
static const uint32_t CMD_OFF    = 0;
static const uint32_t CMD_ON     = 1;
static const uint32_t CMD_TOGGLE = 2;

// State for post-commissioning device info read
static struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t device_type_id;
    char     vendor_name[32];
    char     product_name[32];
    char     node_label[32];
    bool     active;
    uint8_t  reads_pending;
} s_info_read;

static matter_device_state_cb_t s_state_cb = nullptr;

// ---- Attribute report handler (subscriptions) ----

static void on_attribute_report(
    uint64_t node_id,
    const chip::app::ConcreteDataAttributePath &path,
    chip::TLV::TLVReader *data) {
    if (!data) return;

    matter_device_t *dev = device_manager_find_mut(node_id);
    if (!dev) return;

    uint32_t cluster = path.mClusterId;
    uint32_t attr = path.mAttributeId;

    if (cluster == ONOFF_CLUSTER && attr == ATTR_ONOFF) {
        bool val = false;
        if (data->GetType() == chip::TLV::kTLVType_Boolean) {
            data->Get(val);
        }
        dev->on_off = val;
        dev->reachable = true;
        ESP_LOGI(TAG, "Node 0x%llx OnOff=%d",
                 (unsigned long long)node_id, val);
    } else if (cluster == LEVEL_CLUSTER && attr == ATTR_CURRENT_LEVEL) {
        uint8_t val = 0;
        data->Get(val);
        dev->level = val;
        dev->reachable = true;
    } else if (cluster == COLOR_CONTROL_CLUSTER) {
        if (attr == ATTR_CURRENT_HUE) {
            uint8_t val = 0; data->Get(val); dev->hue = val;
        } else if (attr == ATTR_CURRENT_SAT) {
            uint8_t val = 0; data->Get(val); dev->saturation = val;
        } else if (attr == ATTR_COLOR_TEMP) {
            uint16_t val = 0; data->Get(val);
            dev->color_temp_mireds = val;
        }
        dev->reachable = true;
    } else if (cluster == DOOR_LOCK_CLUSTER && attr == ATTR_LOCK_STATE) {
        uint8_t val = 0;
        data->Get(val);
        dev->lock_state = val;
        dev->reachable = true;
    } else if (cluster == THERMOSTAT_CLUSTER) {
        if (attr == ATTR_LOCAL_TEMP) {
            int16_t val = 0; data->Get(val); dev->local_temp = val;
        } else if (attr == ATTR_HEAT_SETPOINT) {
            int16_t val = 0; data->Get(val); dev->setpoint_heat = val;
        } else if (attr == ATTR_COOL_SETPOINT) {
            int16_t val = 0; data->Get(val); dev->setpoint_cool = val;
        } else if (attr == ATTR_SYSTEM_MODE) {
            uint8_t val = 0; data->Get(val);
            dev->thermostat_mode = val;
        }
        dev->reachable = true;
    } else if (cluster == WINDOW_COVERING_CLUSTER) {
        if (attr == ATTR_COVER_LIFT_PCT) {
            uint16_t val = 0; data->Get(val);
            dev->cover_position = (uint8_t)(val / 100);
        } else if (attr == ATTR_COVER_TILT_PCT) {
            uint16_t val = 0; data->Get(val);
            dev->cover_tilt = (uint8_t)(val / 100);
        }
        dev->reachable = true;
    } else if (cluster == FAN_CONTROL_CLUSTER && attr == ATTR_FAN_MODE) {
        uint8_t val = 0; data->Get(val);
        dev->thermostat_mode = val;  // reuse field for fan mode
        dev->reachable = true;
    } else if (cluster == TEMP_MEASURE_CLUSTER &&
               attr == ATTR_MEASURED_VALUE) {
        int16_t val = 0; data->Get(val); dev->temperature = val;
        dev->reachable = true;
    } else if (cluster == HUMIDITY_MEASURE_CLUSTER &&
               attr == ATTR_MEASURED_VALUE) {
        uint16_t val = 0; data->Get(val); dev->humidity = val;
        dev->reachable = true;
    } else if (cluster == OCCUPANCY_CLUSTER &&
               attr == ATTR_OCCUPANCY) {
        uint8_t val = 0; data->Get(val);
        dev->occupancy = (val & 0x01) != 0;
        dev->reachable = true;
    } else if (cluster == BOOLEAN_STATE_CLUSTER &&
               attr == ATTR_STATE_VALUE) {
        bool val = false;
        if (data->GetType() == chip::TLV::kTLVType_Boolean) {
            data->Get(val);
        }
        dev->contact = val;
        dev->reachable = true;
    } else if (cluster == ILLUMINANCE_CLUSTER &&
               attr == ATTR_MEASURED_VALUE) {
        uint16_t val = 0; data->Get(val); dev->illuminance = val;
        dev->reachable = true;
    } else {
        return;  // unknown, don't notify
    }

    if (s_state_cb) {
        s_state_cb(node_id);
    }
}

static void on_subscribe_done(uint64_t node_id,
                               uint32_t subscription_id) {
    ESP_LOGI(TAG, "Subscription established for node 0x%llx, "
             "sub_id=%lu",
             (unsigned long long)node_id,
             (unsigned long)subscription_id);
    matter_device_t *dev = device_manager_find_mut(node_id);
    if (dev) {
        dev->reachable = true;
        if (s_state_cb) s_state_cb(node_id);
    }
}

void matter_device_set_state_cb(matter_device_state_cb_t cb) {
    s_state_cb = cb;
}

// ---- On/Off commands ----

static esp_err_t send_onoff_cmd(
    uint64_t node_id, uint16_t ep, uint32_t cmd) {
    ESP_LOGI(TAG, "Sending OnOff cmd=%lu to node 0x%llx ep%u",
             (unsigned long)cmd, (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, ONOFF_CLUSTER, cmd, nullptr);
}

esp_err_t matter_device_send_on(uint64_t node_id, uint16_t ep) {
    return send_onoff_cmd(node_id, ep, CMD_ON);
}

esp_err_t matter_device_send_off(uint64_t node_id, uint16_t ep) {
    return send_onoff_cmd(node_id, ep, CMD_OFF);
}

esp_err_t matter_device_send_toggle(uint64_t node_id, uint16_t ep) {
    return send_onoff_cmd(node_id, ep, CMD_TOGGLE);
}

// ---- Level control ----

esp_err_t matter_device_send_level(
    uint64_t node_id, uint16_t ep, uint8_t level) {
    ESP_LOGI(TAG, "Sending MoveToLevel %u to 0x%llx ep%u",
             level, (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // MoveToLevel command (0x0000): level(u8), transitionTime(u16),
    // optionsMask(u8), optionsOverride(u8)
    char data[64];
    snprintf(data, sizeof(data),
             "0:%u 1:0 2:0 3:0", level);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, LEVEL_CLUSTER, 0x0000, data);
}

// ---- Color control ----

esp_err_t matter_device_send_color_temp(
    uint64_t node_id, uint16_t ep, uint16_t mireds) {
    ESP_LOGI(TAG, "Sending MoveToColorTemp %u to 0x%llx ep%u",
             mireds, (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // MoveToColorTemperature (0x000A): colorTemperatureMireds(u16),
    // transitionTime(u16), optionsMask(u8), optionsOverride(u8)
    char data[64];
    snprintf(data, sizeof(data),
             "0:%u 1:0 2:0 3:0", mireds);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, COLOR_CONTROL_CLUSTER, 0x000A, data);
}

esp_err_t matter_device_send_hue_sat(
    uint64_t node_id, uint16_t ep, uint8_t hue, uint8_t sat) {
    ESP_LOGI(TAG, "Sending MoveToHueSat %u/%u to 0x%llx ep%u",
             hue, sat, (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // MoveToHueAndSaturation (0x0006): hue(u8), saturation(u8),
    // transitionTime(u16), optionsMask(u8), optionsOverride(u8)
    char data[64];
    snprintf(data, sizeof(data),
             "0:%u 1:%u 2:0 3:0 4:0", hue, sat);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, COLOR_CONTROL_CLUSTER, 0x0006, data);
}

// ---- Door lock ----

esp_err_t matter_device_send_lock(uint64_t node_id, uint16_t ep) {
    ESP_LOGI(TAG, "Sending LockDoor to 0x%llx ep%u",
             (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    // LockDoor (0x0000) - timed invoke required
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, DOOR_LOCK_CLUSTER, 0x0000, nullptr);
}

esp_err_t matter_device_send_unlock(uint64_t node_id, uint16_t ep) {
    ESP_LOGI(TAG, "Sending UnlockDoor to 0x%llx ep%u",
             (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    // UnlockDoor (0x0001) - timed invoke required
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, DOOR_LOCK_CLUSTER, 0x0001, nullptr);
}

// ---- Thermostat ----

esp_err_t matter_device_send_setpoint(
    uint64_t node_id, uint16_t ep,
    int16_t heat, int16_t cool) {
    ESP_LOGI(TAG, "Sending setpoints heat=%d cool=%d to 0x%llx",
             heat, cool, (unsigned long long)node_id);
    // Write OccupiedHeatingSetpoint and OccupiedCoolingSetpoint
    // directly via attribute write would be better, but we use
    // SetpointRaiseLower command for simplicity
    // For now, write attributes directly
    matter_device_t *dev = device_manager_find_mut(node_id);
    if (dev) {
        dev->setpoint_heat = heat;
        dev->setpoint_cool = cool;
    }
    // TODO: implement attribute write when esp-matter supports it
    return ESP_OK;
}

esp_err_t matter_device_send_thermo_mode(
    uint64_t node_id, uint16_t ep, uint8_t mode) {
    ESP_LOGI(TAG, "Sending thermostat mode=%u to 0x%llx",
             mode, (unsigned long long)node_id);
    matter_device_t *dev = device_manager_find_mut(node_id);
    if (dev) dev->thermostat_mode = mode;
    // TODO: implement attribute write
    return ESP_OK;
}

// ---- Window covering ----

esp_err_t matter_device_send_cover_pos(
    uint64_t node_id, uint16_t ep, uint8_t pct) {
    ESP_LOGI(TAG, "Sending GoToLiftPct %u to 0x%llx ep%u",
             pct, (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    // GoToLiftPercentage (0x0005): pct100ths(u16)
    char data[32];
    snprintf(data, sizeof(data), "0:%u", (uint16_t)pct * 100);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, WINDOW_COVERING_CLUSTER, 0x0005, data);
}

esp_err_t matter_device_send_cover_open(
    uint64_t node_id, uint16_t ep) {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, WINDOW_COVERING_CLUSTER, 0x0000, nullptr);
}

esp_err_t matter_device_send_cover_close(
    uint64_t node_id, uint16_t ep) {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, WINDOW_COVERING_CLUSTER, 0x0001, nullptr);
}

esp_err_t matter_device_send_cover_stop(
    uint64_t node_id, uint16_t ep) {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, ep, WINDOW_COVERING_CLUSTER, 0x0002, nullptr);
}

// ---- Fan ----

esp_err_t matter_device_send_fan_mode(
    uint64_t node_id, uint16_t ep, uint8_t mode) {
    ESP_LOGI(TAG, "Sending fan mode=%u to 0x%llx",
             mode, (unsigned long long)node_id);
    matter_device_t *dev = device_manager_find_mut(node_id);
    if (dev) dev->thermostat_mode = mode;  // reused field
    // TODO: implement attribute write
    return ESP_OK;
}

// ---- Type-aware subscribe ----

static esp_err_t subscribe_attr(
    uint64_t node_id, uint16_t ep,
    uint32_t cluster, uint32_t attr) {
    auto *sub = new esp_matter::controller::subscribe_command(
        node_id, ep, cluster, attr,
        esp_matter::controller::SUBSCRIBE_ATTRIBUTE,
        1, 10, true,
        on_attribute_report, nullptr,
        on_subscribe_done, nullptr, true);
    return sub->send_command();
}

esp_err_t matter_device_subscribe_onoff(
    uint64_t node_id, uint16_t ep) {
    ESP_LOGI(TAG, "Subscribing OnOff for node 0x%llx ep%u",
             (unsigned long long)node_id, ep);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return subscribe_attr(node_id, ep, ONOFF_CLUSTER, ATTR_ONOFF);
}

esp_err_t matter_device_subscribe(
    uint64_t node_id, uint16_t ep, device_category_t cat) {
    ESP_LOGI(TAG, "Type-aware subscribe for 0x%llx ep%u cat=%d",
             (unsigned long long)node_id, ep, cat);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // Always subscribe to OnOff for devices that have it
    switch (cat) {
    case DEVICE_CAT_ON_OFF_LIGHT:
    case DEVICE_CAT_DIMMABLE_LIGHT:
    case DEVICE_CAT_COLOR_TEMP_LIGHT:
    case DEVICE_CAT_COLOR_LIGHT:
    case DEVICE_CAT_ON_OFF_PLUG:
    case DEVICE_CAT_DIMMABLE_PLUG:
    case DEVICE_CAT_FAN:
    case DEVICE_CAT_ON_OFF_SWITCH:
    case DEVICE_CAT_UNKNOWN:
        subscribe_attr(node_id, ep, ONOFF_CLUSTER, ATTR_ONOFF);
        break;
    default:
        break;
    }

    // Category-specific subscriptions
    switch (cat) {
    case DEVICE_CAT_DIMMABLE_LIGHT:
    case DEVICE_CAT_COLOR_TEMP_LIGHT:
    case DEVICE_CAT_COLOR_LIGHT:
    case DEVICE_CAT_DIMMABLE_PLUG:
        subscribe_attr(node_id, ep,
                       LEVEL_CLUSTER, ATTR_CURRENT_LEVEL);
        break;
    default:
        break;
    }

    switch (cat) {
    case DEVICE_CAT_COLOR_TEMP_LIGHT:
        subscribe_attr(node_id, ep,
                       COLOR_CONTROL_CLUSTER, ATTR_COLOR_TEMP);
        break;
    case DEVICE_CAT_COLOR_LIGHT:
        subscribe_attr(node_id, ep,
                       COLOR_CONTROL_CLUSTER, ATTR_CURRENT_HUE);
        subscribe_attr(node_id, ep,
                       COLOR_CONTROL_CLUSTER, ATTR_CURRENT_SAT);
        subscribe_attr(node_id, ep,
                       COLOR_CONTROL_CLUSTER, ATTR_COLOR_TEMP);
        break;
    default:
        break;
    }

    switch (cat) {
    case DEVICE_CAT_DOOR_LOCK:
        subscribe_attr(node_id, ep,
                       DOOR_LOCK_CLUSTER, ATTR_LOCK_STATE);
        break;
    case DEVICE_CAT_THERMOSTAT:
        subscribe_attr(node_id, ep,
                       THERMOSTAT_CLUSTER, ATTR_LOCAL_TEMP);
        subscribe_attr(node_id, ep,
                       THERMOSTAT_CLUSTER, ATTR_HEAT_SETPOINT);
        subscribe_attr(node_id, ep,
                       THERMOSTAT_CLUSTER, ATTR_COOL_SETPOINT);
        subscribe_attr(node_id, ep,
                       THERMOSTAT_CLUSTER, ATTR_SYSTEM_MODE);
        break;
    case DEVICE_CAT_WINDOW_COVERING:
        subscribe_attr(node_id, ep,
                       WINDOW_COVERING_CLUSTER, ATTR_COVER_LIFT_PCT);
        break;
    case DEVICE_CAT_FAN:
        subscribe_attr(node_id, ep,
                       FAN_CONTROL_CLUSTER, ATTR_FAN_MODE);
        break;
    case DEVICE_CAT_TEMP_SENSOR:
        subscribe_attr(node_id, ep,
                       TEMP_MEASURE_CLUSTER, ATTR_MEASURED_VALUE);
        break;
    case DEVICE_CAT_HUMIDITY_SENSOR:
        subscribe_attr(node_id, ep,
                       HUMIDITY_MEASURE_CLUSTER, ATTR_MEASURED_VALUE);
        break;
    case DEVICE_CAT_OCCUPANCY_SENSOR:
        subscribe_attr(node_id, ep,
                       OCCUPANCY_CLUSTER, ATTR_OCCUPANCY);
        break;
    case DEVICE_CAT_CONTACT_SENSOR:
        subscribe_attr(node_id, ep,
                       BOOLEAN_STATE_CLUSTER, ATTR_STATE_VALUE);
        break;
    case DEVICE_CAT_LIGHT_SENSOR:
        subscribe_attr(node_id, ep,
                       ILLUMINANCE_CLUSTER, ATTR_MEASURED_VALUE);
        break;
    default:
        break;
    }

    return ESP_OK;
}

esp_err_t matter_device_subscribe_all(void) {
    int count = device_manager_count();
    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (dev) {
            matter_device_subscribe(
                dev->node_id, dev->endpoint_id, dev->category);
        }
    }
    return ESP_OK;
}

esp_err_t matter_device_subscribe_wifi(void) {
    int count = device_manager_count();
    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (dev && !dev->is_thread) {
            matter_device_subscribe(
                dev->node_id, dev->endpoint_id, dev->category);
        }
    }
    return ESP_OK;
}

esp_err_t matter_device_subscribe_thread(void) {
    int count = device_manager_count();
    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (dev && dev->is_thread) {
            matter_device_subscribe(
                dev->node_id, dev->endpoint_id, dev->category);
        }
    }
    return ESP_OK;
}

// Delay Thread subscriptions to allow the Thread network to form
// after border router init (~15s for leader election + routing).
#define THREAD_SUBSCRIBE_DELAY_US (15ULL * 1000000ULL)
static esp_timer_handle_t s_thread_sub_timer = NULL;

static void thread_subscribe_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Thread network settle time elapsed, "
             "subscribing to Thread devices");
    matter_device_subscribe_thread();
}

esp_err_t matter_device_subscribe_thread_delayed(void) {
    if (!s_thread_sub_timer) {
        esp_timer_create_args_t args = {};
        args.callback = thread_subscribe_timer_cb;
        args.name = "thread_sub";
        esp_err_t err = esp_timer_create(&args, &s_thread_sub_timer);
        if (err != ESP_OK) return err;
    }
    esp_timer_stop(s_thread_sub_timer);
    ESP_LOGI(TAG, "Thread device subscribe scheduled in 15s");
    return esp_timer_start_once(
        s_thread_sub_timer, THREAD_SUBSCRIBE_DELAY_US);
}

// ---- Periodic reconnect for unreachable devices ----

#define RECONNECT_INTERVAL_US (30ULL * 1000000ULL)
static esp_timer_handle_t s_reconnect_timer = NULL;

static void reconnect_timer_cb(void *arg) {
    (void)arg;
    int count = device_manager_count();
    bool any_unreachable = false;
    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (dev && !dev->reachable) {
            if (dev->is_thread && !matter_thread_available()) {
                continue;
            }
            ESP_LOGI(TAG, "Retrying subscribe for unreachable "
                     "node 0x%llx",
                     (unsigned long long)dev->node_id);
            matter_device_subscribe(
                dev->node_id, dev->endpoint_id, dev->category);
            any_unreachable = true;
        }
    }
    if (!any_unreachable) {
        ESP_LOGI(TAG, "All devices reachable, stopping "
                 "reconnect timer");
        esp_timer_stop(s_reconnect_timer);
    }
}

esp_err_t matter_device_start_reconnect_timer(void) {
    if (!s_reconnect_timer) {
        esp_timer_create_args_t args = {};
        args.callback = reconnect_timer_cb;
        args.name = "dev_reconn";
        esp_err_t err = esp_timer_create(
            &args, &s_reconnect_timer);
        if (err != ESP_OK) return err;
    }
    esp_timer_stop(s_reconnect_timer);
    return esp_timer_start_periodic(
        s_reconnect_timer, RECONNECT_INTERVAL_US);
}

esp_err_t matter_device_stop_reconnect_timer(void) {
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    return ESP_OK;
}

// ---- Post-commissioning device info read ----

static void info_read_complete(void) {
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_DEVICE_INFO_READY;
    ev.node_id = s_info_read.node_id;
    ev.endpoint_id = s_info_read.endpoint_id;
    ev.device_type_id = s_info_read.device_type_id;

    if (s_info_read.node_label[0]) {
        strncpy(ev.msg, s_info_read.node_label,
                MATTER_EVENT_MSG_LEN - 1);
    } else if (s_info_read.product_name[0]) {
        if (s_info_read.vendor_name[0]) {
            snprintf(ev.msg, MATTER_EVENT_MSG_LEN,
                     "%s %s", s_info_read.vendor_name,
                     s_info_read.product_name);
        } else {
            strncpy(ev.msg, s_info_read.product_name,
                    MATTER_EVENT_MSG_LEN - 1);
        }
    }

    s_info_read.active = false;
    ui_post_event(ev);
}

static void check_info_reads_done(void) {
    if (--s_info_read.reads_pending == 0) {
        info_read_complete();
    }
}

static void on_descriptor_report(
    uint64_t remote_node_id,
    const chip::app::ConcreteDataAttributePath &path,
    chip::TLV::TLVReader *data) {
    if (path.mClusterId != DESCRIPTOR_CLUSTER || !data) return;

    if (path.mAttributeId == DESCRIPTOR_PARTS_LIST) {
        // PartsList is an array of endpoint IDs
        chip::TLV::TLVType container;
        if (data->GetType() != chip::TLV::kTLVType_Array) return;
        if (data->EnterContainer(container) != CHIP_NO_ERROR) return;

        uint16_t first_ep = 1;
        bool found = false;
        while (data->Next() == CHIP_NO_ERROR) {
            uint16_t ep = 0;
            if (data->Get(ep) == CHIP_NO_ERROR && ep != 0) {
                if (!found) {
                    first_ep = ep;
                    found = true;
                    ESP_LOGI(TAG, "Node 0x%llx: first app ep=%u",
                             (unsigned long long)remote_node_id, ep);
                }
            }
        }
        data->ExitContainer(container);
        s_info_read.endpoint_id = first_ep;
    }
}

static void on_descriptor_done(
    uint64_t remote_node_id,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::AttributePathParams> &,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::EventPathParams> &) {
    ESP_LOGI(TAG, "Descriptor read done for 0x%llx, ep=%u",
             (unsigned long long)remote_node_id,
             s_info_read.endpoint_id);
    check_info_reads_done();
}

static void on_device_type_report(
    uint64_t remote_node_id,
    const chip::app::ConcreteDataAttributePath &path,
    chip::TLV::TLVReader *data) {
    if (path.mClusterId != DESCRIPTOR_CLUSTER ||
        path.mAttributeId != DESCRIPTOR_DEVICE_TYPE_LIST || !data) {
        return;
    }

    // DeviceTypeList is array of {deviceType(u32), revision(u16)}
    chip::TLV::TLVType array_type;
    if (data->GetType() != chip::TLV::kTLVType_Array) return;
    if (data->EnterContainer(array_type) != CHIP_NO_ERROR) return;

    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->GetType() != chip::TLV::kTLVType_Structure) {
            continue;
        }
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR) {
            continue;
        }
        while (data->Next() == CHIP_NO_ERROR) {
            chip::TLV::Tag tag = data->GetTag();
            if (chip::TLV::TagNumFromTag(tag) == 0) {
                uint32_t dt = 0;
                if (data->Get(dt) == CHIP_NO_ERROR) {
                    ESP_LOGI(TAG, "Node 0x%llx ep%u: "
                             "device type=0x%04lx",
                             (unsigned long long)remote_node_id,
                             path.mEndpointId,
                             (unsigned long)dt);
                    if (s_info_read.device_type_id == 0) {
                        s_info_read.device_type_id = dt;
                    }
                }
            }
        }
        data->ExitContainer(struct_type);
    }
    data->ExitContainer(array_type);
}

static void on_device_type_done(
    uint64_t remote_node_id,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::AttributePathParams> &,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::EventPathParams> &) {
    ESP_LOGI(TAG, "Device type read done for 0x%llx, type=0x%04lx",
             (unsigned long long)remote_node_id,
             (unsigned long)s_info_read.device_type_id);
    check_info_reads_done();
}

static void copy_tlv_string(chip::TLV::TLVReader *data,
                             char *buf, size_t buf_len) {
    uint32_t len = data->GetLength();
    if (len >= buf_len) len = buf_len - 1;
    const uint8_t *str = nullptr;
    if (data->GetDataPtr(str) == CHIP_NO_ERROR && str) {
        memcpy(buf, str, len);
        buf[len] = '\0';
    }
}

static void on_basic_info_report(
    uint64_t remote_node_id,
    const chip::app::ConcreteDataAttributePath &path,
    chip::TLV::TLVReader *data) {
    if (path.mClusterId != BASIC_INFO_CLUSTER || !data) return;

    if (path.mAttributeId == BASIC_VENDOR_NAME) {
        copy_tlv_string(data, s_info_read.vendor_name,
                        sizeof(s_info_read.vendor_name));
        ESP_LOGI(TAG, "VendorName: %s", s_info_read.vendor_name);
    } else if (path.mAttributeId == BASIC_PRODUCT_NAME) {
        copy_tlv_string(data, s_info_read.product_name,
                        sizeof(s_info_read.product_name));
        ESP_LOGI(TAG, "ProductName: %s", s_info_read.product_name);
    } else if (path.mAttributeId == BASIC_NODE_LABEL) {
        copy_tlv_string(data, s_info_read.node_label,
                        sizeof(s_info_read.node_label));
        ESP_LOGI(TAG, "NodeLabel: %s", s_info_read.node_label);
    }
}

static void on_basic_info_done(
    uint64_t remote_node_id,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::AttributePathParams> &,
    const chip::Platform::ScopedMemoryBufferWithSize<
        chip::app::EventPathParams> &) {
    ESP_LOGI(TAG, "Basic info read done for 0x%llx",
             (unsigned long long)remote_node_id);
    check_info_reads_done();
}

esp_err_t matter_device_read_info(uint64_t node_id) {
    if (s_info_read.active) {
        ESP_LOGW(TAG, "Info read already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_info_read, 0, sizeof(s_info_read));
    s_info_read.node_id = node_id;
    s_info_read.endpoint_id = 1;  // fallback
    s_info_read.active = true;
    s_info_read.reads_pending = 3;  // descriptor + basic info + device type

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // Read 1: Descriptor PartsList from endpoint 0
    auto *desc_read = new esp_matter::controller::read_command(
        node_id, 0, DESCRIPTOR_CLUSTER,
        DESCRIPTOR_PARTS_LIST,
        esp_matter::controller::READ_ATTRIBUTE,
        on_descriptor_report, on_descriptor_done, nullptr);
    esp_err_t err = desc_read->send_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send descriptor read");
        s_info_read.active = false;
        return err;
    }

    // Read 2: Basic Information from endpoint 0
    auto *info_read = new esp_matter::controller::read_command(
        node_id, 0, BASIC_INFO_CLUSTER,
        0xFFFFFFFF,  // wildcard
        esp_matter::controller::READ_ATTRIBUTE,
        on_basic_info_report, on_basic_info_done, nullptr);
    err = info_read->send_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send basic info read");
        s_info_read.reads_pending--;
    }

    // Read 3: DeviceTypeList from the app endpoint
    // We read from endpoint 1 as default; the descriptor read
    // above will tell us the actual endpoint, but that's async.
    // Reading DeviceTypeList from ep 1 covers the common case.
    auto *dt_read = new esp_matter::controller::read_command(
        node_id, 1, DESCRIPTOR_CLUSTER,
        DESCRIPTOR_DEVICE_TYPE_LIST,
        esp_matter::controller::READ_ATTRIBUTE,
        on_device_type_report, on_device_type_done, nullptr);
    err = dt_read->send_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send device type read");
        s_info_read.reads_pending--;
    }

    // If all optional reads failed, complete now
    if (s_info_read.reads_pending == 0) {
        info_read_complete();
    }

    return ESP_OK;
}
