# TODO

## Security

- [ ] **Include the correct certificates to validate the device.

## Reliability

- [x] **Re-subscribe after WiFi reconnect** — registers `IP_EVENT_STA_GOT_IP` handler in `main.c` that calls `matter_device_subscribe_all()` on reconnect.
- [x] **Validate commissioning inputs** — PIN code validated for range (1–99999998), digits-only, and Matter-spec invalid codes (all-same-digit, 12345678, 87654321). Discriminator validated for 0–4095 and digits-only.
- [x] **Handle concurrent commissioning and device control** — `s_commissioning_active` flag in `ui_screens.c` blocks device toggle/on/off/card clicks while commissioning is in progress.

## Device support

- [ ] **Support multiple endpoints** — `device_manager_add` hardcodes endpoint 1 (`ui_screens.c:127`). After commissioning, read the device's descriptor cluster to discover actual endpoints and device types.
- [ ] **Read device type during commissioning** — after successful commissioning, read the Basic Information cluster (vendor name, product name) and use it as the default device name instead of "Device N".

## Border router

- [ ] **Handle border router init failure** — `esp_openthread_border_router_init()` return value is not checked (`matter_init.cpp:67`). Log and surface errors to the UI if the RCP is unresponsive or UART fails.

## UI / UX

- [x] **Add screenshot capability, use one of the Function keys** — F3 takes a screenshot, outputs BMP as base64 over serial console with `===SCREENSHOT_START===`/`===SCREENSHOT_END===` markers.
- [x] **Add confirmation dialog for destructive actions** — "Force Remove" (`card_key_cb` on F2) and "Unpair Device" immediately delete without confirmation. Add a confirmation prompt.
- [x] **Show commissioning progress steps** — the status label only shows "PASE established..." and then success/fail. Show intermediate steps (attestation, network setup, operational discovery) to help diagnose failures.
- [x] **Persist and restore last commissioning method** — switching to the commission screen always resets to method 0 (PIN). Remember the user's last-used method.
- [x] **Show reachable/unreachable state on dashboard cards** — the `reachable` field exists in `matter_device_t` but the dashboard only shows on/off color. Dim or badge unreachable devices.
- [x] **When a commission method is activated in the menu, the focus of the menu item should go to the first text entry item.

## Low prio

- [ ] **Handle subscription failures** — `matter_device_subscribe_onoff` leaks the `subscribe_command` (allocated with `new` at `matter_device_control.cpp:80`) and never detects subscription loss. Track active subscriptions, handle the failure callback (4th arg is currently `nullptr`), and re-subscribe on disconnect.
- [ ] **Mark devices unreachable on subscription drop** — `reachable` is set to `true` on report/subscribe-done but never set to `false`. Add a subscription failure/termination callback that sets `reachable = false` and updates the UI.
- [ ] **Support device types beyond on/off** — currently only subscribes to OnOff cluster (0x0006). Add support for Level Control (dimming), Color Control, Temperature Measurement, and other common clusters.
- [ ] **Show Thread border router status in UI** — display whether the border router is initialized, the Thread network name, and the number of connected Thread devices.
- [ ] **Improve device card layout for many devices** — with `MATTER_DEVICE_MAX=5` and fixed 140x80 cards, the dashboard works. If the limit increases, add scrolling or a list view.
- [ ] **Increase `MATTER_DEVICE_MAX`** — the limit of 5 devices is low for a home with many smart devices. Evaluate memory impact and increase, or switch to dynamic allocation.
- [ ] **Persist `reachable` and `on_off` state correctly** — `device_manager_save` writes the full `matter_device_t` struct including runtime state (`reachable`, `on_off`) as a raw blob. This makes NVS layout fragile (adding a struct field breaks existing data). Serialize only persistent fields, or add a version tag.
- [ ] **Thread-safety for device manager** — `s_devices` and `s_device_count` are accessed from both the main task (UI) and the Matter task (callbacks) without synchronization. Add a mutex or confine all access to a single task.
- [ ] **Clean up stale NVS keys on device removal** — `device_manager_remove` compacts the array and re-saves, but if `device_manager_save` fails partway through, NVS can have inconsistent state. Write the new count last, after all blobs succeed.
