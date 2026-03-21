#include "ui_screens.h"
#include "device_manager.h"
#include "matter_commission.h"
#include "matter_device_control.h"
#include "matter_init.h"

#include "bsp_lvgl.h"
#include "bsp/input.h"
#include "sdcard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nvs.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if LV_USE_SNAPSHOT
#include "esp_heap_caps.h"
#endif

// Event queue for Matter -> UI communication
static QueueHandle_t s_ui_event_queue = NULL;

// Screen objects
static lv_obj_t *scr_dashboard = NULL;
static lv_obj_t *scr_commission = NULL;
static lv_obj_t *scr_detail = NULL;

// Per-screen input groups
static lv_group_t *grp_dashboard = NULL;
static lv_group_t *grp_commission = NULL;
static lv_group_t *grp_detail = NULL;

// Dashboard widgets
static lv_obj_t *dashboard_container = NULL;
static lv_obj_t *dashboard_status_label = NULL;
static lv_obj_t *dashboard_status_row = NULL;
static lv_obj_t *dashboard_spinner = NULL;
static lv_obj_t *dashboard_thread_btn = NULL;

// Number of fixed header buttons in grp_dashboard (Thread + Add)
#define DASHBOARD_HEADER_BTNS 2

// Commission widgets
// 0=Ethernet, 1=BLE+WiFi, 2=BLE+Thread
#define NUM_COMMISSION_METHODS 3
static lv_obj_t *commission_method_radios[NUM_COMMISSION_METHODS] = {};
static lv_obj_t *commission_code_ta = NULL;
static lv_obj_t *commission_disc_ta = NULL;
static lv_obj_t *commission_disc_label = NULL;
static lv_obj_t *commission_hints_ta = NULL;
static lv_obj_t *commission_hints_label = NULL;
static lv_obj_t *commission_thread_ta = NULL;
static lv_obj_t *commission_thread_label = NULL;
static lv_obj_t *commission_code_label = NULL;
static lv_obj_t *commission_name_ta = NULL;
static lv_obj_t *commission_status_label = NULL;
static lv_obj_t *commission_spinner = NULL;
static lv_obj_t *commission_start_btn = NULL;
static int       commission_method = 0;

// Input sub-mode: 0=QR, 1=Manual, 2=Disc+Pass, 3=PIN
#define NUM_INPUT_MODES 4
static lv_obj_t *commission_input_row = NULL;
static lv_obj_t *commission_input_radios[NUM_INPUT_MODES] = {};
static int       commission_input = 0;

// Detail widgets
static lv_obj_t *detail_name_label = NULL;
static lv_obj_t *detail_state_label = NULL;
static lv_obj_t *detail_info_label = NULL;
static lv_obj_t *detail_rename_ta = NULL;
static lv_obj_t *detail_content = NULL;  // container for type-specific controls
static uint64_t  detail_node_id = 0;

// Commissioning state
static uint64_t s_pending_node_id = 0;
static char     s_pending_name[MATTER_DEVICE_NAME_LEN] = {};
static bool     s_commissioning_active = false;
static bool     s_attestation_warned = false;

// Confirmation dialog state
static lv_obj_t *confirm_dialog = NULL;
static uint64_t  confirm_node_id = 0;
typedef enum { CONFIRM_FORCE_REMOVE, CONFIRM_UNPAIR } confirm_action_t;
static confirm_action_t confirm_action;
static lv_group_t *confirm_prev_group = NULL;
static lv_group_t *confirm_dlg_group = NULL;

// LVGL timer for polling event queue
static lv_timer_t *s_event_timer = NULL;

// NVS key for persisted commission method
static const char *NVS_UI_NS = "matter_ui";

// Forward declarations
static void create_dashboard_screen(void);
static void create_commission_screen(void);
static void create_detail_screen(void);
static void refresh_dashboard(void);
static void show_detail_for_device(uint64_t node_id);
static void apply_focus_style(lv_obj_t *obj);
static void activate_group(lv_group_t *grp);
static void show_spinner(lv_obj_t *spinner, bool show);

// ---- Helpers: category queries ----

static bool cat_has_onoff(device_category_t cat) {
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
        return true;
    default:
        return false;
    }
}

static bool cat_has_level(device_category_t cat) {
    return cat == DEVICE_CAT_DIMMABLE_LIGHT ||
           cat == DEVICE_CAT_COLOR_TEMP_LIGHT ||
           cat == DEVICE_CAT_COLOR_LIGHT ||
           cat == DEVICE_CAT_DIMMABLE_PLUG;
}

static bool cat_is_sensor(device_category_t cat) {
    return cat == DEVICE_CAT_TEMP_SENSOR ||
           cat == DEVICE_CAT_HUMIDITY_SENSOR ||
           cat == DEVICE_CAT_OCCUPANCY_SENSOR ||
           cat == DEVICE_CAT_CONTACT_SENSOR ||
           cat == DEVICE_CAT_LIGHT_SENSOR;
}

static const char *cat_icon(device_category_t cat) {
    switch (cat) {
    case DEVICE_CAT_ON_OFF_LIGHT:
    case DEVICE_CAT_DIMMABLE_LIGHT:
    case DEVICE_CAT_COLOR_TEMP_LIGHT:
    case DEVICE_CAT_COLOR_LIGHT:
        return LV_SYMBOL_IMAGE;
    case DEVICE_CAT_ON_OFF_PLUG:
    case DEVICE_CAT_DIMMABLE_PLUG:
        return LV_SYMBOL_POWER;
    case DEVICE_CAT_THERMOSTAT:
        return LV_SYMBOL_CHARGE;
    case DEVICE_CAT_DOOR_LOCK:
        return LV_SYMBOL_CLOSE;
    case DEVICE_CAT_WINDOW_COVERING:
        return LV_SYMBOL_UP;
    case DEVICE_CAT_FAN:
        return LV_SYMBOL_LOOP;
    case DEVICE_CAT_TEMP_SENSOR:
    case DEVICE_CAT_HUMIDITY_SENSOR:
    case DEVICE_CAT_OCCUPANCY_SENSOR:
    case DEVICE_CAT_CONTACT_SENSOR:
    case DEVICE_CAT_LIGHT_SENSOR:
        return LV_SYMBOL_EYE_OPEN;
    case DEVICE_CAT_ON_OFF_SWITCH:
        return LV_SYMBOL_SHUFFLE;
    default:
        return LV_SYMBOL_SETTINGS;
    }
}

static void format_card_state(
    const matter_device_t *dev, char *buf, size_t len) {
    if (!dev->reachable) {
        snprintf(buf, len, "Unreachable");
        return;
    }
    switch (dev->category) {
    case DEVICE_CAT_DOOR_LOCK:
        snprintf(buf, len, dev->lock_state == 1
                 ? "Locked" : "Unlocked");
        break;
    case DEVICE_CAT_THERMOSTAT:
        snprintf(buf, len, "%.1fC",
                 (float)dev->local_temp / 100.0f);
        break;
    case DEVICE_CAT_WINDOW_COVERING:
        snprintf(buf, len, "Open %u%%", dev->cover_position);
        break;
    case DEVICE_CAT_TEMP_SENSOR:
        snprintf(buf, len, "%.1fC",
                 (float)dev->temperature / 100.0f);
        break;
    case DEVICE_CAT_HUMIDITY_SENSOR:
        snprintf(buf, len, "%.1f%%",
                 (float)dev->humidity / 100.0f);
        break;
    case DEVICE_CAT_OCCUPANCY_SENSOR:
        snprintf(buf, len, dev->occupancy
                 ? "Occupied" : "Clear");
        break;
    case DEVICE_CAT_CONTACT_SENSOR:
        snprintf(buf, len, dev->contact
                 ? "Closed" : "Open");
        break;
    case DEVICE_CAT_LIGHT_SENSOR:
        snprintf(buf, len, "%u lux", dev->illuminance);
        break;
    default:
        if (cat_has_level(dev->category) && dev->on_off) {
            snprintf(buf, len, "ON %u%%",
                     (unsigned)(dev->level * 100 / 254));
        } else {
            snprintf(buf, len, dev->on_off ? "ON" : "OFF");
        }
        break;
    }
}

// ---- Group management ----
static void activate_group(lv_group_t *grp) {
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, grp);
        }
    }
    lv_group_set_default(grp);
}

static void switch_to_screen(lv_obj_t *scr, lv_group_t *grp) {
    activate_group(grp);
    lv_group_focus_obj(lv_group_get_focused(grp));
    lv_screen_load(scr);
}

// ---- NVS helpers for UI preferences ----
static void save_commission_method(int method) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_UI_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "comm_method", (uint8_t)method);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static int load_commission_method(void) {
    nvs_handle_t nvs;
    uint8_t method = 0;
    if (nvs_open(NVS_UI_NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "comm_method", &method);
        nvs_close(nvs);
    }
    if (method >= NUM_COMMISSION_METHODS) method = 0;
    return (int)method;
}

static void save_input_mode(int input) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_UI_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "comm_input", (uint8_t)input);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static int load_input_mode(void) {
    nvs_handle_t nvs;
    uint8_t input = 0;
    if (nvs_open(NVS_UI_NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "comm_input", &input);
        nvs_close(nvs);
    }
    if (input >= NUM_INPUT_MODES) input = 0;
    return (int)input;
}

// ---- Confirmation dialog ----
static void dismiss_confirm_dialog(void) {
    if (!confirm_dialog) return;
    lv_obj_delete(confirm_dialog);
    confirm_dialog = NULL;
    if (confirm_dlg_group) {
        lv_group_delete(confirm_dlg_group);
        confirm_dlg_group = NULL;
    }
    if (confirm_prev_group) activate_group(confirm_prev_group);
    confirm_prev_group = NULL;
}

static void confirm_yes_cb(lv_event_t *e) {
    (void)e;
    uint64_t node_id = confirm_node_id;
    confirm_action_t action = confirm_action;

    dismiss_confirm_dialog();

    if (action == CONFIRM_FORCE_REMOVE) {
        device_manager_remove(node_id);
        refresh_dashboard();
    } else if (action == CONFIRM_UNPAIR) {
        matter_device_unpair(node_id);
        device_manager_remove(node_id);
        refresh_dashboard();
        switch_to_screen(scr_dashboard, grp_dashboard);
    }
}

static void confirm_no_cb(lv_event_t *e) {
    (void)e;
    dismiss_confirm_dialog();
}

static void show_confirm_dialog(
    const char *message, uint64_t node_id,
    confirm_action_t action) {
    if (confirm_dialog) return;

    confirm_node_id = node_id;
    confirm_action = action;
    confirm_prev_group = lv_group_get_default();

    confirm_dialog = lv_obj_create(lv_screen_active());
    lv_obj_set_size(confirm_dialog, 300, 160);
    lv_obj_center(confirm_dialog);
    lv_obj_set_flex_flow(confirm_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(confirm_dialog,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(confirm_dialog, 16, 0);
    lv_obj_set_style_pad_gap(confirm_dialog, 12, 0);
    lv_obj_set_style_bg_color(confirm_dialog,
        lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(confirm_dialog,
        lv_color_hex(0xFF5252), 0);
    lv_obj_set_style_border_width(confirm_dialog, 2, 0);

    lv_obj_t *msg = lv_label_create(confirm_dialog);
    lv_label_set_text(msg, message);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn_row = lv_obj_create(confirm_dialog);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, 16, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *no_btn = lv_button_create(btn_row);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "Cancel");
    lv_obj_add_event_cb(no_btn, confirm_no_cb,
        LV_EVENT_CLICKED, NULL);
    apply_focus_style(no_btn);

    lv_obj_t *yes_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(yes_btn,
        lv_color_hex(0xC62828), 0);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Confirm");
    lv_obj_add_event_cb(yes_btn, confirm_yes_cb,
        LV_EVENT_CLICKED, NULL);
    apply_focus_style(yes_btn);

    confirm_dlg_group = lv_group_create();
    lv_group_add_obj(confirm_dlg_group, no_btn);
    lv_group_add_obj(confirm_dlg_group, yes_btn);
    activate_group(confirm_dlg_group);
    lv_group_focus_obj(no_btn);
}

// ---- Thread button state ----
static bool s_thread_running = false;

static void update_thread_btn_label(void) {
    if (!dashboard_thread_btn) return;
    lv_obj_t *lbl = lv_obj_get_child(dashboard_thread_btn, 0);
    if (!lbl) return;
    lv_label_set_text(lbl, s_thread_running
        ? LV_SYMBOL_CLOSE " Stop Thread"
        : LV_SYMBOL_REFRESH " Start Thread");
}

// ---- Event queue timer callback ----
static void event_timer_cb(lv_timer_t *timer) {
    (void)timer;
    matter_event_t ev;
    while (xQueueReceive(s_ui_event_queue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case MATTER_EVENT_STACK_READY: {
            int dev_count = device_manager_count();
            if (dashboard_status_label) {
                if (dev_count > 0) {
                    lv_label_set_text(dashboard_status_label,
                        "Reconnecting to devices...");
                    show_spinner(dashboard_spinner, true);
                } else {
                    lv_label_set_text(dashboard_status_label,
                        "Commissioner ready");
                    show_spinner(dashboard_spinner, false);
                }
            }
            if (!matter_thread_available() && dashboard_thread_btn) {
                lv_obj_add_state(dashboard_thread_btn,
                    LV_STATE_DISABLED);
                lv_obj_t *lbl = lv_obj_get_child(
                    dashboard_thread_btn, 0);
                if (lbl) {
                    lv_label_set_text(lbl,
                        LV_SYMBOL_CLOSE " No Thread Radio");
                }
            }
            break;
        }
        case MATTER_EVENT_PASE_SUCCESS:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "PASE established.\n"
                    "Commissioning: attestation, certs,\n"
                    "network setup, operational discovery...");
            }
            break;
        case MATTER_EVENT_PASE_FAILED:
            s_commissioning_active = false;
            show_spinner(commission_spinner, false);
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "PASE failed!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_COMMISSION_SUCCESS:
            s_commissioning_active = false;
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "Success! Reading device info...");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            matter_device_read_info(ev.node_id);
            break;
        case MATTER_EVENT_DEVICE_INFO_READY: {
            const char *name = s_pending_name[0] ? s_pending_name
                                                 : ev.msg;
            uint16_t ep = ev.endpoint_id ? ev.endpoint_id : 1;
            show_spinner(commission_spinner, false);
            if (commission_status_label) {
                if (s_attestation_warned) {
                    lv_label_set_text(commission_status_label,
                        "\xE2\x9A\xA0 Test/unverified device,"
                        " commission success!");
                } else {
                    lv_label_set_text(commission_status_label,
                        "Success!");
                }
            }
            device_manager_add(
                ev.node_id, ep, name, ev.device_type_id,
                commission_method == 2);
            const matter_device_t *dev =
                device_manager_find(ev.node_id);
            device_category_t cat = dev ? dev->category
                                        : DEVICE_CAT_UNKNOWN;
            matter_device_subscribe(ev.node_id, ep, cat);
            refresh_dashboard();
            break;
        }
        case MATTER_EVENT_COMMISSION_FAILED:
            s_commissioning_active = false;
            show_spinner(commission_spinner, false);
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "Commissioning failed!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_COMMISSION_TIMEOUT:
            s_commissioning_active = false;
            show_spinner(commission_spinner, false);
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "Commissioning timed out (90s)");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_ATTESTATION_WARNING:
            s_attestation_warned = true;
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "Warning: device attestation failed.\n"
                    "Commissioning continues but device\n"
                    "may not be officially certified.");
                lv_obj_set_style_text_color(commission_status_label,
                    lv_color_hex(0xFFFFFF), 0);
            }
            break;
        case MATTER_EVENT_THREAD_BR_ERROR:
            show_spinner(dashboard_spinner, false);
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label, ev.msg);
                lv_obj_set_style_text_color(dashboard_status_label,
                    lv_color_hex(0xFFFFFF), 0);
            }
            s_thread_running = false;
            update_thread_btn_label();
            break;
        case MATTER_EVENT_THREAD_BR_STARTED:
            s_thread_running = true;
            update_thread_btn_label();
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "Thread border router started");
            }
            break;
        }
    }
}

// ---- Navigation callbacks ----
static void btn_thread_toggle_cb(lv_event_t *e) {
    (void)e;
    if (s_thread_running) {
        if (dashboard_status_label) {
            lv_label_set_text(dashboard_status_label,
                "Stopping Thread border router...");
            lv_obj_set_style_text_color(dashboard_status_label,
                lv_color_hex(0x888888), 0);
        }
        esp_err_t err = matter_stop_thread_br();
        if (err == ESP_OK) {
            s_thread_running = false;
            update_thread_btn_label();
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "Thread border router stopped");
            }
        } else {
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "Failed to stop Thread border router");
            }
        }
    } else {
        if (dashboard_status_label) {
            lv_label_set_text(dashboard_status_label,
                "Starting Thread border router...");
            lv_obj_set_style_text_color(dashboard_status_label,
                lv_color_hex(0x888888), 0);
        }
        show_spinner(dashboard_spinner, true);
        esp_err_t err = matter_start_thread_br();
        if (err == ESP_OK) {
            s_thread_running = true;
            update_thread_btn_label();
            matter_device_subscribe_thread();
            show_spinner(dashboard_spinner, false);
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "Thread border router started");
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            show_spinner(dashboard_spinner, false);
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "WiFi not connected. Connect first.");
            }
        } else {
            show_spinner(dashboard_spinner, false);
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label,
                    "Thread border router failed to start");
            }
        }
    }
}

static void btn_add_cb(lv_event_t *e) {
    (void)e;
    if (commission_status_label) lv_label_set_text(commission_status_label, "");
    if (commission_code_ta) lv_textarea_set_text(commission_code_ta, "");
    if (commission_disc_ta) lv_textarea_set_text(commission_disc_ta, "");
    if (commission_hints_ta) lv_textarea_set_text(commission_hints_ta, "");
    if (commission_thread_ta) {
        char dataset_hex[509];
        if (matter_get_thread_active_dataset_hex(
                dataset_hex, sizeof(dataset_hex)) == ESP_OK) {
            lv_textarea_set_text(commission_thread_ta, dataset_hex);
        } else {
            lv_textarea_set_text(commission_thread_ta, "");
        }
    }
    if (commission_name_ta) lv_textarea_set_text(commission_name_ta, "");
    switch_to_screen(scr_commission, grp_commission);

    lv_obj_t *first_ta = (commission_input == 2)
                              ? commission_disc_ta
                              : commission_code_ta;
    lv_group_focus_obj(first_ta);
}

static void btn_back_dashboard_cb(lv_event_t *e) {
    (void)e;
    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);
}

// ---- Commission screen callbacks ----
static void set_field_visible(lv_obj_t *label, lv_obj_t *ta, bool vis) {
    if (vis) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ta, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_spinner(lv_obj_t *spinner, bool show) {
    if (!spinner) return;
    if (show) {
        lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_commission_fields(void) {
    // PIN input mode only available for Ethernet
    if (commission_input == 3 && commission_method != 0) {
        commission_input = 2;
        for (int i = 0; i < NUM_INPUT_MODES; i++) {
            if (i == commission_input)
                lv_obj_add_state(commission_input_radios[i],
                    LV_STATE_CHECKED);
            else
                lv_obj_clear_state(commission_input_radios[i],
                    LV_STATE_CHECKED);
        }
        save_input_mode(commission_input);
    }

    // Show/hide PIN button based on method
    if (commission_input_radios[3]) {
        if (commission_method == 0) {
            lv_obj_clear_flag(commission_input_radios[3],
                LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(commission_input_radios[3],
                LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool show_disc = (commission_input == 2);
    set_field_visible(commission_disc_label, commission_disc_ta,
        show_disc);

    bool show_hints = (commission_method == 0 &&
                       commission_input == 2);
    set_field_visible(commission_hints_label, commission_hints_ta,
        show_hints);

    bool show_thread = (commission_method == 2);
    set_field_visible(commission_thread_label, commission_thread_ta,
        show_thread);

    static const char *code_labels[] = {
        "QR Code Payload:", "Manual Pairing Code:",
        "Passcode:", "Setup PIN Code:"
    };
    static const char *code_placeholders[] = {
        "e.g. MT:...", "e.g. 34970112332",
        "e.g. 20212020", "e.g. 20212020"
    };
    lv_label_set_text(commission_code_label,
        code_labels[commission_input]);
    lv_textarea_set_placeholder_text(commission_code_ta,
        code_placeholders[commission_input]);
}

static void method_radio_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (int i = 0; i < NUM_COMMISSION_METHODS; i++) {
        if (obj == commission_method_radios[i]) {
            commission_method = i;
            lv_obj_add_state(commission_method_radios[i],
                LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(commission_method_radios[i],
                LV_STATE_CHECKED);
        }
    }
    save_commission_method(commission_method);
    update_commission_fields();

    lv_obj_t *first_ta = (commission_input == 2)
                              ? commission_disc_ta
                              : commission_code_ta;
    lv_group_focus_obj(first_ta);
}

static void input_radio_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (int i = 0; i < NUM_INPUT_MODES; i++) {
        if (obj == commission_input_radios[i]) {
            commission_input = i;
            lv_obj_add_state(commission_input_radios[i],
                LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(commission_input_radios[i],
                LV_STATE_CHECKED);
        }
    }
    save_input_mode(commission_input);
    update_commission_fields();

    lv_obj_t *first_ta = (commission_input == 2)
                              ? commission_disc_ta
                              : commission_code_ta;
    lv_group_focus_obj(first_ta);
}

static bool is_all_digits(const char *s) {
    if (!s || *s == '\0') return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

static bool is_valid_pincode(const char *s, uint32_t *out) {
    if (!is_all_digits(s)) return false;
    long val = strtol(s, NULL, 10);
    if (val < 1 || val > 99999998) return false;
    uint32_t u = (uint32_t)val;
    if (u == 12345678 || u == 87654321) return false;
    bool all_same = true;
    for (int i = 1; i < 8; i++) {
        if (((u / 10) % 10) != (u % 10)) { all_same = false; break; }
        u /= 10;
    }
    if (all_same) return false;
    *out = (uint32_t)val;
    return true;
}

static bool is_valid_discriminator(const char *s, uint16_t *out) {
    if (!is_all_digits(s)) return false;
    long val = strtol(s, NULL, 10);
    if (val < 0 || val > 4095) return false;
    *out = (uint16_t)val;
    return true;
}

static void btn_start_commission_cb(lv_event_t *e) {
    (void)e;
    const char *code_str = lv_textarea_get_text(commission_code_ta);
    const char *name_str = lv_textarea_get_text(commission_name_ta);

    if (!code_str || code_str[0] == '\0') {
        static const char *prompts[] = {
            "Enter QR code payload!",
            "Enter manual pairing code!",
            "Enter passcode!",
            "Enter setup PIN code!"
        };
        lv_label_set_text(commission_status_label,
            prompts[commission_input]);
        return;
    }

    strncpy(s_pending_name, name_str, MATTER_DEVICE_NAME_LEN - 1);
    s_pending_name[MATTER_DEVICE_NAME_LEN - 1] = '\0';

    s_pending_node_id = device_manager_next_node_id();
    esp_err_t err = ESP_FAIL;

    lv_label_set_text(commission_status_label, "Starting...");
    lv_obj_add_state(commission_start_btn, LV_STATE_DISABLED);

    bool needs_pincode = (commission_input == 2 ||
                          commission_input == 3);
    uint32_t pincode = 0;
    if (needs_pincode && !is_valid_pincode(code_str, &pincode)) {
        lv_label_set_text(commission_status_label,
            "Invalid PIN (1-99999998)");
        lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
        return;
    }

    uint16_t disc = 0;
    if (commission_input == 2) {
        const char *disc_str =
            lv_textarea_get_text(commission_disc_ta);
        if (!is_valid_discriminator(disc_str, &disc)) {
            lv_label_set_text(commission_status_label,
                "Invalid discriminator (0-4095)");
            lv_obj_clear_state(commission_start_btn,
                LV_STATE_DISABLED);
            return;
        }
    }

    switch (commission_method) {
    case 0: // Ethernet
        if (commission_input <= 1) {
            err = matter_commission_setup_code(
                s_pending_node_id, code_str);
        } else if (commission_input == 2) {
            const char *hints_str =
                lv_textarea_get_text(commission_hints_ta);
            uint8_t hints = 0;
            if (hints_str && is_all_digits(hints_str) &&
                hints_str[0]) {
                hints = (uint8_t)strtol(hints_str, NULL, 10);
            }
            err = matter_commission_disc_pass(
                s_pending_node_id, pincode, disc, hints);
        } else {
            err = matter_commission_on_network(
                s_pending_node_id, pincode);
        }
        break;
    case 1: // BLE+WiFi
        if (commission_input <= 1) {
            err = matter_commission_ble_wifi_code(
                s_pending_node_id, code_str);
        } else {
            err = matter_commission_ble_wifi(
                s_pending_node_id, pincode, disc);
        }
        break;
    case 2: { // BLE+Thread
        const char *thread_str =
            lv_textarea_get_text(commission_thread_ta);
        if (commission_input <= 1) {
            err = matter_commission_ble_thread_code(
                s_pending_node_id, code_str, thread_str);
        } else {
            err = matter_commission_ble_thread(
                s_pending_node_id, pincode, disc, thread_str);
        }
        break;
    }
    }

    if (err != ESP_OK) {
        const char *msg = "Failed to start pairing";
        if (err == ESP_ERR_INVALID_STATE &&
            commission_method <= 1) {
            msg = "WiFi not connected. Connect to WiFi first.";
        } else if (commission_method == 2 &&
                   err == ESP_ERR_NOT_FOUND) {
            msg = "No Thread dataset: border router has no active "
                  "network. Enter a dataset manually.";
        } else if (err == ESP_ERR_INVALID_ARG &&
                   commission_input <= 1) {
            msg = "Invalid setup code. Check the QR or manual "
                  "pairing code and try again.";
        }
        lv_label_set_text(commission_status_label, msg);
        lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
    } else {
        s_commissioning_active = true;
        s_attestation_warned = false;
        show_spinner(commission_spinner, true);
        lv_label_set_text(commission_status_label,
            "Establishing PASE...");
    }
}

// ---- Dashboard card callbacks ----
static void card_click_cb(lv_event_t *e) {
    if (s_commissioning_active) return;
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    const matter_device_t *dev = device_manager_find(node_id);
    if (!dev) return;
    // Only toggle for devices with on/off capability
    if (cat_has_onoff(dev->category) && !cat_is_sensor(dev->category)) {
        matter_device_send_toggle(dev->node_id, dev->endpoint_id);
    }
}

static void card_key_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    if (key == LV_KEY_HOME) {
        show_detail_for_device(node_id);
    } else if (key == LV_KEY_END) {
        const matter_device_t *dev = device_manager_find(node_id);
        const char *name = dev ? dev->name : "this device";
        char msg[96];
        snprintf(msg, sizeof(msg),
            "Force remove \"%s\"?\nThis won't unpair from the device.",
            name);
        show_confirm_dialog(msg, node_id, CONFIRM_FORCE_REMOVE);
    }
}

// ---- Detail screen callbacks ----
static void btn_on_cb(lv_event_t *e) {
    (void)e;
    if (s_commissioning_active) return;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_on(dev->node_id, dev->endpoint_id);
}

static void btn_off_cb(lv_event_t *e) {
    (void)e;
    if (s_commissioning_active) return;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_off(dev->node_id, dev->endpoint_id);
}

static void btn_toggle_cb(lv_event_t *e) {
    (void)e;
    if (s_commissioning_active) return;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_toggle(dev->node_id, dev->endpoint_id);
}

static void slider_key_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return;

    lv_obj_t *slider = lv_event_get_target(e);
    uint16_t mods = lvgl_get_nav_modifiers();
    int32_t step = (mods & BSP_INPUT_MODIFIER_SHIFT) ? 1 : 10;
    int32_t val = lv_slider_get_value(slider);
    int32_t delta = (key == LV_KEY_RIGHT) ? step : -step;
    lv_slider_set_value(slider, val + delta, LV_ANIM_ON);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_event_stop_processing(e);
}

static void slider_level_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_level(
            dev->node_id, dev->endpoint_id, (uint8_t)val);
    }
}

static void slider_color_temp_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_color_temp(
            dev->node_id, dev->endpoint_id, (uint16_t)val);
    }
}

static void slider_hue_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_hue_sat(
            dev->node_id, dev->endpoint_id,
            (uint8_t)val, dev->saturation);
    }
}

static void slider_sat_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_hue_sat(
            dev->node_id, dev->endpoint_id,
            dev->hue, (uint8_t)val);
    }
}

static void btn_lock_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_lock(dev->node_id, dev->endpoint_id);
}

static void btn_unlock_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_unlock(dev->node_id, dev->endpoint_id);
}

static void btn_cover_open_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_cover_open(
            dev->node_id, dev->endpoint_id);
    }
}

static void btn_cover_close_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_cover_close(
            dev->node_id, dev->endpoint_id);
    }
}

static void btn_cover_stop_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_cover_stop(
            dev->node_id, dev->endpoint_id);
    }
}

static void slider_cover_pos_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) {
        matter_device_send_cover_pos(
            dev->node_id, dev->endpoint_id, (uint8_t)val);
    }
}

static void btn_rename_cb(lv_event_t *e) {
    (void)e;
    const char *new_name = lv_textarea_get_text(detail_rename_ta);
    if (new_name && new_name[0]) {
        device_manager_rename(detail_node_id, new_name);
        device_manager_save();
        lv_label_set_text(detail_name_label, new_name);
    }
}

static void btn_reconnect_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (!dev) return;
    matter_device_subscribe(
        dev->node_id, dev->endpoint_id, dev->category);
    if (detail_state_label) {
        lv_label_set_text(detail_state_label, "Reconnecting...");
        lv_obj_set_style_text_color(detail_state_label,
            lv_color_hex(0x888888), 0);
    }
}

static void btn_unpair_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    const char *name = dev ? dev->name : "this device";
    char msg[96];
    snprintf(msg, sizeof(msg),
        "Unpair \"%s\"?\nThis will remove the device.", name);
    show_confirm_dialog(msg, detail_node_id, CONFIRM_UNPAIR);
}

static void btn_back_detail_cb(lv_event_t *e) {
    (void)e;
    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);
}

// ---- Embedded key icons ----
extern const uint8_t icon_esc_png_start[] asm("_binary_esc_png_start");
extern const uint8_t icon_esc_png_end[]   asm("_binary_esc_png_end");
extern const uint8_t icon_f1_png_start[]  asm("_binary_f1_png_start");
extern const uint8_t icon_f1_png_end[]    asm("_binary_f1_png_end");
extern const uint8_t icon_f2_png_start[]  asm("_binary_f2_png_start");
extern const uint8_t icon_f2_png_end[]    asm("_binary_f2_png_end");
extern const uint8_t icon_f3_png_start[]  asm("_binary_f3_png_start");
extern const uint8_t icon_f3_png_end[]    asm("_binary_f3_png_end");
extern const uint8_t icon_f4_png_start[]  asm("_binary_f4_png_start");
extern const uint8_t icon_f4_png_end[]    asm("_binary_f4_png_end");
extern const uint8_t icon_f5_png_start[]  asm("_binary_f5_png_start");
extern const uint8_t icon_f5_png_end[]    asm("_binary_f5_png_end");
extern const uint8_t icon_f6_png_start[]  asm("_binary_f6_png_start");
extern const uint8_t icon_f6_png_end[]    asm("_binary_f6_png_end");

static lv_image_dsc_t img_dsc_esc;
static lv_image_dsc_t img_dsc_f1;
static lv_image_dsc_t img_dsc_f2;
static lv_image_dsc_t img_dsc_f3;
static lv_image_dsc_t img_dsc_f4;
static lv_image_dsc_t img_dsc_f5;
static lv_image_dsc_t img_dsc_f6;
static bool icons_initialized = false;

static void init_key_icons(void) {
    if (icons_initialized) return;

    img_dsc_esc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_esc.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_esc.header.w = 32;
    img_dsc_esc.header.h = 32;
    img_dsc_esc.data = icon_esc_png_start;
    img_dsc_esc.data_size = icon_esc_png_end - icon_esc_png_start;

    img_dsc_f1.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f1.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f1.header.w = 32;
    img_dsc_f1.header.h = 32;
    img_dsc_f1.data = icon_f1_png_start;
    img_dsc_f1.data_size = icon_f1_png_end - icon_f1_png_start;

    img_dsc_f2.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f2.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f2.header.w = 32;
    img_dsc_f2.header.h = 32;
    img_dsc_f2.data = icon_f2_png_start;
    img_dsc_f2.data_size = icon_f2_png_end - icon_f2_png_start;

    img_dsc_f3.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f3.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f3.header.w = 32;
    img_dsc_f3.header.h = 32;
    img_dsc_f3.data = icon_f3_png_start;
    img_dsc_f3.data_size = icon_f3_png_end - icon_f3_png_start;

    img_dsc_f4.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f4.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f4.header.w = 32;
    img_dsc_f4.header.h = 32;
    img_dsc_f4.data = icon_f4_png_start;
    img_dsc_f4.data_size = icon_f4_png_end - icon_f4_png_start;

    img_dsc_f5.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f5.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f5.header.w = 32;
    img_dsc_f5.header.h = 32;
    img_dsc_f5.data = icon_f5_png_start;
    img_dsc_f5.data_size = icon_f5_png_end - icon_f5_png_start;

    img_dsc_f6.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_f6.header.cf = LV_COLOR_FORMAT_RAW;
    img_dsc_f6.header.w = 32;
    img_dsc_f6.header.h = 32;
    img_dsc_f6.data = icon_f6_png_start;
    img_dsc_f6.data_size = icon_f6_png_end - icon_f6_png_start;

    icons_initialized = true;
}

// ---- Key hint bar ----
static void hint_add_icon(lv_obj_t *bar, const lv_image_dsc_t *icon) {
    lv_obj_t *img = lv_image_create(bar);
    lv_image_set_src(img, icon);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(img, 20, 20);
    lv_image_set_scale(img, 160);
}

static void hint_add_text(lv_obj_t *bar, const char *text) {
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), 0);
}

static void apply_focus_style(lv_obj_t *obj) {
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED | LV_STATE_CHECKED);
}

// ---- Screenshot (F3) — save PPM to SD card ----
#if LV_USE_SNAPSHOT
static void take_screenshot(void) {
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "Cannot save screenshot (SD not mounted)");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;

    ESP_LOGI(TAG, "Taking screenshot...");

    lv_draw_buf_t *snap = lv_snapshot_take(
        scr, LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        ESP_LOGE(TAG, "Snapshot failed (out of memory?)");
        return;
    }

    uint32_t w = snap->header.w;
    uint32_t h = snap->header.h;
    uint32_t stride = snap->header.stride;

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char filename[64];
    snprintf(filename, sizeof(filename),
             "/sd/matter-%04d%02d%02d%02d%02d%02d.ppm",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1,
             tm_info->tm_mday, tm_info->tm_hour,
             tm_info->tm_min, tm_info->tm_sec);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filename);
        lv_draw_buf_destroy(snap);
        return;
    }

    fprintf(f, "P6\n%lu %lu\n255\n",
            (unsigned long)w, (unsigned long)h);

    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row =
            (const uint16_t *)(snap->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint16_t px = row[x];
            // RGB565 to RGB888
            uint8_t r = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((px & 0x1F) * 255 / 31);
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }

    fclose(f);
    lv_draw_buf_destroy(snap);
    ESP_LOGI(TAG, "Screenshot saved: %s (%lux%lu)",
             filename, (unsigned long)w, (unsigned long)h);
}
#endif

static void global_key_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    uint32_t key = lv_indev_get_key(indev);
#if LV_USE_SNAPSHOT
    if (key == BSP_KEY_F3) {
        take_screenshot();
        return;
    }
#endif
    (void)key;
}

static lv_obj_t *create_key_hints_bar(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 24);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_set_style_pad_gap(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

// ---- Screen creation ----
static lv_obj_t *create_header(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb, lv_group_t *grp) {
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (back_cb) {
        lv_obj_t *btn = lv_button_create(header);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
        apply_focus_style(btn);
        if (grp) lv_group_add_obj(grp, btn);
    }

    lv_obj_t *lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_flex_grow(lbl, 1);

    return header;
}

static void create_dashboard_screen(void) {
    grp_dashboard = lv_group_create();

    scr_dashboard = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_dashboard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_dashboard, 4, 0);

    lv_obj_t *header = lv_obj_create(scr_dashboard);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_set_style_pad_gap(header, 8, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_HOME " Matter Commissioner");
    lv_obj_set_flex_grow(title, 1);

    dashboard_thread_btn = lv_button_create(header);
    lv_obj_t *thread_lbl = lv_label_create(dashboard_thread_btn);
    lv_label_set_text(thread_lbl, LV_SYMBOL_REFRESH " Start Thread");
    lv_obj_add_event_cb(dashboard_thread_btn,
        btn_thread_toggle_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(dashboard_thread_btn);
    lv_group_add_obj(grp_dashboard, dashboard_thread_btn);

    lv_obj_t *add_btn = lv_button_create(header);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add");
    lv_obj_add_event_cb(add_btn, btn_add_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(add_btn);
    lv_group_add_obj(grp_dashboard, add_btn);

    dashboard_container = lv_obj_create(scr_dashboard);
    lv_obj_set_size(dashboard_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dashboard_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_grow(dashboard_container, 1);
    lv_obj_set_style_pad_all(dashboard_container, 4, 0);
    lv_obj_set_style_pad_gap(dashboard_container, 8, 0);

    dashboard_status_row = lv_obj_create(scr_dashboard);
    lv_obj_set_size(dashboard_status_row, LV_PCT(100),
        LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dashboard_status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dashboard_status_row,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(dashboard_status_row, 2, 0);
    lv_obj_set_style_pad_gap(dashboard_status_row, 8, 0);
    lv_obj_set_style_border_width(dashboard_status_row, 0, 0);
    lv_obj_set_style_bg_opa(dashboard_status_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(dashboard_status_row, LV_OBJ_FLAG_SCROLLABLE);

    dashboard_spinner = lv_spinner_create(dashboard_status_row);
    lv_spinner_set_anim_params(dashboard_spinner, 1000, 270);
    lv_obj_set_size(dashboard_spinner, 20, 20);
    lv_obj_set_style_arc_width(dashboard_spinner, 3, 0);
    lv_obj_set_style_arc_width(dashboard_spinner, 3,
        LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(dashboard_spinner,
        lv_color_hex(0x00E5FF), LV_PART_INDICATOR);

    dashboard_status_label = lv_label_create(dashboard_status_row);
    lv_label_set_text(dashboard_status_label, "Initializing...");
    lv_obj_set_style_text_color(dashboard_status_label,
        lv_color_hex(0x888888), 0);

    lv_obj_t *dash_hints = create_key_hints_bar(scr_dashboard);
    hint_add_text(dash_hints, "Enter: Toggle");
    hint_add_icon(dash_hints, &img_dsc_f1);
    hint_add_text(dash_hints, "Details");
    hint_add_icon(dash_hints, &img_dsc_f2);
    hint_add_text(dash_hints, "Force Remove");
    hint_add_icon(dash_hints, &img_dsc_f3);
    hint_add_text(dash_hints, "Screenshot");
}

static void refresh_dashboard(void) {
    if (!dashboard_container) return;
    lv_obj_clean(dashboard_container);

    while (lv_group_get_obj_count(grp_dashboard) > DASHBOARD_HEADER_BTNS) {
        lv_obj_t *last = lv_group_get_obj_by_index(
            grp_dashboard,
            lv_group_get_obj_count(grp_dashboard) - 1);
        lv_group_remove_obj(last);
    }

    int count = device_manager_count();
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(dashboard_container);
        lv_label_set_text(lbl,
            "No devices.\n"
            "Select '" LV_SYMBOL_PLUS " Add' to commission.");
        lv_group_focus_obj(lv_group_get_obj_by_index(
            grp_dashboard, 0));
        return;
    }

    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (!dev) continue;

        lv_obj_t *card = lv_obj_create(dashboard_container);
        lv_obj_set_size(card, 140, 80);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_set_style_pad_gap(card, 2, 0);

        // Card color based on device type and state
        if (!dev->reachable) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x303030), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
        } else if (dev->category == DEVICE_CAT_DOOR_LOCK) {
            lv_obj_set_style_bg_color(card,
                dev->lock_state == 1
                    ? lv_color_hex(0x2E7D32)   // green = locked
                    : lv_color_hex(0xE65100), 0); // amber = unlocked
        } else if (dev->category == DEVICE_CAT_OCCUPANCY_SENSOR) {
            lv_obj_set_style_bg_color(card,
                dev->occupancy
                    ? lv_color_hex(0x2E7D32)   // green = occupied
                    : lv_color_hex(0x37474F), 0);
        } else if (dev->category == DEVICE_CAT_CONTACT_SENSOR) {
            lv_obj_set_style_bg_color(card,
                dev->contact
                    ? lv_color_hex(0x2E7D32)   // green = closed
                    : lv_color_hex(0xE65100), 0); // amber = open
        } else if (cat_is_sensor(dev->category)) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x37474F), 0);
        } else if (dev->on_off) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x2E7D32), 0);
        } else {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x424242), 0);
        }
        lv_obj_set_style_text_color(card, lv_color_hex(0xFFFFFF), 0);

        // Icon
        lv_obj_t *icon_lbl = lv_label_create(card);
        lv_label_set_text(icon_lbl, cat_icon(dev->category));

        // Name
        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, dev->name);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, 120);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        // State text
        char state_buf[32];
        format_card_state(dev, state_buf, sizeof(state_buf));
        lv_obj_t *state_lbl = lv_label_create(card);
        lv_label_set_text(state_lbl, state_buf);

        void *user_data = (void *)(uintptr_t)dev->node_id;
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_SHORT_CLICKED, user_data);
        lv_obj_add_event_cb(card, card_key_cb, LV_EVENT_KEY, user_data);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_outline_width(card, 4, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(card, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(card, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(card, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(card, 3, LV_STATE_FOCUSED);

        lv_group_add_obj(grp_dashboard, card);
    }

    if (lv_group_get_obj_count(grp_dashboard) > DASHBOARD_HEADER_BTNS) {
        lv_group_focus_obj(lv_group_get_obj_by_index(
            grp_dashboard, DASHBOARD_HEADER_BTNS));
    } else {
        lv_group_focus_obj(lv_group_get_obj_by_index(
            grp_dashboard, 0));
    }
}

static void create_commission_screen(void) {
    grp_commission = lv_group_create();

    scr_commission = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_commission, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_commission, 4, 0);
    lv_obj_set_style_pad_gap(scr_commission, 4, 0);

    create_header(scr_commission, "Add New Device",
        btn_back_dashboard_cb, grp_commission);

    // Description label
    lv_obj_t *desc_label = lv_label_create(scr_commission);
    lv_label_set_text(desc_label, "Matter IoT device transport:");
    lv_obj_set_style_text_color(desc_label,
        lv_color_hex(0xAAAAAA), 0);

    // Transport method row: Ethernet, BLE+WiFi, BLE+Thread
    lv_obj_t *method_row = lv_obj_create(scr_commission);
    lv_obj_set_size(method_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(method_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(method_row, 2, 0);
    lv_obj_set_style_pad_gap(method_row, 4, 0);
    lv_obj_clear_flag(method_row, LV_OBJ_FLAG_SCROLLABLE);

    static const char *method_labels[] = {
        "Ethernet", "BLE+WiFi", "BLE+Thread"
    };
    for (int i = 0; i < NUM_COMMISSION_METHODS; i++) {
        commission_method_radios[i] =
            lv_button_create(method_row);
        lv_obj_add_flag(commission_method_radios[i],
            LV_OBJ_FLAG_CHECKABLE);
        lv_obj_t *lbl = lv_label_create(
            commission_method_radios[i]);
        lv_label_set_text(lbl, method_labels[i]);
        lv_obj_add_event_cb(commission_method_radios[i],
            method_radio_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_bg_color(
            commission_method_radios[i],
            lv_color_hex(0x333333),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(
            commission_method_radios[i], LV_OPA_COVER,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(
            commission_method_radios[i],
            lv_color_hex(0x999999),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(
            commission_method_radios[i], 0,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(
            commission_method_radios[i],
            lv_color_hex(0x00C853),
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(
            commission_method_radios[i], LV_OPA_COVER,
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(
            commission_method_radios[i],
            lv_color_hex(0x000000),
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(
            commission_method_radios[i], 2,
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(
            commission_method_radios[i],
            lv_color_hex(0xFFFFFF),
            LV_PART_MAIN | LV_STATE_CHECKED);
        apply_focus_style(commission_method_radios[i]);
        lv_group_add_obj(grp_commission,
            commission_method_radios[i]);
    }
    commission_method = load_commission_method();
    lv_obj_add_state(commission_method_radios[commission_method],
        LV_STATE_CHECKED);

    // Input sub-mode row (QR / Manual / Disc+Pass / PIN)
    commission_input_row = lv_obj_create(scr_commission);
    lv_obj_set_size(commission_input_row,
        LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(commission_input_row,
        LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(commission_input_row, 2, 0);
    lv_obj_set_style_pad_gap(commission_input_row, 4, 0);
    lv_obj_clear_flag(commission_input_row,
        LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *via_label = lv_label_create(commission_input_row);
    lv_label_set_text(via_label, "Via:");
    lv_obj_set_style_text_color(via_label,
        lv_color_hex(0xAAAAAA), 0);

    static const char *input_labels[] = {
        "QR Code", "Manual Code", "Disc+Pass", "PIN"
    };
    commission_input = load_input_mode();
    for (int i = 0; i < NUM_INPUT_MODES; i++) {
        commission_input_radios[i] =
            lv_button_create(commission_input_row);
        lv_obj_add_flag(commission_input_radios[i],
            LV_OBJ_FLAG_CHECKABLE);
        lv_obj_t *lbl = lv_label_create(
            commission_input_radios[i]);
        lv_label_set_text(lbl, input_labels[i]);
        lv_obj_add_event_cb(commission_input_radios[i],
            input_radio_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_bg_color(
            commission_input_radios[i],
            lv_color_hex(0x333333),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(
            commission_input_radios[i],
            LV_OPA_COVER,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(
            commission_input_radios[i],
            lv_color_hex(0x999999),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(
            commission_input_radios[i], 0,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(
            commission_input_radios[i],
            lv_color_hex(0x00897B),
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(
            commission_input_radios[i],
            LV_OPA_COVER,
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(
            commission_input_radios[i],
            lv_color_hex(0xFFFFFF),
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(
            commission_input_radios[i], 2,
            LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(
            commission_input_radios[i],
            lv_color_hex(0xFFFFFF),
            LV_PART_MAIN | LV_STATE_CHECKED);
        apply_focus_style(commission_input_radios[i]);
        lv_group_add_obj(grp_commission,
            commission_input_radios[i]);
    }
    lv_obj_add_state(
        commission_input_radios[commission_input],
        LV_STATE_CHECKED);

    commission_disc_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_disc_label, "Discriminator:");
    lv_obj_add_flag(commission_disc_label, LV_OBJ_FLAG_HIDDEN);

    commission_disc_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_disc_ta, true);
    lv_textarea_set_placeholder_text(commission_disc_ta, "e.g. 3840");
    lv_obj_set_width(commission_disc_ta, LV_PCT(100));
    lv_obj_add_flag(commission_disc_ta, LV_OBJ_FLAG_HIDDEN);
    apply_focus_style(commission_disc_ta);
    lv_group_add_obj(grp_commission, commission_disc_ta);

    commission_hints_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_hints_label,
        "Discovery (1=SoftAP 2=BLE 4=IP):");
    lv_obj_add_flag(commission_hints_label, LV_OBJ_FLAG_HIDDEN);

    commission_hints_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_hints_ta, true);
    lv_textarea_set_placeholder_text(commission_hints_ta,
        "optional, e.g. 4");
    lv_obj_set_width(commission_hints_ta, LV_PCT(100));
    lv_obj_add_flag(commission_hints_ta, LV_OBJ_FLAG_HIDDEN);
    apply_focus_style(commission_hints_ta);
    lv_group_add_obj(grp_commission, commission_hints_ta);

    commission_thread_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_thread_label,
        "Thread Dataset (optional, auto-fetched from BR):");
    lv_obj_add_flag(commission_thread_label, LV_OBJ_FLAG_HIDDEN);

    commission_thread_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_thread_ta, true);
    lv_textarea_set_placeholder_text(commission_thread_ta,
        "Auto-filled from border router");
    lv_obj_set_width(commission_thread_ta, LV_PCT(100));
    lv_obj_add_flag(commission_thread_ta, LV_OBJ_FLAG_HIDDEN);
    apply_focus_style(commission_thread_ta);
    lv_group_add_obj(grp_commission, commission_thread_ta);

    commission_code_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_code_label, "Setup PIN Code:");

    commission_code_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_code_ta, true);
    lv_textarea_set_placeholder_text(commission_code_ta, "e.g. 20212020");
    lv_obj_set_width(commission_code_ta, LV_PCT(100));
    apply_focus_style(commission_code_ta);
    lv_group_add_obj(grp_commission, commission_code_ta);

    lv_obj_t *name_label = lv_label_create(scr_commission);
    lv_label_set_text(name_label, "Device Name:");

    commission_name_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_name_ta, true);
    lv_textarea_set_placeholder_text(commission_name_ta, "My Light");
    lv_obj_set_width(commission_name_ta, LV_PCT(100));
    apply_focus_style(commission_name_ta);
    lv_group_add_obj(grp_commission, commission_name_ta);

    commission_start_btn = lv_button_create(scr_commission);
    lv_obj_set_width(commission_start_btn, LV_PCT(100));
    lv_obj_t *start_lbl = lv_label_create(commission_start_btn);
    lv_label_set_text(start_lbl, "Start Commissioning");
    lv_obj_center(start_lbl);
    lv_obj_add_event_cb(commission_start_btn, btn_start_commission_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(commission_start_btn);
    lv_group_add_obj(grp_commission, commission_start_btn);

    lv_obj_t *comm_status_row = lv_obj_create(scr_commission);
    lv_obj_set_size(comm_status_row, LV_PCT(100),
        LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comm_status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(comm_status_row, LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(comm_status_row, 2, 0);
    lv_obj_set_style_pad_gap(comm_status_row, 8, 0);
    lv_obj_set_style_border_width(comm_status_row, 0, 0);
    lv_obj_set_style_bg_opa(comm_status_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(comm_status_row, LV_OBJ_FLAG_SCROLLABLE);

    commission_spinner = lv_spinner_create(comm_status_row);
    lv_spinner_set_anim_params(commission_spinner, 1000, 270);
    lv_obj_set_size(commission_spinner, 20, 20);
    lv_obj_set_style_arc_width(commission_spinner, 3, 0);
    lv_obj_set_style_arc_width(commission_spinner, 3,
        LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(commission_spinner,
        lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_add_flag(commission_spinner, LV_OBJ_FLAG_HIDDEN);

    commission_status_label = lv_label_create(comm_status_row);
    lv_label_set_text(commission_status_label, "");
    lv_obj_set_flex_grow(commission_status_label, 1);
    lv_label_set_long_mode(commission_status_label,
        LV_LABEL_LONG_WRAP);

    lv_obj_t *comm_hints = create_key_hints_bar(scr_commission);
    hint_add_text(comm_hints, "Tab: Next field");
    hint_add_text(comm_hints, "Enter: Confirm");
    hint_add_icon(comm_hints, &img_dsc_esc);
    hint_add_text(comm_hints, "Back");
    hint_add_icon(comm_hints, &img_dsc_f3);
    hint_add_text(comm_hints, "Screenshot");

    update_commission_fields();
}

static void create_detail_screen(void) {
    scr_detail = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_detail, 4, 0);
    lv_obj_set_style_pad_gap(scr_detail, 4, 0);

    // detail_content is the container that gets rebuilt
    // The back button header is created inside show_detail_for_device
    detail_content = NULL;
}

// Helper: add a labeled slider to detail screen
static lv_obj_t *detail_add_slider(
    const char *label_text, int32_t min, int32_t max,
    int32_t val, lv_event_cb_t cb) {
    lv_obj_t *lbl = lv_label_create(detail_content);
    lv_label_set_text(lbl, label_text);

    lv_obj_t *slider = lv_slider_create(detail_content);
    lv_obj_set_width(slider, LV_PCT(90));
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(
        slider, slider_key_cb,
        LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    apply_focus_style(slider);
    lv_group_add_obj(grp_detail, slider);
    return slider;
}

// Helper: add a button to detail screen
static lv_obj_t *detail_add_btn(
    lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(btn);
    lv_group_add_obj(grp_detail, btn);
    return btn;
}

static void show_detail_for_device(uint64_t node_id) {
    const matter_device_t *dev = device_manager_find(node_id);
    if (!dev) return;

    detail_node_id = node_id;

    // Recreate the group and screen content
    if (grp_detail) lv_group_delete(grp_detail);
    grp_detail = lv_group_create();

    // Delete all children and rebuild
    lv_obj_clean(scr_detail);

    // Back button header
    create_header(scr_detail, "", btn_back_detail_cb, grp_detail);

    // Scrollable content area
    detail_content = lv_obj_create(scr_detail);
    lv_obj_set_size(detail_content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(detail_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(detail_content, 1);
    lv_obj_set_style_pad_all(detail_content, 4, 0);
    lv_obj_set_style_pad_gap(detail_content, 4, 0);

    // Device name (large)
    detail_name_label = lv_label_create(detail_content);
    lv_label_set_text(detail_name_label, dev->name);
    lv_obj_set_style_text_font(detail_name_label,
        &lv_font_montserrat_32, 0);

    // State label
    detail_state_label = lv_label_create(detail_content);

    // Format state based on type
    char state_buf[64];
    if (!dev->reachable) {
        snprintf(state_buf, sizeof(state_buf), "State: Unreachable");
        lv_obj_set_style_text_color(detail_state_label,
            lv_color_hex(0x777777), 0);
    } else {
        lv_obj_set_style_text_color(detail_state_label,
            lv_color_hex(0xFFFFFF), 0);
        switch (dev->category) {
        case DEVICE_CAT_DOOR_LOCK:
            snprintf(state_buf, sizeof(state_buf),
                     "State: %s",
                     dev->lock_state == 1 ? "Locked" : "Unlocked");
            break;
        case DEVICE_CAT_THERMOSTAT:
            snprintf(state_buf, sizeof(state_buf),
                     "Temp: %.1fC  Heat: %.1fC  Cool: %.1fC  "
                     "Mode: %u",
                     (float)dev->local_temp / 100.0f,
                     (float)dev->setpoint_heat / 100.0f,
                     (float)dev->setpoint_cool / 100.0f,
                     dev->thermostat_mode);
            break;
        case DEVICE_CAT_WINDOW_COVERING:
            snprintf(state_buf, sizeof(state_buf),
                     "Position: %u%%", dev->cover_position);
            break;
        case DEVICE_CAT_TEMP_SENSOR:
            snprintf(state_buf, sizeof(state_buf),
                     "Temperature: %.1fC",
                     (float)dev->temperature / 100.0f);
            break;
        case DEVICE_CAT_HUMIDITY_SENSOR:
            snprintf(state_buf, sizeof(state_buf),
                     "Humidity: %.1f%%",
                     (float)dev->humidity / 100.0f);
            break;
        case DEVICE_CAT_OCCUPANCY_SENSOR:
            snprintf(state_buf, sizeof(state_buf),
                     "Occupancy: %s",
                     dev->occupancy ? "Occupied" : "Clear");
            break;
        case DEVICE_CAT_CONTACT_SENSOR:
            snprintf(state_buf, sizeof(state_buf),
                     "Contact: %s",
                     dev->contact ? "Closed" : "Open");
            break;
        case DEVICE_CAT_LIGHT_SENSOR:
            snprintf(state_buf, sizeof(state_buf),
                     "Illuminance: %u lux", dev->illuminance);
            break;
        default:
            if (cat_has_level(dev->category)) {
                snprintf(state_buf, sizeof(state_buf),
                         "State: %s  Level: %u%%",
                         dev->on_off ? "ON" : "OFF",
                         (unsigned)(dev->level * 100 / 254));
            } else {
                snprintf(state_buf, sizeof(state_buf),
                         "State: %s",
                         dev->on_off ? "ON" : "OFF");
            }
            break;
        }
    }
    lv_label_set_text(detail_state_label, state_buf);

    // ---- Type-specific controls ----
    device_category_t cat = dev->category;

    // On/Off buttons for applicable types
    if (cat_has_onoff(cat) && !cat_is_sensor(cat)) {
        lv_obj_t *btn_row = lv_obj_create(detail_content);
        lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(btn_row, 8, 0);
        lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

        detail_add_btn(btn_row, "ON", btn_on_cb);
        detail_add_btn(btn_row, "OFF", btn_off_cb);
        detail_add_btn(btn_row, "TOGGLE", btn_toggle_cb);
    }

    // Level slider for dimmable types
    if (cat_has_level(cat)) {
        detail_add_slider("Brightness:", 0, 254,
                          dev->level, slider_level_cb);
    }

    // Color temp slider
    if (cat == DEVICE_CAT_COLOR_TEMP_LIGHT ||
        cat == DEVICE_CAT_COLOR_LIGHT) {
        detail_add_slider("Color Temp (mireds):", 153, 500,
                          dev->color_temp_mireds,
                          slider_color_temp_cb);
    }

    // Hue + Saturation sliders for full color lights
    if (cat == DEVICE_CAT_COLOR_LIGHT) {
        detail_add_slider("Hue:", 0, 254,
                          dev->hue, slider_hue_cb);
        detail_add_slider("Saturation:", 0, 254,
                          dev->saturation, slider_sat_cb);
    }

    // Door lock controls
    if (cat == DEVICE_CAT_DOOR_LOCK) {
        lv_obj_t *btn_row = lv_obj_create(detail_content);
        lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(btn_row, 8, 0);
        lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

        detail_add_btn(btn_row, LV_SYMBOL_CLOSE " Lock",
                       btn_lock_cb);
        detail_add_btn(btn_row, LV_SYMBOL_OK " Unlock",
                       btn_unlock_cb);
    }

    // Window covering controls
    if (cat == DEVICE_CAT_WINDOW_COVERING) {
        lv_obj_t *btn_row = lv_obj_create(detail_content);
        lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(btn_row, 8, 0);
        lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

        detail_add_btn(btn_row, LV_SYMBOL_UP " Open",
                       btn_cover_open_cb);
        detail_add_btn(btn_row, LV_SYMBOL_DOWN " Close",
                       btn_cover_close_cb);
        detail_add_btn(btn_row, LV_SYMBOL_PAUSE " Stop",
                       btn_cover_stop_cb);

        detail_add_slider("Position:", 0, 100,
                          dev->cover_position,
                          slider_cover_pos_cb);
    }

    // Info label
    detail_info_label = lv_label_create(detail_content);
    char info[128];
    snprintf(info, sizeof(info),
             "Node ID: 0x%llX\nEndpoint: %u\nType: 0x%04lX",
             (unsigned long long)dev->node_id,
             dev->endpoint_id,
             (unsigned long)dev->device_type_id);
    lv_label_set_text(detail_info_label, info);

    // Rename section
    lv_obj_t *rename_row = lv_obj_create(detail_content);
    lv_obj_set_size(rename_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rename_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(rename_row, 4, 0);
    lv_obj_clear_flag(rename_row, LV_OBJ_FLAG_SCROLLABLE);

    detail_rename_ta = lv_textarea_create(rename_row);
    lv_textarea_set_one_line(detail_rename_ta, true);
    lv_textarea_set_placeholder_text(detail_rename_ta, "New name");
    lv_textarea_set_text(detail_rename_ta, dev->name);
    lv_obj_set_flex_grow(detail_rename_ta, 1);
    apply_focus_style(detail_rename_ta);
    lv_group_add_obj(grp_detail, detail_rename_ta);

    detail_add_btn(rename_row, "Save", btn_rename_cb);

    // Reconnect button
    detail_add_btn(detail_content,
        LV_SYMBOL_REFRESH " Reconnect", btn_reconnect_cb);

    // Unpair button
    lv_obj_t *unpair_btn = lv_button_create(detail_content);
    lv_obj_set_width(unpair_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(unpair_btn, lv_color_hex(0xC62828), 0);
    lv_obj_t *unpair_lbl = lv_label_create(unpair_btn);
    lv_label_set_text(unpair_lbl, "Unpair Device");
    lv_obj_center(unpair_lbl);
    lv_obj_add_event_cb(unpair_btn, btn_unpair_cb,
        LV_EVENT_CLICKED, NULL);
    apply_focus_style(unpair_btn);
    lv_group_add_obj(grp_detail, unpair_btn);

    // Key hints
    lv_obj_t *detail_hints = create_key_hints_bar(scr_detail);
    hint_add_text(detail_hints, "Tab: Next");
    hint_add_text(detail_hints, "Enter: Confirm");
    hint_add_icon(detail_hints, &img_dsc_esc);
    hint_add_text(detail_hints, "Back");
    hint_add_icon(detail_hints, &img_dsc_f3);
    hint_add_text(detail_hints, "Screenshot");

    switch_to_screen(scr_detail, grp_detail);
}

// ---- Public API ----
void ui_screens_init(void) {
    s_ui_event_queue = xQueueCreate(16, sizeof(matter_event_t));

    init_key_icons();
    create_dashboard_screen();
    create_commission_screen();
    create_detail_screen();

    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_add_event_cb(indev, global_key_cb,
                LV_EVENT_PRESSED, NULL);
        }
    }

    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);

    s_event_timer = lv_timer_create(event_timer_cb, 100, NULL);
}

void ui_post_event(matter_event_t event) {
    if (s_ui_event_queue) {
        xQueueSend(s_ui_event_queue, &event, 0);
    }
}

void ui_update_device_state(uint64_t node_id) {
    refresh_dashboard();

    if (dashboard_status_label) {
        int total = device_manager_count();
        int reachable = 0;
        for (int i = 0; i < total; i++) {
            const matter_device_t *d = device_manager_get(i);
            if (d && d->reachable) reachable++;
        }
        if (reachable >= total) {
            lv_label_set_text(dashboard_status_label,
                "Commissioner ready");
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf),
                "Reconnecting to devices... (%d/%d)",
                reachable, total);
            lv_label_set_text(dashboard_status_label, buf);
        }
    }

    // Update detail screen if showing this device
    if (detail_node_id == node_id &&
        lv_screen_active() == scr_detail) {
        show_detail_for_device(node_id);
    }
}

void ui_show_dashboard(void) {
    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);
}
