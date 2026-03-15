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

static const char *TAG = "matter_ctrl";

static const uint32_t ONOFF_CLUSTER_ID = 0x0006;
static const uint32_t ONOFF_ATTR_ID    = 0x0000;
static const uint32_t CMD_OFF    = 0;
static const uint32_t CMD_ON     = 1;
static const uint32_t CMD_TOGGLE = 2;

// Descriptor cluster (endpoint 0)
static const uint32_t DESCRIPTOR_CLUSTER_ID = 0x001D;
static const uint32_t DESCRIPTOR_PARTS_LIST_ATTR_ID = 0x0003;

// Basic Information cluster (endpoint 0)
static const uint32_t BASIC_INFO_CLUSTER_ID = 0x0028;
static const uint32_t BASIC_INFO_VENDOR_NAME_ATTR_ID = 0x0001;
static const uint32_t BASIC_INFO_PRODUCT_NAME_ATTR_ID = 0x0003;
static const uint32_t BASIC_INFO_NODE_LABEL_ATTR_ID = 0x0005;

// State for the post-commissioning device info read
static struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    char     vendor_name[32];
    char     product_name[32];
    char     node_label[32];
    bool     active;
    uint8_t  reads_pending;
} s_info_read;

static matter_device_state_cb_t s_state_cb = nullptr;

static void on_attribute_report(uint64_t remote_node_id,
                                const chip::app::ConcreteDataAttributePath &path,
                                chip::TLV::TLVReader *data) {
    if (path.mClusterId == ONOFF_CLUSTER_ID && path.mAttributeId == ONOFF_ATTR_ID && data) {
        bool on_off = false;
        if (data->GetType() == chip::TLV::kTLVType_Boolean) {
            data->Get(on_off);
        }
        ESP_LOGI(TAG, "Node 0x%llx ep%u OnOff=%d",
                 (unsigned long long)remote_node_id, path.mEndpointId, on_off);

        matter_device_t *dev = device_manager_find_mut(remote_node_id);
        if (dev) {
            dev->on_off = on_off;
            dev->reachable = true;
        }
        if (s_state_cb) {
            s_state_cb(remote_node_id, on_off);
        }
    }
}

static void on_subscribe_done(uint64_t remote_node_id, uint32_t subscription_id) {
    ESP_LOGI(TAG, "Subscription established for node 0x%llx, sub_id=%lu",
             (unsigned long long)remote_node_id, (unsigned long)subscription_id);
    matter_device_t *dev = device_manager_find_mut(remote_node_id);
    if (dev) {
        dev->reachable = true;
        if (s_state_cb) {
            s_state_cb(remote_node_id, dev->on_off);
        }
    }
}

void matter_device_set_state_cb(matter_device_state_cb_t cb) {
    s_state_cb = cb;
}

static esp_err_t send_onoff_cmd(uint64_t node_id, uint16_t endpoint_id, uint32_t cmd) {
    ESP_LOGI(TAG, "Sending OnOff cmd=%lu to node 0x%llx ep%u",
             (unsigned long)cmd, (unsigned long long)node_id, endpoint_id);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return esp_matter::controller::send_invoke_cluster_command(
        node_id, endpoint_id, ONOFF_CLUSTER_ID, cmd, nullptr);
}

esp_err_t matter_device_send_on(uint64_t node_id, uint16_t endpoint_id) {
    return send_onoff_cmd(node_id, endpoint_id, CMD_ON);
}

esp_err_t matter_device_send_off(uint64_t node_id, uint16_t endpoint_id) {
    return send_onoff_cmd(node_id, endpoint_id, CMD_OFF);
}

esp_err_t matter_device_send_toggle(uint64_t node_id, uint16_t endpoint_id) {
    return send_onoff_cmd(node_id, endpoint_id, CMD_TOGGLE);
}

esp_err_t matter_device_subscribe_onoff(uint64_t node_id, uint16_t endpoint_id) {
    ESP_LOGI(TAG, "Subscribing to OnOff for node 0x%llx ep%u",
             (unsigned long long)node_id, endpoint_id);

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    auto *sub = new esp_matter::controller::subscribe_command(
        node_id, endpoint_id, ONOFF_CLUSTER_ID, ONOFF_ATTR_ID,
        esp_matter::controller::SUBSCRIBE_ATTRIBUTE,
        1, 10,
        true,
        on_attribute_report,
        nullptr,
        on_subscribe_done,
        nullptr,
        true);

    return sub->send_command();
}

esp_err_t matter_device_subscribe_all(void) {
    int count = device_manager_count();
    for (int i = 0; i < count; i++) {
        const matter_device_t *dev = device_manager_get(i);
        if (dev) {
            matter_device_subscribe_onoff(dev->node_id, dev->endpoint_id);
        }
    }
    return ESP_OK;
}

// ---- Post-commissioning device info read ----

static void info_read_complete(void) {
    matter_event_t ev = {};
    ev.type = MATTER_EVENT_DEVICE_INFO_READY;
    ev.node_id = s_info_read.node_id;
    ev.endpoint_id = s_info_read.endpoint_id;

    // Build name: prefer NodeLabel, fall back to "VendorName ProductName"
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
    // If all empty, msg stays "" and device_manager_add
    // will generate "Device N"

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
    if (path.mClusterId != DESCRIPTOR_CLUSTER_ID ||
        path.mAttributeId != DESCRIPTOR_PARTS_LIST_ATTR_ID ||
        !data) {
        return;
    }

    // PartsList is an array of endpoint IDs
    chip::TLV::TLVType container;
    if (data->GetType() != chip::TLV::kTLVType_Array) return;
    if (data->EnterContainer(container) != CHIP_NO_ERROR) return;

    uint16_t first_ep = 1;  // fallback
    bool found = false;
    while (data->Next() == CHIP_NO_ERROR) {
        uint16_t ep = 0;
        if (data->Get(ep) == CHIP_NO_ERROR && ep != 0) {
            if (!found) {
                first_ep = ep;
                found = true;
                ESP_LOGI(TAG, "Node 0x%llx: first app endpoint=%u",
                         (unsigned long long)remote_node_id, ep);
            }
        }
    }
    data->ExitContainer(container);

    s_info_read.endpoint_id = first_ep;
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
    if (path.mClusterId != BASIC_INFO_CLUSTER_ID || !data) return;

    if (path.mAttributeId == BASIC_INFO_VENDOR_NAME_ATTR_ID) {
        copy_tlv_string(data, s_info_read.vendor_name,
                        sizeof(s_info_read.vendor_name));
        ESP_LOGI(TAG, "VendorName: %s", s_info_read.vendor_name);
    } else if (path.mAttributeId == BASIC_INFO_PRODUCT_NAME_ATTR_ID) {
        copy_tlv_string(data, s_info_read.product_name,
                        sizeof(s_info_read.product_name));
        ESP_LOGI(TAG, "ProductName: %s", s_info_read.product_name);
    } else if (path.mAttributeId == BASIC_INFO_NODE_LABEL_ATTR_ID) {
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
    s_info_read.reads_pending = 2;

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

    // Read 1: Descriptor PartsList from endpoint 0
    auto *desc_read = new esp_matter::controller::read_command(
        node_id, 0, DESCRIPTOR_CLUSTER_ID,
        DESCRIPTOR_PARTS_LIST_ATTR_ID,
        esp_matter::controller::READ_ATTRIBUTE,
        on_descriptor_report, on_descriptor_done, nullptr);
    esp_err_t err = desc_read->send_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send descriptor read");
        s_info_read.active = false;
        return err;
    }

    // Read 2: Basic Information (VendorName, ProductName, NodeLabel)
    // from endpoint 0 using wildcard attribute ID
    auto *info_read = new esp_matter::controller::read_command(
        node_id, 0, BASIC_INFO_CLUSTER_ID,
        0xFFFFFFFF,  // wildcard: read all attributes
        esp_matter::controller::READ_ATTRIBUTE,
        on_basic_info_report, on_basic_info_done, nullptr);
    err = info_read->send_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send basic info read");
        // First read is already in flight, let it finish
        s_info_read.reads_pending = 1;
    }

    return ESP_OK;
}
