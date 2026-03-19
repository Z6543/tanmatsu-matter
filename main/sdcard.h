#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Initialize SD card power and mount filesystem at /sd
esp_err_t sdcard_init(void);

// Check if SD card is mounted
bool sdcard_is_mounted(void);
