# Tanmatsu Matter Commissioner

A Matter commissioner/controller GUI application for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/) device (ESP32-P4). Commission and control Matter-compatible smart home devices directly from the Tanmatsu using its built-in display and input controls.

## Features

- Commission Matter devices via multiple methods:
  - Setup PIN Code (on-network)
  - Discriminator + Passcode
  - Manual Pairing Code
  - QR Code payload
  - BLE + WiFi (discover over BLE, provision WiFi credentials)
  - BLE + Thread (discover over BLE, provision Thread network credentials)
- Thread Border Router using the ESP32-C6 IEEE 802.15.4 radio as an OpenThread RCP
- Dashboard with device cards showing on/off state
- Toggle, turn on/off, rename, and unpair commissioned devices
- Keyboard/encoder navigation with visual focus indicators
- Device state persisted in NVS across reboots

## Hardware

- **Main SoC**: ESP32-P4 (with PSRAM, 16MB flash)
- **Radio co-processor**: ESP32-C6 via ESP-Hosted (SDIO transport for WiFi/BLE, UART for Thread RCP)
- **Display**: DSI LCD panel via LVGL
- **Input**: Keypad/encoder (navigated via LVGL input groups)

WiFi and BLE are provided by the ESP32-C6 co-processor over SDIO. The WiFi stack is managed by the tanmatsu wifi-manager component, not by the CHIP/Matter stack.

The ESP32-C6 also provides an IEEE 802.15.4 radio (OpenThread RCP) over UART, enabling this device to act as a Thread Border Router. Thread and WiFi operate simultaneously with RF coexistence handled by the C6.

## Project Structure

```
main/
  main.c                    - Application entry point and init sequence
  ui_screens.c/h            - LVGL UI (dashboard, commission, detail screens)
  bsp_lvgl.c/h              - LVGL display and input driver setup
  matter_init.cpp/h         - Matter stack and commissioner initialization
  matter_commission.cpp/h   - Commissioning (PIN, QR, BLE+WiFi, BLE+Thread)
  matter_device_control.cpp/h - Device control (on/off/toggle, subscriptions)
  device_manager.c/h        - Device persistence (NVS storage)
  matter_project_config.h   - CHIP project configuration overrides
  esp_ot_config.h           - OpenThread RCP UART pin configuration
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

### tanmatsu-radio (Thread support)

Thread commissioning requires the ESP32-C6 co-processor to run the [tanmatsu-radio](https://github.com/nicolai-electronics/tanmatsu-radio) firmware, which provides an OpenThread RCP over UART alongside WiFi/BLE over SDIO.

Flash the radio firmware to the ESP32-C6 before using Thread features:

```bash
cd ../tanmatsu-radio
make build
make flash
```

The RCP communicates Spinel HDLC frames over UART1 at 460800 baud. The UART pins are directly wired between the P4 and C6 (P4 GPIO54 ← C6 TX, P4 GPIO53 → C6 RX), so no extra wiring is needed.

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
   - **PIN** — on-network commissioning with a numeric setup PIN code
   - **Disc+Pass** — discriminator + passcode with optional discovery hints
   - **Manual** — manual pairing code (11-digit numeric, auto-detects transport)
   - **QR** — QR code payload string (e.g. `MT:...`, auto-detects transport)
   - **BLE+WiFi** — discover device over BLE, provision the current WiFi network credentials
   - **BLE+Thread** — discover device over BLE, provision Thread network credentials via the border router
   - Enter a device name and press **Start Commissioning**
4. Commissioned devices appear as cards on the dashboard
   - **Enter**: Toggle the device on/off
   - **F1**: Open device detail screen (on/off controls, rename, unpair)
   - **F2**: Force remove device

## Key Configuration

Notable sdkconfig settings in `sdkconfigs/tanmatsu`:

| Setting | Value | Purpose |
|---------|-------|---------|
| `CONFIG_ENABLE_WIFI_STATION` | `n` | Prevents Matter from managing WiFi (handled by wifi-manager) |
| `CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER` | `n` | Controller-only mode (no Matter server endpoint) |
| `CONFIG_ENABLE_ESP32_BLE_CONTROLLER` | `y` | Enable BLE controller for BLE commissioning |
| `CONFIG_ESP_MATTER_COMMISSIONER_ENABLE` | `y` | Enable Matter commissioner functionality |
| `CONFIG_CHIP_TASK_STACK_SIZE` | `15360` | Increased stack for Matter task |
| `CONFIG_OPENTHREAD_ENABLED` | `y` | Enable OpenThread stack |
| `CONFIG_OPENTHREAD_BORDER_ROUTER` | `y` | Enable Thread Border Router (ESP32-C6 RCP) |
| `CONFIG_OPENTHREAD_RADIO_SPINEL_UART` | `y` | Spinel over UART to C6 RCP |
| `CONFIG_OPENTHREAD_FTD` | `y` | Full Thread Device (required for border router) |

## Thread Commissioning

The Tanmatsu acts as a Thread Border Router, bridging between the WiFi/IP network and the Thread mesh network. This allows commissioning and controlling Thread-based Matter devices.

### How it works

1. The ESP32-C6 runs as an OpenThread RCP, providing the IEEE 802.15.4 radio
2. The ESP32-P4 runs the OpenThread stack in Full Thread Device (FTD) mode with border routing enabled
3. When WiFi connects, the border router initializes with the WiFi interface as the backbone
4. Thread devices are discovered over BLE, then provisioned with Thread network credentials
5. After joining the Thread mesh, devices register via SRP and become reachable through the border router

### Commissioning a Thread device

1. Ensure the ESP32-C6 is running the [tanmatsu-radio](https://github.com/badgeteam/tanmatsu-radio) firmware
2. Ensure WiFi is connected (the border router needs a backbone interface)
3. On the commission screen, select **BLE+Thread**
4. Enter the device's passcode and discriminator
5. The Thread dataset field is pre-filled with the default network configuration — modify it if your Thread network uses different settings
6. Press **Start Commissioning**

The default Thread network uses channel 15, PAN ID 0x1234, and network name "Tanmatsu" with development keys. For production use, replace the dataset with your network's active operational dataset.

## License

The contents of this repository may be considered in the public domain or [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your disposal.

At Badge.Team we love open source so we recommend licensing your work based on this template under terms of the [MIT license](https://opensource.org/license/mit).
