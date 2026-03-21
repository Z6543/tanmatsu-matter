# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Matter commissioner/controller GUI for the Tanmatsu device (ESP32-P4). Commissions and controls Matter smart home devices via a touchscreen/keypad UI. Controller-only mode — no Matter server endpoint.

## Dependencies

esp-idf (v5.5.1) and esp-matter (Z6543 fork with Tanmatsu patches) are included as git submodules. Clone with:

```bash
git clone --recursive https://github.com/Z6543/tanmatsu-matter.git
```

If already cloned without `--recursive`:
```bash
git submodule update --init --recursive
```

## Build & Flash

The build requires ESP-IDF v5.5, esp-matter, and Pigweed (sourced transitively). The fastest way to build:

```bash
zsh build.sh
```

This sources esp-idf, esp-matter, and pigweed exports from the repo's submodules, then runs `idf.py -D DEVICE=tanmatsu build`. Alternatively:

```bash
source esp-idf/export.sh
export ESP_MATTER_PATH="$(pwd)/esp-matter"
source $ESP_MATTER_PATH/export.sh
idf.py -D DEVICE=tanmatsu build
```

Other useful commands:
- `make flash PORT=/dev/ttyACM0` — flash to device
- `idf.py -p /dev/ttyACM0 monitor` — serial monitor
- `make menuconfig DEVICE=tanmatsu` — open sdkconfig editor
- `make format` — clang-format all files in `main/`
- `make fullclean` — remove build dir + sdkconfig (needed after sdkconfig changes)

There are no unit tests — validation is done by flashing and testing on hardware.

## Architecture

### Hardware topology

ESP32-P4 host with two ESP32-C6 co-processors connected via ESP-Hosted:
- **WiFi + BLE**: C6 over SDIO — provides `esp_wifi_*` APIs remotely via ESP-Hosted. WiFi is managed by the tanmatsu wifi-manager component, NOT by the CHIP/Matter stack.
- **Thread RCP**: Same C6 over UART (Spinel HDLC at 460800 baud, P4 GPIO54/53) — OpenThread Border Router with the P4 running the full Thread stack.

### Key sdkconfig implications

- `CONFIG_ENABLE_WIFI_STATION=n` — WiFi is remote via ESP-Hosted, not managed by CHIP. This means `CHIP_DEVICE_CONFIG_ENABLE_WIFI=0`, which affects code guarded by that macro in the CHIP SDK (see DNS-SD fix below).
- `CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER=n` — controller-only, no server endpoints.
- `CONFIG_ENABLE_ESP32_BLE_CONTROLLER=y` — BLE central role for commissioning.
- `CONFIG_USE_MINIMAL_MDNS=n` — uses platform (ESP) mDNS, not CHIP's minimal mDNS.

### Application layers

```
main.c                      App entry: NVS, BSP, LVGL, WiFi, Matter init
    |
    +-- ui_screens.c/h      LVGL UI: dashboard cards, commission form, detail screen
    |                        (pure C, keyboard/encoder nav via LVGL groups)
    |
    +-- matter_init.cpp      Matter stack init, Thread border router lifecycle
    |
    +-- matter_commission.cpp  All commissioning methods (on-network, BLE+WiFi, BLE+Thread)
    |
    +-- matter_device_control.cpp  Cluster commands (on/off, level, color, lock, etc.)
    |                               and attribute subscriptions
    |
    +-- device_manager.c     Device list with NVS persistence, max 5 devices
```

### C/C++ boundary

UI and device management are pure C. Matter code is C++ (required by the CHIP SDK). All headers use `extern "C"` guards. Callbacks cross the boundary via function pointers (`matter_event_cb_t`, `matter_device_state_cb_t`).

### Multi-device-type support

`device_category_t` maps Matter device type IDs to UI categories (17 types: lights, plugs, thermostats, locks, sensors, fans, etc.). `matter_device_t` contains fields for all types. The detail screen rebuilds dynamically per category. Subscriptions are type-aware — each category subscribes to its relevant clusters.

### Known SDK patches

The esp-matter submodule (`esp-matter/`) is a fork with patches committed directly to the `tanmatsu` branch. The connectedhomeip submodule inside it also points to a Z6543 fork with patches:

1. **DnssdImpl.cpp** (`connectedhomeip/src/platform/ESP32/DnssdImpl.cpp`): ESP mDNS calls were guarded by `#if CHIP_DEVICE_CONFIG_ENABLE_WIFI`, which is 0 because WiFi is via ESP-Hosted. Patched to always call `EspDnssd*` functions since they ARE compiled in via `ESP32DnssdImpl.cpp`.

2. **BLEManagerImpl.cpp** (`connectedhomeip/src/platform/ESP32/nimble/BLEManagerImpl.cpp`): `HandleGAPConnect` was calling `ble_gattc_exchange_mtu` then immediately starting GATT discovery via `peer_disc_all`. NimBLE only allows one GATT procedure per connection at a time, causing `BLE_HS_EBUSY` → `CHIP_ERROR_INTERNAL` (0xAC) → BLE stack teardown. Patched to defer MTU exchange to `OnGattDiscComplete`.

## Code Style

- Formatting: `.clang-format` (Google-based, 120 col limit, 4-space indent)
- Run `make format` before committing
- Logging: `ESP_LOGI/W/E(TAG, ...)` with a static `TAG` per file
- Error handling: `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` macros
- CHIP errors: `CHIP_ERROR` type, check with `== CHIP_NO_ERROR`, log with `ChipLogError`

## PAA Certificates

161 PAA root certs compiled into the binary for device attestation. To refresh:

```bash
./scripts/fetch_paa_certs.sh          # fetch from Matter DCL
python3 scripts/generate_paa_array.py  # regenerate paa_cert_data.cpp
```

## GitHub

Use GitHub REST API (`gh api`) for all GitHub operations. The GraphQL API is not accessible with the configured token.
