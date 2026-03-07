#include "ui_screens.h"
#include "device_manager.h"
#include "matter_commission.h"
#include "matter_device_control.h"

#include "bsp_lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

// Event queue for Matter -> UI communication
static QueueHandle_t s_ui_event_queue = NULL;

// Screen objects
static lv_obj_t *scr_dashboard = NULL;
static lv_obj_t *scr_commission = NULL;
static lv_obj_t *scr_detail = NULL;

// Dashboard widgets
static lv_obj_t *dashboard_container = NULL;
static lv_obj_t *dashboard_status_label = NULL;

// Commission widgets
static lv_obj_t *commission_method_radios[2] = {};
static lv_obj_t *commission_code_ta = NULL;
static lv_obj_t *commission_name_ta = NULL;
static lv_obj_t *commission_status_label = NULL;
static lv_obj_t *commission_start_btn = NULL;
static int       commission_method = 0;  // 0=setup PIN code, 1=QR code

// Detail widgets
static lv_obj_t *detail_name_label = NULL;
static lv_obj_t *detail_state_label = NULL;
static lv_obj_t *detail_info_label = NULL;
static lv_obj_t *detail_rename_ta = NULL;
static uint64_t  detail_node_id = 0;

// Commissioning state
static uint64_t s_pending_node_id = 0;
static char     s_pending_name[MATTER_DEVICE_NAME_LEN] = {};

// LVGL timer for polling event queue
static lv_timer_t *s_event_timer = NULL;

// Forward declarations
static void create_dashboard_screen(void);
static void create_commission_screen(void);
static void create_detail_screen(void);
static void refresh_dashboard(void);
static void show_detail_for_device(uint64_t node_id);

// ---- Event queue timer callback ----
static void event_timer_cb(lv_timer_t *timer) {
    (void)timer;
    matter_event_t ev;
    while (xQueueReceive(s_ui_event_queue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case MATTER_EVENT_STACK_READY:
            if (dashboard_status_label) {
                lv_label_set_text(dashboard_status_label, "Commissioner ready");
            }
            break;
        case MATTER_EVENT_PASE_SUCCESS:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "PASE established...");
            }
            break;
        case MATTER_EVENT_PASE_FAILED:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "PASE failed!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        case MATTER_EVENT_COMMISSION_SUCCESS:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "Success!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            device_manager_add(ev.node_id, 1, s_pending_name);
            matter_device_subscribe_onoff(ev.node_id, 1);
            refresh_dashboard();
            break;
        case MATTER_EVENT_COMMISSION_FAILED:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label, "Commissioning failed!");
            }
            if (commission_start_btn) lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
            break;
        }
    }
}

// ---- Navigation callbacks ----
static void btn_add_cb(lv_event_t *e) {
    (void)e;
    if (commission_status_label) lv_label_set_text(commission_status_label, "");
    if (commission_code_ta) lv_textarea_set_text(commission_code_ta, "");
    if (commission_name_ta) lv_textarea_set_text(commission_name_ta, "");
    lv_screen_load(scr_commission);
}

static void btn_back_dashboard_cb(lv_event_t *e) {
    (void)e;
    refresh_dashboard();
    lv_screen_load(scr_dashboard);
}

// ---- Commission screen callbacks ----
static void method_radio_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (int i = 0; i < 2; i++) {
        if (obj == commission_method_radios[i]) {
            commission_method = i;
            lv_obj_add_state(commission_method_radios[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(commission_method_radios[i], LV_STATE_CHECKED);
        }
    }
}

static void btn_start_commission_cb(lv_event_t *e) {
    (void)e;
    const char *code_str = lv_textarea_get_text(commission_code_ta);
    const char *name_str = lv_textarea_get_text(commission_name_ta);

    if (!code_str || code_str[0] == '\0') {
        lv_label_set_text(commission_status_label, commission_method == 0 ? "Enter setup PIN code!" : "Enter QR code!");
        return;
    }

    strncpy(s_pending_name, name_str, MATTER_DEVICE_NAME_LEN - 1);
    s_pending_name[MATTER_DEVICE_NAME_LEN - 1] = '\0';

    s_pending_node_id = device_manager_next_node_id();
    esp_err_t err = ESP_FAIL;

    lv_label_set_text(commission_status_label, "Starting...");
    lv_obj_add_state(commission_start_btn, LV_STATE_DISABLED);

    switch (commission_method) {
    case 0: {
        // Setup PIN code, on-network
        uint32_t pincode = (uint32_t)atol(code_str);
        err = matter_commission_on_network(s_pending_node_id, pincode);
        break;
    }
    case 1: {
        // QR code payload
        err = matter_commission_code(s_pending_node_id, code_str);
        break;
    }
    }

    if (err != ESP_OK) {
        lv_label_set_text(commission_status_label, "Failed to start pairing");
        lv_obj_clear_state(commission_start_btn, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(commission_status_label, "Establishing PASE...");
    }
}

// ---- Dashboard card callbacks ----
static void card_click_cb(lv_event_t *e) {
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    const matter_device_t *dev = device_manager_find(node_id);
    if (dev) {
        matter_device_send_toggle(dev->node_id, dev->endpoint_id);
    }
}

static void card_long_press_cb(lv_event_t *e) {
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    show_detail_for_device(node_id);
}

// ---- Detail screen callbacks ----
static void btn_on_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_on(dev->node_id, dev->endpoint_id);
}

static void btn_off_cb(lv_event_t *e) {
    (void)e;
    const matter_device_t *dev = device_manager_find(detail_node_id);
    if (dev) matter_device_send_off(dev->node_id, dev->endpoint_id);
}

static void btn_toggle_cb(lv_event_t *e) {
    (void)e;
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
    matter_device_unpair(detail_node_id);
    device_manager_remove(detail_node_id);
    refresh_dashboard();
    lv_screen_load(scr_dashboard);
}

static void btn_back_detail_cb(lv_event_t *e) {
    (void)e;
    refresh_dashboard();
    lv_screen_load(scr_dashboard);
}

// ---- Screen creation ----
static lv_obj_t *create_header(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb) {
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (back_cb) {
        lv_obj_t *btn = lv_button_create(header);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT);
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_flex_grow(lbl, 1);

    return header;
}

static void create_dashboard_screen(void) {
    scr_dashboard = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_dashboard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_dashboard, 4, 0);

    // Header with Add button
    lv_obj_t *header = lv_obj_create(scr_dashboard);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Matter Commissioner");

    lv_obj_t *add_btn = lv_button_create(header);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, "+ Add");
    lv_obj_add_event_cb(add_btn, btn_add_cb, LV_EVENT_CLICKED, NULL);
    lv_group_t *grp = lv_group_get_default();
    if (grp) lv_group_add_obj(grp, add_btn);

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
}

static void refresh_dashboard(void) {
    if (!dashboard_container) return;
    lv_obj_clean(dashboard_container);

    int count = device_manager_count();
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(dashboard_container);
        lv_label_set_text(lbl, "No devices.\nTap '+ Add' to commission.");
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

        if (dev->on_off) {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x2E7D32), 0);
            lv_obj_set_style_text_color(card, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(card, lv_color_hex(0x424242), 0);
            lv_obj_set_style_text_color(card, lv_color_hex(0xCCCCCC), 0);
        }

        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, dev->name);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, 120);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *state_lbl = lv_label_create(card);
        lv_label_set_text(state_lbl, dev->on_off ? "ON" : "OFF");

        void *user_data = (void *)(uintptr_t)dev->node_id;
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_SHORT_CLICKED, user_data);
        lv_obj_add_event_cb(card, card_long_press_cb, LV_EVENT_LONG_PRESSED, user_data);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        // Focus style: bright border when focused via keyboard
        lv_obj_set_style_outline_width(card, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(card, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(card, 2, LV_STATE_FOCUSED);

        // Add to default group for keyboard navigation
        lv_group_t *grp = lv_group_get_default();
        if (grp) lv_group_add_obj(grp, card);
    }
}

static void create_commission_screen(void) {
    scr_commission = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_commission, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_commission, 4, 0);
    lv_obj_set_style_pad_gap(scr_commission, 4, 0);

    create_header(scr_commission, "Add New Device", btn_back_dashboard_cb);

    // Method selector
    lv_obj_t *method_row = lv_obj_create(scr_commission);
    lv_obj_set_size(method_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(method_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(method_row, 2, 0);
    lv_obj_clear_flag(method_row, LV_OBJ_FLAG_SCROLLABLE);

    static const char *method_labels[] = {"Setup PIN Code", "QR Code"};
    for (int i = 0; i < 2; i++) {
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
        // Style for selected (checked) state — high contrast
        lv_obj_set_style_bg_color(commission_method_radios[i], lv_color_hex(0x00C853), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(commission_method_radios[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(commission_method_radios[i], lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(commission_method_radios[i], 2, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(commission_method_radios[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_CHECKED);
    }
    lv_obj_add_state(commission_method_radios[0], LV_STATE_CHECKED);
    commission_method = 0;

    // Code input
    lv_obj_t *code_label = lv_label_create(scr_commission);
    lv_label_set_text(code_label, "Setup PIN Code or QR Code:");

    commission_code_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_code_ta, true);
    lv_textarea_set_placeholder_text(commission_code_ta, "e.g. 20212020 or MT:...");
    lv_obj_set_width(commission_code_ta, LV_PCT(100));

    // Device name
    lv_obj_t *name_label = lv_label_create(scr_commission);
    lv_label_set_text(name_label, "Device Name:");

    commission_name_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_name_ta, true);
    lv_textarea_set_placeholder_text(commission_name_ta, "My Light");
    lv_obj_set_width(commission_name_ta, LV_PCT(100));

    // Start button
    commission_start_btn = lv_button_create(scr_commission);
    lv_obj_set_width(commission_start_btn, LV_PCT(100));
    lv_obj_t *start_lbl = lv_label_create(commission_start_btn);
    lv_label_set_text(start_lbl, "Start Commissioning");
    lv_obj_center(start_lbl);
    lv_obj_add_event_cb(commission_start_btn, btn_start_commission_cb, LV_EVENT_CLICKED, NULL);

    // Status label
    commission_status_label = lv_label_create(scr_commission);
    lv_label_set_text(commission_status_label, "");
    lv_obj_set_width(commission_status_label, LV_PCT(100));
    lv_label_set_long_mode(commission_status_label, LV_LABEL_LONG_WRAP);
}

static void create_detail_screen(void) {
    scr_detail = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_detail, 4, 0);
    lv_obj_set_style_pad_gap(scr_detail, 4, 0);

    create_header(scr_detail, "", btn_back_detail_cb);

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

    lv_obj_t *off_btn = lv_button_create(btn_row);
    lv_obj_t *off_lbl = lv_label_create(off_btn);
    lv_label_set_text(off_lbl, "OFF");
    lv_obj_add_event_cb(off_btn, btn_off_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *toggle_btn = lv_button_create(btn_row);
    lv_obj_t *toggle_lbl = lv_label_create(toggle_btn);
    lv_label_set_text(toggle_lbl, "TOGGLE");
    lv_obj_add_event_cb(toggle_btn, btn_toggle_cb, LV_EVENT_CLICKED, NULL);

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

    lv_obj_t *save_btn = lv_button_create(rename_row);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_add_event_cb(save_btn, btn_rename_cb, LV_EVENT_CLICKED, NULL);

    // Unpair button
    lv_obj_t *unpair_btn = lv_button_create(scr_detail);
    lv_obj_set_width(unpair_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(unpair_btn, lv_color_hex(0xC62828), 0);
    lv_obj_t *unpair_lbl = lv_label_create(unpair_btn);
    lv_label_set_text(unpair_lbl, "Unpair Device");
    lv_obj_center(unpair_lbl);
    lv_obj_add_event_cb(unpair_btn, btn_unpair_cb, LV_EVENT_CLICKED, NULL);
}

static void show_detail_for_device(uint64_t node_id) {
    const matter_device_t *dev = device_manager_find(node_id);
    if (!dev) return;

    detail_node_id = node_id;
    lv_label_set_text(detail_name_label, dev->name);
    lv_label_set_text(detail_state_label, dev->on_off ? "State: ON" : "State: OFF");

    char info[128];
    snprintf(info, sizeof(info), "Node ID: 0x%llX\nEndpoint: %u",
             (unsigned long long)dev->node_id, dev->endpoint_id);
    lv_label_set_text(detail_info_label, info);

    lv_textarea_set_text(detail_rename_ta, dev->name);

    lv_screen_load(scr_detail);
}

// ---- Public API ----
void ui_screens_init(void) {
    s_ui_event_queue = xQueueCreate(16, sizeof(matter_event_t));

    create_dashboard_screen();
    create_commission_screen();
    create_detail_screen();

    refresh_dashboard();
    lv_screen_load(scr_dashboard);

    s_event_timer = lv_timer_create(event_timer_cb, 100, NULL);
}

void ui_post_event(matter_event_t event) {
    if (s_ui_event_queue) {
        xQueueSend(s_ui_event_queue, &event, 0);
    }
}

void ui_update_device_state(uint64_t node_id, bool on_off) {
    refresh_dashboard();

    // Update detail screen if showing this device
    if (detail_node_id == node_id && lv_screen_active() == scr_detail) {
        lv_label_set_text(detail_state_label, on_off ? "State: ON" : "State: OFF");
    }
}

void ui_show_dashboard(void) {
    refresh_dashboard();
    lv_screen_load(scr_dashboard);
}
