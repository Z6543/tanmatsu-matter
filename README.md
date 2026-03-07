# Tanmatsu Matter Commissioner

A Matter commissioner/controller GUI application for the [Tanmatsu](https://badge.team/) device (ESP32-P4). Commission and control Matter-compatible smart home devices directly from the Tanmatsu using its built-in display and input controls.

## Features

- Commission Matter devices via Setup PIN Code (on-network) or QR Code payload
- Dashboard with device cards showing on/off state
- Toggle, turn on/off, rename, and unpair commissioned devices
- Keyboard/encoder navigation with visual focus indicators
- Device state persisted in NVS across reboots

## Hardware

- **Main SoC**: ESP32-P4 (with PSRAM, 16MB flash)
- **Radio co-processor**: ESP32-C6 via ESP-Hosted (SDIO transport)
- **Display**: DSI LCD panel via LVGL
- **Input**: Keypad/encoder (navigated via LVGL input groups)

WiFi and BLE are provided by the ESP32-C6 co-processor. The WiFi stack is managed by the tanmatsu wifi-manager component, not by the CHIP/Matter stack.

## Project Structure

```
main/
  main.c                    - Application entry point and init sequence
  ui_screens.c/h            - LVGL UI (dashboard, commission, detail screens)
  bsp_lvgl.c/h              - LVGL display and input driver setup
  matter_init.cpp/h         - Matter stack and commissioner initialization
  matter_commission.cpp/h   - Commissioning functions (PIN code, QR code)
  matter_device_control.cpp/h - Device control (on/off/toggle, subscriptions)
  device_manager.c/h        - Device persistence (NVS storage)
  matter_project_config.h   - CHIP project configuration overrides
sdkconfigs/
  general                   - Shared sdkconfig defaults (all targets)
  tanmatsu                  - Tanmatsu-specific sdkconfig defaults
  mch2022                   - MCH2022 badge sdkconfig defaults
partitions.csv              - Partition table (OTA-capable, 6MB app partition)
```

## Prerequisites

### ESP-IDF (v5.5)

Clone and install ESP-IDF:

```bash
git clone --recursive --branch v5.5 https://github.com/espressif/esp-idf.git --depth=1 --shallow-submodules
cd esp-idf
git submodule update --init --recursive
./install.sh all
cd ..
```

### esp-matter

Clone and set up the esp-matter SDK. It must be located alongside this project (or wherever you point `ESP_MATTER_PATH`):

```bash
git clone --recursive https://github.com/nicola/esp-matter.git
```

Follow the [esp-matter setup guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) to ensure all dependencies are installed.

## Environment Setup

Set the required environment variables and source the ESP-IDF export script. You can either use the `env` helper script or set them manually:

### Option A: Using the `env` script

If you have ESP-IDF and tools installed inside the project directory:

```bash
source env
```

### Option B: Manual setup

```bash
export IDF_PATH=/path/to/esp-idf
export IDF_TOOLS_PATH=/path/to/esp-idf-tools
export ESP_MATTER_PATH=/path/to/esp-matter

source $IDF_PATH/export.sh
```

## Building

### Using Make (recommended)

The included Makefile wraps the idf.py commands and handles environment sourcing:

```bash
# Build for tanmatsu (default)
make build

# Build for a specific device
make build DEVICE=tanmatsu

# Full clean and rebuild
make fullclean
make build

# Open menuconfig
make menuconfig
```

### Using idf.py directly

After sourcing the environment:

```bash
idf.py -D DEVICE=tanmatsu build
```

The `DEVICE` parameter selects the target board and its sdkconfig defaults. Supported values: `tanmatsu`, `mch2022`.

## Flashing

```bash
# Flash using default port (/dev/ttyACM0)
make flash

# Flash to a specific port
make flash PORT=/dev/ttyUSB0

# Or using idf.py directly
idf.py -p /dev/ttyACM0 flash
```

## Monitoring

```bash
idf.py -p /dev/ttyACM0 monitor
```

UART console output is enabled by default (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`).

## Usage

1. The app connects to WiFi automatically on boot using stored credentials (managed by the tanmatsu wifi-manager)
2. The Matter commissioner initializes and the dashboard shows "Commissioner ready"
3. Press **+ Add** to commission a new device:
   - Select **Setup PIN Code** for on-network commissioning with a numeric PIN
   - Select **QR Code** for commissioning via a Matter QR code payload string (e.g. `MT:...`)
   - Enter a device name and press **Start Commissioning**
4. Commissioned devices appear as cards on the dashboard
   - **Short press / Enter**: Toggle the device on/off
   - **Long press**: Open the device detail screen (on/off controls, rename, unpair)

## Key Configuration

Notable sdkconfig settings in `sdkconfigs/tanmatsu`:

| Setting | Value | Purpose |
|---------|-------|---------|
| `CONFIG_ENABLE_WIFI_STATION` | `n` | Prevents Matter from managing WiFi (handled by wifi-manager) |
| `CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER` | `n` | Controller-only mode (no Matter server endpoint) |
| `CONFIG_ENABLE_ESP32_BLE_CONTROLLER` | `n` | BLE controller disabled (co-processor BLE not supported by Matter SDK) |
| `CONFIG_ESP_MATTER_COMMISSIONER_ENABLE` | `y` | Enable Matter commissioner functionality |
| `CONFIG_CHIP_TASK_STACK_SIZE` | `15360` | Increased stack for Matter task |

## License

The contents of this repository may be considered in the public domain or [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your disposal.

At Badge.Team we love open source so we recommend licensing your work based on this template under terms of the [MIT license](https://opensource.org/license/mit).
