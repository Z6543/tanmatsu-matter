# Tanmatsu Matter Commissioner

A Matter commissioner/controller GUI application for the [Tanmatsu](https://nicolaielectronics.nl/tanmatsu/) device (ESP32-P4). Commission and control Matter-compatible smart home devices directly from the Tanmatsu using its built-in display and input controls.

## Features

- Commission Matter devices via multiple methods:
  - Setup PIN Code (on-network)
  - Discriminator + Passcode
  - Manual Pairing Code
  - QR Code payload (type manually or **scan with the built-in camera**)
  - BLE + WiFi (discover over BLE, provision WiFi credentials)
  - BLE + Thread (discover over BLE, provision Thread network credentials)
- Thread Border Router using the ESP32-C6 IEEE 802.15.4 radio as an OpenThread RCP
- Dashboard with device cards showing real-time state
- Control devices by type: on/off, dimming, color temperature, hue/saturation, door locks, thermostats, window coverings, fans
- Rename and unpair commissioned devices
- Keyboard/encoder navigation with visual focus indicators
- Device state persisted in NVS across reboots

## Hardware

- **Main SoC**: ESP32-P4 (with PSRAM, 16MB flash)
- **Radio co-processor**: ESP32-C6 via ESP-Hosted (SDIO transport for WiFi/BLE, UART for Thread RCP)
- **Display**: DSI LCD panel via LVGL
- **Camera**: OV5647 MIPI CSI (2-lane, 800×640 @ 50fps) — used for QR code scanning
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
  camera_qr.c/h             - OV5647 camera init + quirc QR decode loop
  quirc/                    - Vendored quirc QR library (PSRAM-patched for ESP32)
  matter_device_control.cpp/h - Device control (on/off/toggle, subscriptions)
  device_manager.c/h        - Device persistence (NVS storage)
  matter_project_config.h   - CHIP project configuration overrides
  esp_ot_config.h           - OpenThread RCP UART pin configuration
  paa_certs_embedded.cpp/h  - Custom AttestationTrustStore backed by compiled-in DER arrays
  paa_cert_data.cpp         - Auto-generated cert data (regenerate with scripts/generate_paa_array.py)
paa_cert/                   - Source DER cert files (SDK test certs + DCL-fetched certs)
scripts/
  fetch_paa_certs.sh        - Fetch latest PAA certs from the Matter DCL into paa_cert/
  generate_paa_array.py     - Regenerate paa_cert_data.cpp from paa_cert/*.der
sdkconfigs/
  general                   - Shared sdkconfig defaults (all targets)
  tanmatsu                  - Tanmatsu-specific sdkconfig defaults
  mch2022                   - MCH2022 badge sdkconfig defaults
partitions.csv              - Partition table (OTA-capable, 6MB app partition)
```

## Prerequisites

### Python 3.11

ESP-IDF v5.5.1 requires Python 3.11. Ensure it is installed and available as `python3` before running the install scripts.

### Clone

ESP-IDF (v5.5.1) and esp-matter (with Tanmatsu patches) are included as git submodules:

```bash
git clone --recursive https://github.com/Z6543/tanmatsu-matter.git
cd tanmatsu-matter
```

If already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

### Install toolchains

Run the bootstrap script to install both the ESP-IDF toolchain and esp-matter dependencies in one step:

```bash
zsh bootstrap.sh
```

### tanmatsu-radio (Thread support)

Thread commissioning requires the ESP32-C6 co-processor to run the [tanmatsu-radio](https://github.com/nicolai-electronics/tanmatsu-radio) firmware, which provides an OpenThread RCP over UART alongside WiFi/BLE over SDIO.

Flash the radio firmware to the ESP32-C6 before using Thread features:

```bash
cd ../tanmatsu-radio
make build
make flash
```

The RCP communicates Spinel HDLC frames over UART1 at 460800 baud. The UART pins are directly wired between the P4 and C6 (P4 GPIO54 ← C6 TX, P4 GPIO53 → C6 RX), so no extra wiring is needed.

## Building

### Using build.sh (recommended)

```bash
zsh build.sh
```

This sources esp-idf, esp-matter, and pigweed exports from the submodules, then runs `idf.py -D DEVICE=tanmatsu build`.

### Using Make

The included Makefile wraps the idf.py commands:

```bash
# Build for tanmatsu (default)
make build

# Full clean and rebuild
make fullclean
make build

# Open menuconfig
make menuconfig
```

### Using idf.py directly

```bash
source esp-idf/export.sh
export ESP_MATTER_PATH="$(pwd)/esp-matter"
source $ESP_MATTER_PATH/export.sh
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
   - Choose a **transport**: Ethernet (on-network), BLE+WiFi, or BLE+Thread
   - Choose an **input mode**: QR code payload, manual pairing code, discriminator+passcode, or PIN code (PIN only for Ethernet)
   - When **QR Code** input mode is selected, press **Scan QR** to open a live camera preview. Point the camera at the device's QR code — the payload is detected automatically and fills the code field.
   - Enter a device name and press **Start Commissioning**
4. Commissioned devices appear as cards on the dashboard
   - **Enter**: Toggle the device on/off
   - **F1**: Open device detail screen with type-specific controls (dimming, color, lock, thermostat, etc.)
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
| `CONFIG_CAMERA_OV5647` | `y` | Enable OV5647 MIPI CSI camera driver |

## Device Attestation Certificates

The firmware includes 161 PAA (Product Attestation Authority) root certificates compiled directly into the binary — 158 fetched from the Matter DCL (production and test networks) and 3 from the chip SDK test suite. These are used to verify that commissioned devices are genuine certified Matter products.

The certificates are stored as DER byte arrays in `main/paa_cert_data.cpp` (auto-generated) and looked up by SKID at commissioning time. If attestation fails for a known-good device, the cert bundle may be out of date.

### Refreshing the certificate bundle

```bash
# 1. Install fetch script dependencies (once)
pip install click click-option-group requests cryptography

# 2. Fetch the latest certs from the Matter DCL
./scripts/fetch_paa_certs.sh

# 3. Regenerate the compiled-in cert array
python3 scripts/generate_paa_array.py

# 4. Rebuild and flash
make build && make flash
```

`fetch_paa_certs.sh` downloads from both the DCL MainNet and TestNet and saves the DER files as `paa_cert/dcl_NNNN.der`. Previously downloaded DCL certs are replaced; the three SDK test certs (`Chip-Test-PAA-*.der`) are preserved.

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

### Managing the Thread dataset via BadgeLink

The Thread active operational dataset is stored in NVS and can be read, written, or erased using [BadgeLink](https://docs.tanmatsu.cloud/software/badgelink/). This is useful for pre-provisioning a Thread network, sharing credentials between devices, or resetting the Thread configuration.

The dataset is stored as a binary blob in the `openthread` NVS namespace under key `OT0100` (OpenThread key format: `OT` + `01` for ActiveDataset + `00` for index 0).

```bash
# List all OpenThread NVS keys
./badgelink.sh nvs list openthread

# Read the current active dataset (binary blob)
./badgelink.sh nvs read openthread OT0100 blob

# Write a dataset from a binary file
./badgelink.sh nvs write openthread OT0100 blob dataset.bin --file

# Delete the dataset (forces regeneration from sdkconfig defaults on next boot)
./badgelink.sh nvs delete openthread OT0100
```

To transfer a dataset from another Thread border router:

1. Export the dataset as hex from the source (e.g., `ot-ctl dataset active -x`)
2. Convert to a binary file: `echo -n "<hex>" | xxd -r -p > dataset.bin`
3. Write it: `./badgelink.sh nvs write openthread OT0100 blob dataset.bin --file`
4. Reboot the device — the border router will use the stored dataset

If no dataset exists in NVS on boot, the app creates one automatically from the sdkconfig defaults (`esp_openthread_auto_start`).

## Known Limitations

- **Thread/WiFi coexistence** — enabling the Thread border router can cause WiFi to disconnect. Root cause is in the ESP32-C6 radio firmware / ESP-Hosted transport; not fixable at the application level.
- **Thread re-commissioning** — commissioning a Thread device already joined to a different Thread network/fabric (via manual pairing mode) fails for unknown reasons.
- **Device limit** — a maximum of 5 devices can be commissioned. Increasing this requires evaluating PSRAM and NVS impact.
- **Subscription loss** — if a subscribed device goes offline, it is not automatically marked unreachable and re-subscribe on reconnect is not yet implemented.
- **NVS struct layout** — device state is persisted as a raw struct blob. Adding fields to `matter_device_t` will break existing NVS data and require a manual NVS erase.

## License

The contents of this repository may be considered in the public domain or [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your disposal.

At Badge.Team we love open source so we recommend licensing your work based on this template under terms of the [MIT license](https://opensource.org/license/mit).
