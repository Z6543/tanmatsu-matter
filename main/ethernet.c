#include "ethernet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_gpio.h"

static const char TAG[] = "ethernet";

/* J4 PMOD pin mapping (from Tanmatsu hardware netlist) */
#define W5500_SPI_HOST  SPI2_HOST
#define W5500_PIN_SCK   2   /* J4 pin 9  – MTCK  */
#define W5500_PIN_MOSI  3   /* J4 pin 10 – MTDI  */
#define W5500_PIN_MISO  5   /* J4 pin 8  – MTDO  */
#define W5500_PIN_CS    4   /* J4 pin 7  – MTMS  */
#define W5500_PIN_INT   15  /* J4 pin 5  – SAO_IO1 */
#define W5500_PIN_RST   34  /* J4 pin 6  – SAO_IO2 */

static esp_eth_handle_t s_eth_handle = NULL;
static bool             s_connected  = false;

static void on_eth_event(
    void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    esp_eth_handle_t handle = *(esp_eth_handle_t *)data;
    uint8_t mac[6];

    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
            ESP_LOGI(TAG, "Ethernet link up  "
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet link down");
            s_connected = false;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            s_connected = false;
            break;
        default:
            break;
    }
}

static void on_got_ip(
    void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t  *event = data;
    const esp_netif_ip_info_t *ip   = &event->ip_info;
    ESP_LOGI(TAG, "Ethernet got IP " IPSTR, IP2STR(&ip->ip));
    s_connected = true;
}

esp_err_t ethernet_init(void) {
    ESP_LOGI(TAG, "Pin config: SCK=%d MOSI=%d MISO=%d CS=%d INT=%d RST=%d",
             W5500_PIN_SCK, W5500_PIN_MOSI, W5500_PIN_MISO,
             W5500_PIN_CS, W5500_PIN_INT, W5500_PIN_RST);

    /* GPIO2-5 are JTAG pins on ESP32-P4 (MTCK/MTDI/MTDO/MTMS).
       gpio_reset_pin() fully disconnects from JTAG and resets
       pull-up/down to defaults, which is stronger than
       esp_rom_gpio_pad_select_gpio(). */
    ESP_LOGI(TAG, "Releasing JTAG pins (GPIO 2-5) for SPI use");
    gpio_reset_pin(W5500_PIN_SCK);
    gpio_reset_pin(W5500_PIN_MOSI);
    gpio_reset_pin(W5500_PIN_MISO);
    gpio_reset_pin(W5500_PIN_CS);
    gpio_reset_pin(W5500_PIN_INT);
    gpio_reset_pin(W5500_PIN_RST);

    /* Hardware reset: pulse RST low so the W5500 is in a known state
       before the MAC driver tries to read the version register. */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << W5500_PIN_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(W5500_PIN_RST, 0);
    ESP_LOGI(TAG, "W5500 hardware reset: RST low");
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(W5500_PIN_RST, 1);
    ESP_LOGI(TAG, "W5500 hardware reset: RST high, waiting 500ms");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* SPI bus */
    spi_bus_config_t spi_cfg = {
        .mosi_io_num   = W5500_PIN_MOSI,
        .miso_io_num   = W5500_PIN_MISO,
        .sclk_io_num   = W5500_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_LOGI(TAG, "Initializing SPI%d bus", W5500_SPI_HOST + 1);
    esp_err_t ret = spi_bus_initialize(
        W5500_SPI_HOST, &spi_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Quick SPI probe: add a device, do a dummy full-duplex transaction,
       then remove it. If MISO is floating (nothing connected), the read
       bytes will typically be 0xFF. */
    ESP_LOGI(TAG, "SPI bus initialized, probing W5500...");
    spi_device_handle_t probe_dev = NULL;
    spi_device_interface_config_t probe_cfg = {
        .mode           = 0,
        .clock_speed_hz = 1 * 1000 * 1000,  /* 1 MHz for probe */
        .queue_size     = 1,
        .spics_io_num   = W5500_PIN_CS,
    };
    ret = spi_bus_add_device(W5500_SPI_HOST, &probe_cfg, &probe_dev);
    if (ret == ESP_OK) {
        /* Read W5500 version register (VERSIONR) at address 0x0039,
           common register block = control byte 0x00 (BSB=000, R/W=0,
           OM=00). W5500 SPI frame: 2-byte addr + 1-byte control + data.
           Expected reply: 0x04 for W5500. */
        uint8_t tx[4] = {0x00, 0x39, 0x00, 0x00};
        uint8_t rx[4] = {0};
        spi_transaction_t t = {
            .length    = 32,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        ret = spi_device_transmit(probe_dev, &t);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SPI probe rx: %02x %02x %02x %02x "
                     "(expect xx xx xx 04 for W5500)",
                     rx[0], rx[1], rx[2], rx[3]);
            if (rx[3] == 0x04) {
                ESP_LOGI(TAG, "W5500 detected (version 0x04)");
            } else if (rx[3] == 0xFF) {
                ESP_LOGW(TAG, "MISO reads 0xFF — W5500 may not be "
                         "connected or pins are wrong");
            } else {
                ESP_LOGW(TAG, "Unexpected version byte 0x%02x", rx[3]);
            }
        } else {
            ESP_LOGW(TAG, "SPI probe transaction failed: %s",
                     esp_err_to_name(ret));
        }
        spi_bus_remove_device(probe_dev);
    } else {
        ESP_LOGW(TAG, "SPI probe add_device failed: %s",
                 esp_err_to_name(ret));
    }

    /* W5500 SPI device */
    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,
        .clock_speed_hz = 12 * 1000 * 1000,  /* 12 MHz */
        .queue_size     = 20,
        .spics_io_num   = W5500_PIN_CS,
    };

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(
        W5500_SPI_HOST, &dev_cfg);
    w5500_cfg.int_gpio_num = W5500_PIN_INT;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num   = W5500_PIN_RST;
    phy_cfg.phy_addr         = 1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "W5500 MAC/PHY creation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Installing Ethernet driver...");
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_cfg, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s",
                 esp_err_to_name(ret));
        mac->del(mac);
        phy->del(phy);
        return ret;
    }

    /* Generate a locally-administered MAC from the chip's base MAC */
    uint8_t base_mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(base_mac));
    uint8_t eth_mac[6];
    esp_derive_local_mac(eth_mac, base_mac);
    ESP_ERROR_CHECK(
        esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    /* Network interface */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue =
        esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    /* Event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL));

    /* Start */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "W5500 Ethernet initialized on SPI2 "
             "(SCK=%d MOSI=%d MISO=%d CS=%d INT=%d RST=%d)",
             W5500_PIN_SCK, W5500_PIN_MOSI, W5500_PIN_MISO,
             W5500_PIN_CS, W5500_PIN_INT, W5500_PIN_RST);
    return ESP_OK;
}

bool ethernet_connected(void) {
    return s_connected;
}
