# AnyMouse ESP32-P4 Dongle

USB man-in-the-middle firmware for the Waveshare ESP32-P4-Module-DEV-KIT. The mouse plugs into the dongle's USB-A port; the dongle applies the 8-octant vectoring and appears to the PC as a normal USB mouse. It is configured over a vendor HID channel on the same cable, from a web page (WebHID, nothing to install) or from `./anymouse` (which also does live FpsAimForge calibration). No Wi-Fi, no network.

**Status (2026-10-26):** the control plane works end to end (56 checks). The mouse side **works with a real mouse** (Razer Basilisk Ultimate): report-descriptor parser, High-Speed USB host, per-report scaling and the 1 kHz PC-side output are in the firmware (0.5.0) and measured at about 1000 reports/s in and out with no errors. The firmware also reports every attached device in three tiers (real CPI and polling for known mice, generic info for any HID mouse, boot fallback); see [PLAN.md](PLAN.md) section 9.5 and [../../PROGRESS.md](../../PROGRESS.md). Board verified (ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM). One USB-C is the CH343 UART bridge (flashing/console); the other is the P4's USB Serial/JTAG by default, but the firmware moves the Full-Speed OTG controller onto it in software (proved by [`spikes/m0_phy_swap/`](spikes/m0_phy_swap/)), so it enumerates as a HID mouse plus a vendor HID channel. Over that channel the firmware stores octant weights in flash (4 profile slots, auto-save, CRC), accepts updates from the host, and pushes changes back as events; the control-plane test passes with no mouse attached. See [PLAN.md](PLAN.md) for the milestone table.

**Update (2026-10-26, firmware 0.6.0):** full OpenMouse coverage has started ([PLAN.md section 10](PLAN.md#10-full-openmouse-coverage-on-the-dongle), status in 10.6). The dongle now carries a catalog of 810 OpenMouse device ids, claims a driver's vendor interface (not just the mouse's), and has a read-only Logitech HID++ driver, verified on a G Pro behind its Lightspeed receiver (name, DPI, polling rate, battery, asleep or awake). New protocol commands serve model names and a brand-neutral settings list; they are built and flashed but not yet exercised from a host, and the Studio UI for them is not written. The OTA slots are 4 MB each.

**Earlier plan:** once a real mouse is available, the plan is automatic discovery of every attached mouse, immediate vectoring, real CPI and polling for known mice and a generic fallback for the rest. The design and the acceptance checklist are in [PLAN.md section 9](PLAN.md#9-mouse-discovery-device-info-and-hardware-verification).

**Cabling:** the UART USB-C to the Mac (flashing/console), the "other" USB-C to the PC under test, and the mouse (or its receiver) in the **lower USB-A socket next to the Ethernet jack** with **both jumper caps on HOST** (that is the direct port). With the caps on DEVICE the P4 talks to the onboard hub and the other three sockets instead. USB-A VBUS is always on, so a mouse powers up in any socket; only the data pair is switched.

## Documents
| File | Purpose |
|:---|:---|
| [PLAN.md](PLAN.md) | Design, verified hardware, milestones M0 to M6, risks, next actions |
| [spikes/m0_phy_swap/README.md](spikes/m0_phy_swap/README.md) | The working M0 proof of concept (build and check steps) |
| [../../README.md](../../README.md#13-esp32-p4-hardware-dongle-phase-4) | Project-level overview of the dongle |
| [../../common/amv_dongle_proto.h](../../common/amv_dongle_proto.h) | The vendor-HID wire protocol (reference), shared with the host tools |
| [../../common/amv_octant.h](../../common/amv_octant.h) | The octant scaling math shared by firmware and tests |
| [../../PROGRESS.md](../../PROGRESS.md) | Dated log and roadmap checklist |

## Quickstart
Needs ESP-IDF v5.5.4 (`~/esp/esp-idf`, see [PLAN.md](PLAN.md) section 3), `cmake` and `ninja`.
```bash
. ~/esp/esp-idf/export.sh
cd firmware/esp32p4-dongle
idf.py set-target esp32p4        # first time only
idf.py build
idf.py -p <UART USB-C port> flash monitor     # macOS: /dev/cu.usbmodem<serial>; Linux: /dev/ttyACM0 or /dev/ttyUSB0
```
**Always flash and monitor through the UART USB-C.** While the firmware runs, the other USB-C is the PC link and cannot flash. Download mode is not fused off (hold BOOT, tap RST), so a bad image can always be replaced.

## Components
| Component | Role |
|:---|:---|
| `amv_common` | Header-only link to the repo-level `common/` (scaling math, wire protocol) |
| `amv_store`, `amv_ctrl` | Flash profiles (CRC, 4 slots) and the state owner (weights, generation counter, lock-free hot-path read, mouse count) |
| `amv_proto` | Vendor-HID command handler (`HELLO`, weights, slots, bypass, stats, debug commands) |
| `amv_usb_device` | PC side: Full-Speed device on the non-UART USB-C (PHY swap); 16-button / 16-bit mouse report (3-byte boot fallback) plus the vendor channel |
| `amv_catalog` | Generated catalog of OpenMouse's supported devices (`tools/openmouse/gen_catalog.sh`, needs Node and an OpenMouse checkout in `OPENMOUSE_DIR`); exact and vendor-wide lookup |
| `amv_hid_parse` | Pure-C report-descriptor parser (also built on the host for tests) |
| `amv_usb_host` | Mouse side: High-Speed host; enumerates, claims relative-motion mouse interfaces, keeps 3 interrupt transfers in flight, logs each report descriptor (hex), decides the tier (known / generic / boot / ignored), serves the `GET_MOUSE*` / `SET_MOUSE_*` commands, and runs the tier-1 drivers `drv_razer.c`, `drv_finalmouse.c`, `drv_gearhub.c`, `drv_logitech.c` (HID++ 2.0 over the claimed vendor interface through the `amv_hl_*` shim, read only) (ported from OpenMouse, AGPL-3.0; writes only for models in the driver's `VERIFIED` list) |
| `amv_vector` | Hot path: scales every native report, sums into 1 ms frames, merges several mice, never merges button edges |

The console prints every 5 s: input reports, frames out, raw vs out counts, and per mouse VID:PID, report count and errors.

## Bring-up switches and diagnostics
- **`-DAMV_HOST_ONLY`** (`idf.py -DCMAKE_C_FLAGS=-DAMV_HOST_ONLY build`): leaves the PC-side USB device off, to test the host alone. Clean the `build/` directory afterwards, because CMake remembers the flag.
- **Host port register:** the 5 s status line and a 20 Hz change logger print the High-Speed controller's `HPRT` (connected, enabled, power, D-/D+ line state, speed). Line state `00` with nothing plugged in means no device is pulling the data pair up; a connect shows `conn=1` and the speed (0 high, 1 full, 2 low).
- **Debug logs for the host stack:** connect and enumeration events only appear at debug level; for a session, set `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` and `esp_log_level_set("*", ESP_LOG_DEBUG)` in `amv_usb_host_start()`, and revert afterwards.
- **Saving a mouse's report descriptor:** on attach the console prints each HID interface's descriptor as hex; paste it into `tests/descriptors/<vendor>_<model>_<vid>_<pid>_if<N>.hex` and add a case to `tests/test_hid_parse.c`.

## Test without a mouse
With both USB-C cables connected and the firmware flashed:
```bash
pip install hidapi
python3 -I firmware/esp32p4-dongle/tools/dongle_test.py          # 56 checks over the vendor-HID channel
cc -std=c11 -Wall -Wextra -Icommon firmware/esp32p4-dongle/tests/test_octant.c -lm -o /tmp/test_octant && /tmp/test_octant   # from the repo root
(cd firmware/esp32p4-dongle && cc -std=c11 -Wall -Wextra -Icomponents/amv_hid_parse/include tests/test_hid_parse.c components/amv_hid_parse/amv_hid_parse.c -o /tmp/test_hid_parse && /tmp/test_hid_parse)   # descriptor parser, 30 checks
```
`dongle_test.py` covers the `HELLO` identity handshake, host-to-device writes (valid, invalid, stale), device-to-host events, bypass, profile slots, factory reset, the real scaling code against an independent reference, and persistence across a reboot. It leaves the dongle at factory defaults. Protocol constants are parsed from [`common/amv_dongle_proto.h`](../../common/amv_dongle_proto.h), which is the protocol's reference.

**Status of the debug commands:** `TEST_SCALE` and `TEST_SELF_CHANGE` are in the firmware only for this testing; they do not move your cursor. They should be gated out of release builds.
