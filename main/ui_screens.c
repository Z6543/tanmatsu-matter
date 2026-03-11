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

// Per-screen input groups
static lv_group_t *grp_dashboard = NULL;
static lv_group_t *grp_commission = NULL;
static lv_group_t *grp_detail = NULL;

// Dashboard widgets
static lv_obj_t *dashboard_container = NULL;
static lv_obj_t *dashboard_status_label = NULL;

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

// Default Thread operational dataset (hex TLV) for the Tanmatsu
// ESP32-C6 OT RCP via Spinel.
// Channel 15, PAN ID 0x1234, network "Tanmatsu", development keys.
// Replace with your OTBR's active dataset for production use.
#define DEFAULT_THREAD_DATASET \
    "0e080000000000010000" /* Active Timestamp = 1 */ \
    "000300000f"           /* Channel 15 */ \
    "3506000407fff800"     /* Channel Mask (11-26) */ \
    "0208dead00beef00cafe" /* Extended PAN ID */ \
    "0708fd000db800a00000" /* Mesh Local Prefix */ \
    "0510" "00112233445566778899aabbccddeeff" /* Network Key */ \
    "030854616e6d61747375" /* Network Name "Tanmatsu" */ \
    "01021234"             /* PAN ID 0x1234 */ \
    "0410" "3aa55f91ca47d1e4e71a08cb35e91591" /* PSKc */ \
    "0c0402a0f7f8"         /* Security Policy */

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
        case MATTER_EVENT_ATTESTATION_WARNING:
            if (commission_status_label) {
                lv_label_set_text(commission_status_label,
                    "Warning: device attestation failed.\n"
                    "Commissioning continues but device\n"
                    "may not be officially certified.");
                lv_obj_set_style_text_color(commission_status_label,
                    lv_color_hex(0xFFA000), 0);
            }
            break;
        }
    }
}

// ---- Navigation callbacks ----
static void btn_add_cb(lv_event_t *e) {
    (void)e;
    if (commission_status_label) lv_label_set_text(commission_status_label, "");
    if (commission_code_ta) lv_textarea_set_text(commission_code_ta, "");
    if (commission_disc_ta) lv_textarea_set_text(commission_disc_ta, "");
    if (commission_hints_ta) lv_textarea_set_text(commission_hints_ta, "");
    if (commission_thread_ta) lv_textarea_set_text(commission_thread_ta, DEFAULT_THREAD_DATASET);
    if (commission_name_ta) lv_textarea_set_text(commission_name_ta, "");
    switch_to_screen(scr_commission, grp_commission);
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
    update_commission_fields();
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

    if (commission_method == 5) {
        const char *thread_str = lv_textarea_get_text(commission_thread_ta);
        if (!thread_str || thread_str[0] == '\0') {
            lv_label_set_text(commission_status_label, "Enter Thread dataset!");
            return;
        }
    }

    if (commission_method == 1 || commission_method == 4 ||
        commission_method == 5) {
        const char *disc_str = lv_textarea_get_text(commission_disc_ta);
        if (!disc_str || disc_str[0] == '\0') {
            lv_label_set_text(commission_status_label, "Enter discriminator!");
            return;
        }
    }

    strncpy(s_pending_name, name_str, MATTER_DEVICE_NAME_LEN - 1);
    s_pending_name[MATTER_DEVICE_NAME_LEN - 1] = '\0';

    s_pending_node_id = device_manager_next_node_id();
    esp_err_t err = ESP_FAIL;

    lv_label_set_text(commission_status_label, "Starting...");
    lv_obj_add_state(commission_start_btn, LV_STATE_DISABLED);

    switch (commission_method) {
    case 0: {
        // Setup PIN code, on-network (no discriminator)
        uint32_t pincode = (uint32_t)atol(code_str);
        err = matter_commission_on_network(s_pending_node_id, pincode);
        break;
    }
    case 1: {
        // Discriminator + Passcode with optional discovery hints
        uint32_t pincode = (uint32_t)atol(code_str);
        const char *disc_str = lv_textarea_get_text(commission_disc_ta);
        uint16_t disc = (uint16_t)atoi(disc_str);
        const char *hints_str = lv_textarea_get_text(commission_hints_ta);
        uint8_t hints = (hints_str && hints_str[0]) ? (uint8_t)atoi(hints_str) : 0;
        err = matter_commission_disc_pass(s_pending_node_id, pincode, disc, hints);
        break;
    }
    case 2: {
        // Manual pairing code (auto-detects transport from code)
        err = matter_commission_setup_code(s_pending_node_id, code_str);
        break;
    }
    case 3: {
        // QR code payload (auto-detects transport from code)
        err = matter_commission_setup_code(s_pending_node_id, code_str);
        break;
    }
    case 4: {
        // BLE+WiFi: discriminator + passcode over BLE, WiFi creds auto-fetched
        uint32_t pincode = (uint32_t)atol(code_str);
        const char *disc_str = lv_textarea_get_text(commission_disc_ta);
        uint16_t disc = (uint16_t)atoi(disc_str);
        err = matter_commission_ble_wifi(s_pending_node_id, pincode, disc);
        break;
    }
    case 5: {
        // BLE+Thread: discover via BLE, commission with Thread dataset
        uint32_t pincode = (uint32_t)atol(code_str);
        const char *disc_str = lv_textarea_get_text(commission_disc_ta);
        uint16_t disc = (uint16_t)atoi(disc_str);
        const char *thread_str = lv_textarea_get_text(commission_thread_ta);
        err = matter_commission_ble_thread(
            s_pending_node_id, pincode, disc, thread_str);
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

static void card_key_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    uint64_t node_id = (uint64_t)(uintptr_t)lv_event_get_user_data(e);
    if (key == LV_KEY_HOME) {  // F1: details
        show_detail_for_device(node_id);
    } else if (key == LV_KEY_END) {  // F2: force remove
        device_manager_remove(node_id);
        refresh_dashboard();
    }
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
    switch_to_screen(scr_dashboard, grp_dashboard);
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

static lv_image_dsc_t img_dsc_esc;
static lv_image_dsc_t img_dsc_f1;
static lv_image_dsc_t img_dsc_f2;
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

    // Header with Add button
    lv_obj_t *header = lv_obj_create(scr_dashboard);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_HOME " Matter Commissioner");

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

    // Remove all card objects from group (keep the Add button which is first)
    while (lv_group_get_obj_count(grp_dashboard) > 1) {
        lv_obj_t *last = lv_group_get_obj_by_index(grp_dashboard, lv_group_get_obj_count(grp_dashboard) - 1);
        lv_group_remove_obj(last);
    }

    int count = device_manager_count();
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(dashboard_container);
        lv_label_set_text(lbl, "No devices.\nSelect '" LV_SYMBOL_PLUS " Add' to commission.");
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
    lv_obj_add_state(commission_method_radios[0], LV_STATE_CHECKED);
    commission_method = 0;

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
        "Thread Dataset (hex TLV):");
    lv_obj_add_flag(commission_thread_label, LV_OBJ_FLAG_HIDDEN);

    commission_thread_ta = lv_textarea_create(scr_commission);
    lv_textarea_set_one_line(commission_thread_ta, true);
    lv_textarea_set_text(commission_thread_ta, DEFAULT_THREAD_DATASET);
    lv_textarea_set_placeholder_text(commission_thread_ta,
        "Thread dataset hex TLV from OTBR");
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
    lv_label_set_text(detail_state_label, dev->on_off ? "State: ON" : "State: OFF");

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

    // Update detail screen if showing this device
    if (detail_node_id == node_id && lv_screen_active() == scr_detail) {
        lv_label_set_text(detail_state_label, on_off ? "State: ON" : "State: OFF");
    }
}

void ui_show_dashboard(void) {
    refresh_dashboard();
    switch_to_screen(scr_dashboard, grp_dashboard);
}
