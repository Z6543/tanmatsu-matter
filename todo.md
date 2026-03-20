# TODO and current known limitations

- [ ] **If a Thread device is already commissioned to another TBR on another Thread network, and we want to commission it into our Matter fabric by putting the device into manual pairing mode, commission will fail for unknown reasons
- [ ] **Add OV5647 camera support for QR code reading
- [ ] **Implement W5500 for Ethernet support
- [ ] **Warn if test device is onboarded
- [ ] **Thread and WiFi does not work well together right now, e.g. using Thread makes WiFi to disconnect.
- [ ] **For slider type GUI elements, one keypress should move slider with 10 unit, and shift+arrow should move it 1 unit
- [ ] **BLE commissioning fails due to NimBLE GATT procedure conflict** — `HandleGAPConnect` in `BLEManagerImpl.cpp` calls `ble_gattc_exchange_mtu` then immediately starts GATT discovery via `peer_disc_all`. NimBLE only allows one GATT procedure per connection, causing `BLE_HS_EBUSY` → `CHIP_ERROR_INTERNAL` (0xAC) → full BLE stack teardown. Patched locally by deferring MTU exchange to `OnGattDiscComplete`. Needs upstream fix or permanent local patch management.

## Low prio

- [ ] **Handle subscription failures** — `matter_device_subscribe_onoff` leaks the `subscribe_command` (allocated with `new` at `matter_device_control.cpp:80`) and never detects subscription loss. Track active subscriptions, handle the failure callback (4th arg is currently `nullptr`), and re-subscribe on disconnect.
- [ ] **Mark devices unreachable on subscription drop** — `reachable` is set to `true` on report/subscribe-done but never set to `false`. Add a subscription failure/termination callback that sets `reachable = false` and updates the UI.
- [ ] **Improve device card layout for many devices** — with `MATTER_DEVICE_MAX=5` and fixed 140x80 cards, the dashboard works. If the limit increases, add scrolling or a list view.
- [ ] **Increase `MATTER_DEVICE_MAX`** — the limit of 5 devices is low for a home with many smart devices. Evaluate memory impact and increase, or switch to dynamic allocation.
- [ ] **Persist `reachable` and `on_off` state correctly** — `device_manager_save` writes the full `matter_device_t` struct including runtime state (`reachable`, `on_off`) as a raw blob. This makes NVS layout fragile (adding a struct field breaks existing data). Serialize only persistent fields, or add a version tag.
- [ ] **Thread-safety for device manager** — `s_devices` and `s_device_count` are accessed from both the main task (UI) and the Matter task (callbacks) without synchronization. Add a mutex or confine all access to a single task.
- [ ] **Clean up stale NVS keys on device removal** — `device_manager_remove` compacts the array and re-saves, but if `device_manager_save` fails partway through, NVS can have inconsistent state. Write the new count last, after all blobs succeed.
