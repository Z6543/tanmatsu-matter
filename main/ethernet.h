#pragma once

#include "esp_err.h"
#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize W5500 Ethernet over SPI on the J4 PMOD connector.
 *
 * Wiring (W5500 breakout → Tanmatsu J4):
 *   SCK  → Pin 9  (MTCK  / GPIO2)
 *   MOSI → Pin 10 (MTDI  / GPIO3)
 *   MISO → Pin 8  (MTDO  / GPIO5)
 *   CS   → Pin 7  (MTMS  / GPIO4)
 *   INT  → Pin 5  (SAO_IO1 / GPIO15)
 *   RST  → Pin 6  (SAO_IO2 / GPIO34)
 *   3.3V → Pin 1 or 14
 *   GND  → Pin 2, 11, or 12
 *
 * Returns ESP_OK on success. Non-fatal: caller may continue without
 * Ethernet if the W5500 is not connected.
 */
esp_err_t ethernet_init(void);

/** Return true if Ethernet link is up and has an IP address. */
bool ethernet_connected(void);

#ifdef __cplusplus
}
#endif
