#include "matter_device_control.h"
#include "device_manager.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_subscribe_command.h>

static const char *TAG = "matter_ctrl";

static const uint32_t ONOFF_CLUSTER_ID = 0x0006;
static const uint32_t ONOFF_ATTR_ID    = 0x0000;
static const uint32_t CMD_OFF    = 0;
static const uint32_t CMD_ON     = 1;
static const uint32_t CMD_TOGGLE = 2;

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
