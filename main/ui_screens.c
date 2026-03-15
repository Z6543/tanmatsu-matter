#include "ui_screens.h"
#include "device_manager.h"
#include "matter_commission.h"
#include "matter_device_control.h"

#include "bsp_lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nvs.h"
#include <stdlib.h>

static const char *TAG = "ui";

#if LV_USE_SNAPSHOT
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>
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
static lv_obj_t *dashboard_thread_btn = NULL;

// Number of fixed header buttons in grp_dashboard (Thread + Add)
#define DASHBOARD_HEADER_BTNS 2

// Commission widgets
#define NUM_COMMISSION_METHODS 6
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
static lv_obj_t *commission_start_btn = NULL;
// 0=PIN, 1=Disc+Pass, 2=Manual, 3=QR, 4=BLE+WiFi, 5=BLE+Thread
static int       commission_method = 0;

// Detail widgets
static lv_obj_t *detail_name_label = NULL;
static lv_obj_t *detail_state_label = NULL;
static lv_obj_t *detail_info_label = NULL;
static lv_obj_t *detail_rename_ta = NULL;
static uint64_t  detail_node_id = 0;

// Commissioning state
static uint64_t s_pending_node_id = 0;
static char     s_pending_name[MATTER_DEVICE_NAME_LEN] = {};
static bool     s_commissioning_active = false;

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

    // Temporary group so keyboard navigates the dialog
    confirm_dlg_group = lv_group_create();
    lv_group_add_obj(confirm_dlg_group, no_btn);
    lv_group_add_obj(confirm_dlg_group, yes_btn);
    activate_group(confirm_dlg_group);
    lv_group_focus_obj(no_btn);
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
                } else {
                    lv_label_set_text(dashboard_status_label,
                        "Commissioner ready");
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
            // Use user-provided name if set, otherwise discovered name
            const char *name = s_pending_name[0] ? s_pending_name
                                                 : ev.msg;
            uint16_t ep = ev.endpoint_id ? ev.endpoint_id : 1;
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "Success!");
            }
            device_manager_add(ev.node_id, ep, name);
            matter_device_subscribe_onoff(ev.node_id, ep);
            refresh_dashboard();
            break;
        }
        case MATTER_EVENT_COMMISSION_FAILED:
            s_commissioning_active = false;
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "Commissioning failed!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_COMMISSION_TIMEOUT:
            s_commissioning_active = false;
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "Commissioning timed out (90s)");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_ATTESTATION_WARNING:
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
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label, ev.msg);
                lv_obj_set_style_text_color(dashboard_status_label,
                    lv_color_hex(0xFFFFFF), 0);
            }
            if (dashboard_thread_btn) {
                lv_obj_clear_state(dashboard_thread_btn,
                    LV_STATE_DISABLED);
                lv_obj_t *lbl = lv_obj_get_child(
                    dashboard_thread_btn, 0);
                if (lbl) lv_label_set_text(lbl,
                    LV_SYMBOL_REFRESH " Start Thread");
            }
            break;
        }
    }
}

// ---- Navigation callbacks ----
static void btn_start_thread_cb(lv_event_t *e) {
    (void)e;
    if (dashboard_status_label) {
        lv_label_set_text(dashboard_status_label,
            "Starting Thread border router...");
        lv_obj_set_style_text_color(dashboard_status_label,
            lv_color_hex(0x888888), 0);
    }
    esp_err_t err = matter_start_thread_br();
    if (err == ESP_OK) {
        if (dashboard_thread_btn) {
            lv_obj_t *lbl = lv_obj_get_child(
                dashboard_thread_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Thread: ON");
            lv_obj_add_state(dashboard_thread_btn,
                LV_STATE_DISABLED);
        }
        if (dashboard_status_label) {
            lv_label_set_text(dashboard_status_label,
                "Thread border router started");
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        if (dashboard_status_label) {
            lv_label_set_text(dashboard_status_label,
                "WiFi not connected. Connect first.");
        }
    } else {
        if (dashboard_status_label) {
            lv_label_set_text(dashboard_status_label,
                "Thread border router failed to start");
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

    // Focus the first visible text entry for the selected method
    bool need_disc = (commission_method == 1 ||
                      commission_method == 4 ||
                      commission_method == 5);
    lv_obj_t *first_ta = need_disc ? commission_disc_ta
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

static void update_commission_fields(void) {
    bool show_disc = (commission_method == 1 ||
                      commission_method == 4 ||
                      commission_method == 5);
    set_field_visible(commission_disc_label, commission_disc_ta, show_disc);

    bool show_hints = (commission_method == 1);
    set_field_visible(commission_hints_label, commission_hints_ta, show_hints);

    bool show_thread = (commission_method == 5);
    set_field_visible(commission_thread_label, commission_thread_ta, show_thread);

    static const char *labels[] = {
        "Setup PIN Code:", "Passcode:", "Manual Pairing Code:",
        "QR Code Payload:", "Passcode:", "Passcode:"
    };
    static const char *placeholders[] = {
        "e.g. 20212020", "e.g. 20212020", "e.g. 34970112332",
        "e.g. MT:...", "e.g. 20212020", "e.g. 20212020"
    };
    lv_label_set_text(commission_code_label, labels[commission_method]);
    lv_textarea_set_placeholder_text(commission_code_ta, placeholders[commission_method]);
}

static void method_radio_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (int i = 0; i < NUM_COMMISSION_METHODS; i++) {
        if (obj == commission_method_radios[i]) {
            commission_method = i;
            lv_obj_add_state(commission_method_radios[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(commission_method_radios[i], LV_STATE_CHECKED);
        }
    }
    save_commission_method(commission_method);
    update_commission_fields();

    // Focus the first visible text entry for the selected method
    bool need_disc = (commission_method == 1 ||
                      commission_method == 4 ||
                      commission_method == 5);
    lv_obj_t *first_ta = need_disc ? commission_disc_ta
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
    // Matter spec invalid codes: all-same-digit and 12345678/87654321
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
            "Enter setup PIN code!", "Enter passcode!",
            "Enter manual pairing code!", "Enter QR code payload!",
            "Enter passcode!", "Enter passcode!"
        };
        lv_label_set_text(commission_status_label, prompts[commission_method]);
        return;
    }

    strncpy(s_pending_name, name_str, MATTER_DEVICE_NAME_LEN - 1);
    s_pending_name[MATTER_DEVICE_NAME_LEN - 1] = '\0';

    s_pending_node_id = device_manager_next_node_id();
    esp_err_t err = ESP_FAIL;

    lv_label_set_text(commission_status_label, "Starting...");
    lv_obj_add_state(commission_start_btn, LV_STATE_DISABLED);

    // Methods that require a numeric PIN code
    bool needs_pincode = (commission_method != 2 && commission_method != 3);
    uint32_t pincode = 0;
    if (needs_pincode && !is_valid_pincode(code_str, &pincode)) {
        lv_label_set_text(commission_status_label,
            "Invalid PIN (1-99999998)");
        lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
        return;
    }

    // Methods that require a discriminator
    bool needs_disc = (commission_method == 1 || commission_method == 4 ||
                       commission_method == 5);
    uint16_t disc = 0;
    if (needs_disc) {
        const char *disc_str = lv_textarea_get_text(commission_disc_ta);
        if (!is_valid_discriminator(disc_str, &disc)) {
            lv_label_set_text(commission_status_label,
                "Invalid discriminator (0-4095)");
            lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            return;
        }
    }

    switch (commission_method) {
    case 0:
        err = matter_commission_on_network(s_pending_node_id, pincode);
        break;
    case 1: {
        const char *hints_str = lv_textarea_get_text(commission_hints_ta);
        uint8_t hints = 0;
        if (hints_str && is_all_digits(hints_str) && hints_str[0]) {
            hints = (uint8_t)strtol(hints_str, NULL, 10);
        }
        err = matter_commission_disc_pass(
            s_pending_node_id, pincode, disc, hints);
        break;
    }
    case 2:
        err = matter_commission_setup_code(s_pending_node_id, code_str);
        break;
    case 3:
        err = matter_commission_setup_code(s_pending_node_id, code_str);
        break;
    case 4:
        err = matter_commission_ble_wifi(s_pending_node_id, pincode, disc);
        break;
    case 5: {
        const char *thread_str = lv_textarea_get_text(commission_thread_ta);
        err = matter_commission_ble_thread(
            s_pending_node_id, pincode, disc, thread_str);
        break;
    }
    }

    if (err != ESP_OK) {
        const char *msg = "Failed to start pairing";
        if (err == ESP_ERR_INVALID_STATE &&
            commission_method <= 3) {
            msg = "WiFi not connected. Connect to WiFi first "
                  "for on-network commissioning.";
        } else if (commission_method == 5 &&
                   err == ESP_ERR_NOT_FOUND) {
            msg = "No Thread dataset: border router has no active "
                  "network. Enter a dataset manually.";
        }
        lv_label_set_text(commission_status_label, msg);
        lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
    } else {
        s_commissioning_active = true;
        lv_label_set_text(commission_status_label, "Establishing PASE...");
    }
}

// ---- Dashboard card callbacks ----
static void card_click_cb(lv_event_t *e) {
    if (s_commissioning_active) return;
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    const matter_device_t *dev = device_manager_find(node_id);
    if (dev) {
        matter_device_send_toggle(dev->node_id, dev->endpoint_id);
    }
}

static void card_key_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    if (key == LV_KEY_HOME) {  // F1: details
        show_detail_for_device(node_id);
    } else if (key == LV_KEY_END) {  // F2: force remove
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

static void btn_rename_cb(lv_event_t *e) {
    (void)e;
    const char *new_name = lv_textarea_get_text(detail_rename_ta);
    if (new_name && new_name[0]) {
        device_manager_rename(detail_node_id, new_name);
        device_manager_save();
        lv_label_set_text(detail_name_label, new_name);
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

static lv_image_dsc_t img_dsc_esc;
static lv_image_dsc_t img_dsc_f1;
static lv_image_dsc_t img_dsc_f2;
static lv_image_dsc_t img_dsc_f3;
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

// Apply a strong, consistent focus indicator to any focusable widget.
// Uses a thick cyan border so keyboard focus is immediately obvious.
static void apply_focus_style(lv_obj_t *obj) {
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
    // Also cover focused+checked combo (e.g. checkable radio buttons)
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED | LV_STATE_CHECKED);
}

// ---- Screenshot (F3) ----
#if LV_USE_SNAPSHOT
static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Stream base64 in chunks of 57 raw bytes (= 76 b64 chars).
// 57 is a multiple of 3 so no padding until the final chunk.
#define B64_RAW_CHUNK 57
#define B64_OUT_CHUNK 80  // 57*4/3 + margin

static void b64_stream(
    const uint8_t *data, uint32_t len, uint32_t *carry) {
    // carry tracks leftover bytes from previous call
    (void)carry;
    uint8_t out[B64_OUT_CHUNK];
    size_t olen;
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > B64_RAW_CHUNK) chunk = B64_RAW_CHUNK;
        // Only last chunk of entire stream may be non-3-aligned
        mbedtls_base64_encode(
            out, sizeof(out), &olen,
            data + off, chunk);
        out[olen] = '\0';
        printf("%s\n", (char *)out);
        off += chunk;
    }
}

static void take_screenshot(void) {
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
    uint32_t row_bytes = w * 2;  // RGB565
    // BMP rows must be 4-byte aligned
    uint32_t bmp_row = (row_bytes + 3) & ~3u;
    uint32_t pixel_size = bmp_row * h;
    uint32_t hdr_size = 14 + 40;
    uint32_t file_size = hdr_size + pixel_size;

    // Build a single contiguous BMP in SPIRAM so we can
    // base64-stream it with clean 3-byte aligned chunks.
    uint8_t *bmp = heap_caps_malloc(
        file_size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        ESP_LOGE(TAG, "BMP alloc failed (%lu bytes)",
                 (unsigned long)file_size);
        lv_draw_buf_destroy(snap);
        return;
    }

    // BMP file header (14 bytes)
    memset(bmp, 0, hdr_size);
    bmp[0] = 'B'; bmp[1] = 'M';
    write_le32(&bmp[2], file_size);
    write_le32(&bmp[10], hdr_size);
    // DIB header (40 bytes)
    write_le32(&bmp[14], 40);
    write_le32(&bmp[18], w);
    write_le32(&bmp[22], h);
    write_le16(&bmp[26], 1);     // planes
    write_le16(&bmp[28], 16);    // bpp
    write_le32(&bmp[30], 0);     // BI_RGB
    write_le32(&bmp[34], pixel_size);

    // Copy pixel rows bottom-to-top (BMP row order)
    uint32_t pad_bytes = bmp_row - row_bytes;
    uint8_t *dst = bmp + hdr_size;
    for (int y = (int)h - 1; y >= 0; y--) {
        const uint8_t *src = snap->data + (y * stride);
        memcpy(dst, src, row_bytes);
        if (pad_bytes > 0)
            memset(dst + row_bytes, 0, pad_bytes);
        dst += bmp_row;
    }

    lv_draw_buf_destroy(snap);

    printf("\n===SCREENSHOT_START===\n");
    b64_stream(bmp, file_size, NULL);
    printf("===SCREENSHOT_END===\n");
    fflush(stdout);

    free(bmp);
    ESP_LOGI(TAG, "Screenshot done (%lux%lu)",
             (unsigned long)w, (unsigned long)h);
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

    // Header with Start Thread and Add buttons
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
        btn_start_thread_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(dashboard_thread_btn);
    lv_group_add_obj(grp_dashboard, dashboard_thread_btn);

    lv_obj_t *add_btn = lv_button_create(header);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add");
    lv_obj_add_event_cb(add_btn, btn_add_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(add_btn);
    lv_group_add_obj(grp_dashboard, add_btn);

    // Device cards container
    dashboard_container = lv_obj_create(scr_dashboard);
    lv_obj_set_size(dashboard_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dashboard_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_grow(dashboard_container, 1);
    lv_obj_set_style_pad_all(dashboard_container, 4, 0);
    lv_obj_set_style_pad_gap(dashboard_container, 8, 0);

    // Status bar
    dashboard_status_label = lv_label_create(scr_dashboard);
    lv_label_set_text(dashboard_status_label, "Initializing...");
    lv_obj_set_style_text_color(dashboard_status_label, lv_color_hex(0x888888), 0);

    lv_obj_t *dash_hints = create_key_hints_bar(scr_dashboard);
    hint_add_text(dash_hints, "Enter: Toggle");
    hint_add_icon(dash_hints, &img_dsc_f1);
    hint_add_text(dash_hints, "Details");
    hint_add_icon(dash_hints, &img_dsc_f2);
    hint_add_text(dash_hints, "Force Remove");
}

static void refresh_dashboard(void) {
    if (!dashboard_container) return;
    lv_obj_clean(dashboard_container);

    // Remove all card objects from group (keep header buttons)
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
        lv_obj_set_style_pad_all(card, 6, 0);

        if (!dev->reachable) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x303030), 0);
            lv_obj_set_style_text_color(card, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
        } else if (dev->on_off) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x2E7D32), 0);
            lv_obj_set_style_text_color(card, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x424242), 0);
            lv_obj_set_style_text_color(card, lv_color_hex(0xFFFFFF), 0);
        }

        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, dev->name);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, 120);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *state_lbl = lv_label_create(card);
        if (!dev->reachable) {
            lv_label_set_text(state_lbl, "Unreachable");
        } else {
            lv_label_set_text(state_lbl, dev->on_off ? "ON" : "OFF");
        }

        void *user_data = (void *)(uintptr_t)dev->node_id;
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_SHORT_CLICKED, user_data);
        lv_obj_add_event_cb(card, card_key_cb, LV_EVENT_KEY, user_data);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        // Focus style: thick bright outline + border when focused via keyboard
        lv_obj_set_style_outline_width(card, 4, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(card, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(card, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(card, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(card, 3, LV_STATE_FOCUSED);

        lv_group_add_obj(grp_dashboard, card);
    }

    // Focus first device card if available, otherwise first header button
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

    create_header(scr_commission, "Add New Device", btn_back_dashboard_cb, grp_commission);

    // Method selector - single row for all 5 methods
    lv_obj_t *method_row = lv_obj_create(scr_commission);
    lv_obj_set_size(method_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(method_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(method_row, 2, 0);
    lv_obj_set_style_pad_gap(method_row, 4, 0);
    lv_obj_clear_flag(method_row, LV_OBJ_FLAG_SCROLLABLE);

    static const char *method_labels[] = {
        "PIN", "Disc+Pass", "Manual", "QR", "BLE+WiFi", "BLE+Thread"
    };
    for (int i = 0; i < NUM_COMMISSION_METHODS; i++) {
        commission_method_radios[i] = lv_button_create(method_row);
        lv_obj_add_flag(commission_method_radios[i], LV_OBJ_FLAG_CHECKABLE);
        lv_obj_t *lbl = lv_label_create(commission_method_radios[i]);
        lv_label_set_text(lbl, method_labels[i]);
        lv_obj_add_event_cb(commission_method_radios[i], method_radio_cb, LV_EVENT_CLICKED, NULL);
        // Style for unselected state
        lv_obj_set_style_bg_color(commission_method_radios[i], lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(commission_method_radios[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(commission_method_radios[i], lv_color_hex(0x999999), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(commission_method_radios[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        // Style for selected (checked) state
        lv_obj_set_style_bg_color(commission_method_radios[i], lv_color_hex(0x00C853), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(commission_method_radios[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(commission_method_radios[i], lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(commission_method_radios[i], 2, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(commission_method_radios[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_CHECKED);
        apply_focus_style(commission_method_radios[i]);
        lv_group_add_obj(grp_commission, commission_method_radios[i]);
    }
    commission_method = load_commission_method();
    lv_obj_add_state(commission_method_radios[commission_method],
        LV_STATE_CHECKED);

    // Discriminator input (only visible for method 1)
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

    // Discovery hints (only visible for method 1: Disc+Passcode)
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

    // Thread dataset (only visible for method 5: Thread)
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

    // Code input
    commission_code_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_code_label, "Setup PIN Code:");

    commission_code_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_code_ta, true);
    lv_textarea_set_placeholder_text(commission_code_ta, "e.g. 20212020");
    lv_obj_set_width(commission_code_ta, LV_PCT(100));
    apply_focus_style(commission_code_ta);
    lv_group_add_obj(grp_commission, commission_code_ta);

    // Device name
    lv_obj_t *name_label = lv_label_create(scr_commission);
    lv_label_set_text(name_label, "Device Name:");

    commission_name_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_name_ta, true);
    lv_textarea_set_placeholder_text(commission_name_ta, "My Light");
    lv_obj_set_width(commission_name_ta, LV_PCT(100));
    apply_focus_style(commission_name_ta);
    lv_group_add_obj(grp_commission, commission_name_ta);

    // Start button
    commission_start_btn = lv_button_create(scr_commission);
    lv_obj_set_width(commission_start_btn, LV_PCT(100));
    lv_obj_t *start_lbl = lv_label_create(commission_start_btn);
    lv_label_set_text(start_lbl, "Start Commissioning");
    lv_obj_center(start_lbl);
    lv_obj_add_event_cb(commission_start_btn, btn_start_commission_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(commission_start_btn);
    lv_group_add_obj(grp_commission, commission_start_btn);

    // Status label
    commission_status_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_status_label, "");
    lv_obj_set_width(commission_status_label, LV_PCT(100));
    lv_label_set_long_mode(commission_status_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *comm_hints = create_key_hints_bar(scr_commission);
    hint_add_text(comm_hints, "Tab: Next field");
    hint_add_text(comm_hints, "Enter: Confirm");
    hint_add_icon(comm_hints, &img_dsc_esc);
    hint_add_text(comm_hints, "Back");

    // Apply persisted method's field visibility
    update_commission_fields();
}

static void create_detail_screen(void) {
    grp_detail = lv_group_create();

    scr_detail = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_detail, 4, 0);
    lv_obj_set_style_pad_gap(scr_detail, 4, 0);

    create_header(scr_detail, "", btn_back_detail_cb, grp_detail);

    detail_name_label = lv_label_create(scr_detail);
    lv_label_set_text(detail_name_label, "");
    lv_obj_set_style_text_font(detail_name_label, &lv_font_montserrat_32, 0);

    detail_state_label = lv_label_create(scr_detail);
    lv_label_set_text(detail_state_label, "");

    // Control buttons row
    lv_obj_t *btn_row = lv_obj_create(scr_detail);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *on_btn = lv_button_create(btn_row);
    lv_obj_t *on_lbl = lv_label_create(on_btn);
    lv_label_set_text(on_lbl, "ON");
    lv_obj_add_event_cb(on_btn, btn_on_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(on_btn);
    lv_group_add_obj(grp_detail, on_btn);

    lv_obj_t *off_btn = lv_button_create(btn_row);
    lv_obj_t *off_lbl = lv_label_create(off_btn);
    lv_label_set_text(off_lbl, "OFF");
    lv_obj_add_event_cb(off_btn, btn_off_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(off_btn);
    lv_group_add_obj(grp_detail, off_btn);

    lv_obj_t *toggle_btn = lv_button_create(btn_row);
    lv_obj_t *toggle_lbl = lv_label_create(toggle_btn);
    lv_label_set_text(toggle_lbl, "TOGGLE");
    lv_obj_add_event_cb(toggle_btn, btn_toggle_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(toggle_btn);
    lv_group_add_obj(grp_detail, toggle_btn);

    // Info label
    detail_info_label = lv_label_create(scr_detail);
    lv_label_set_text(detail_info_label, "");

    // Rename section
    lv_obj_t *rename_row = lv_obj_create(scr_detail);
    lv_obj_set_size(rename_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rename_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(rename_row, 4, 0);
    lv_obj_clear_flag(rename_row, LV_OBJ_FLAG_SCROLLABLE);

    detail_rename_ta = lv_textarea_create(rename_row);
    lv_textarea_set_one_line(detail_rename_ta, true);
    lv_textarea_set_placeholder_text(detail_rename_ta, "New name");
    lv_obj_set_flex_grow(detail_rename_ta, 1);
    apply_focus_style(detail_rename_ta);
    lv_group_add_obj(grp_detail, detail_rename_ta);

    lv_obj_t *save_btn = lv_button_create(rename_row);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_add_event_cb(save_btn, btn_rename_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(save_btn);
    lv_group_add_obj(grp_detail, save_btn);

    // Unpair button
    lv_obj_t *unpair_btn = lv_button_create(scr_detail);
    lv_obj_set_width(unpair_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(unpair_btn, lv_color_hex(0xC62828), 0);
    lv_obj_t *unpair_lbl = lv_label_create(unpair_btn);
    lv_label_set_text(unpair_lbl, "Unpair Device");
    lv_obj_center(unpair_lbl);
    lv_obj_add_event_cb(unpair_btn, btn_unpair_cb, LV_EVENT_CLICKED, NULL);
    apply_focus_style(unpair_btn);
    lv_group_add_obj(grp_detail, unpair_btn);

    lv_obj_t *detail_hints = create_key_hints_bar(scr_detail);
    hint_add_text(detail_hints, "Tab: Next");
    hint_add_text(detail_hints, "Enter: Confirm");
    hint_add_icon(detail_hints, &img_dsc_esc);
    hint_add_text(detail_hints, "Back");
}

static void show_detail_for_device(uint64_t node_id) {
    const matter_device_t *dev = device_manager_find(node_id);
    if (!dev) return;

    detail_node_id = node_id;
    lv_label_set_text(detail_name_label, dev->name);
    if (!dev->reachable) {
        lv_label_set_text(detail_state_label, "State: Unreachable");
        lv_obj_set_style_text_color(detail_state_label,
            lv_color_hex(0x777777), 0);
    } else {
        lv_label_set_text(detail_state_label,
            dev->on_off ? "State: ON" : "State: OFF");
        lv_obj_set_style_text_color(detail_state_label,
            lv_color_hex(0xFFFFFF), 0);
    }

    char info[128];
    snprintf(info, sizeof(info), "Node ID: 0x%llX\nEndpoint: %u",
             (unsigned long long)dev->node_id, dev->endpoint_id);
    lv_label_set_text(detail_info_label, info);

    lv_textarea_set_text(detail_rename_ta, dev->name);

    switch_to_screen(scr_detail, grp_detail);
}

// ---- Public API ----
void ui_screens_init(void) {
    s_ui_event_queue = xQueueCreate(16, sizeof(matter_event_t));

    init_key_icons();
    create_dashboard_screen();
    create_commission_screen();
    create_detail_screen();

    // Register global key handler on all keypad indevs
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

void ui_update_device_state(uint64_t node_id, bool on_off) {
    refresh_dashboard();

    // Update dashboard status with connection progress
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
    if (detail_node_id == node_id && lv_screen_active() == scr_detail) {
        const matter_device_t *dev = device_manager_find(node_id);
        if (dev && !dev->reachable) {
            lv_label_set_text(detail_state_label, "State: Unreachable");
            lv_obj_set_style_text_color(detail_state_label,
                lv_color_hex(0x777777), 0);
        } else {
            lv_label_set_text(detail_state_label,
                on_off ? "State: ON" : "State: OFF");
            lv_obj_set_style_text_color(detail_state_label,
                lv_color_hex(0xFFFFFF), 0);
        }
    }
}

void ui_show_dashboard(void) {
    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);
}
