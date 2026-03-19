#include "sdcard.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard";
static bool mounted = false;
static sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

static DRAM_DMA_ALIGNED_ATTR uint8_t dma_buf[512 * 4];

esp_err_t sdcard_init(void) {
    esp_err_t res;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    res = sd_pwr_ctrl_new_on_chip_ldo(
        &ldo_config, &pwr_ctrl_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SD LDO power control: %s",
                 esp_err_to_name(res));
        return res;
    }

    ESP_LOGI(TAG, "Power cycling SD card...");
    sd_pwr_ctrl_set_io_voltage(pwr_ctrl_handle, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    sd_pwr_ctrl_set_io_voltage(pwr_ctrl_handle, 3300);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "SD card power cycle complete");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = "/sd";

    ESP_LOGI(TAG, "Initializing SD card (SDMMC 4-bit mode)");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    host.dma_aligned_buffer = dma_buf;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk   = GPIO_NUM_43;
    slot_config.cmd   = GPIO_NUM_44;
    slot_config.d0    = GPIO_NUM_39;
    slot_config.d1    = GPIO_NUM_40;
    slot_config.d2    = GPIO_NUM_41;
    slot_config.d3    = GPIO_NUM_42;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    res = esp_vfs_fat_sdmmc_mount(
        mount_point, &host, &slot_config,
        &mount_config, &card);

    if (res != ESP_OK) {
        if (res == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD card filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s",
                     esp_err_to_name(res));
        }
        return res;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, card);
    mounted = true;
    return ESP_OK;
}

bool sdcard_is_mounted(void) {
    return mounted;
}
