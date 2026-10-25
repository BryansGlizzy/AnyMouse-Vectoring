# AnyMouse ESP32-P4 Dongle

USB man-in-the-middle firmware for the Waveshare ESP32-P4-Module-DEV-KIT. The mouse plugs into the dongle's USB-A port; the dongle applies the 8-octant vectoring and appears to the PC as a normal USB mouse. It is configured over a vendor HID channel on the same cable, from a web page (WebHID, nothing to install) or from `./anymouse` (which also does live FpsAimForge calibration). No Wi-Fi, no network.

**Status (2026-10-25):** the control plane works end to end; the mouse side is untested. Board verified (ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM). One USB-C is the CH343 UART bridge (flashing/console); the other is the P4's USB Serial/JTAG by default, but the firmware moves the Full-Speed OTG controller onto it in software (proved by [`spikes/m0_phy_swap/`](spikes/m0_phy_swap/)), so it enumerates as a HID mouse plus a vendor HID channel. Over that channel the firmware stores octant weights in flash (4 profile slots, auto-save, CRC), accepts updates from the host, and pushes changes back as events; a 48-check test passes with no mouse attached. Next is the USB host half (a real mouse on the USB-A port). See [PLAN.md](PLAN.md) for the milestone table.

**Next:** once a real mouse is available, the plan is automatic discovery of every attached mouse, immediate vectoring, real CPI and polling for known mice and a generic fallback for the rest. The design and the acceptance checklist are in [PLAN.md section 9](PLAN.md#9-mouse-discovery-device-info-and-hardware-verification).

**Cabling for now:** the UART USB-C to the Mac (flashing/console), the "other" USB-C to the PC under test, and the mouse receiver in USB-A port 1 with the HOST jumper set.

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

## Test without a mouse
With both USB-C cables connected and the firmware flashed:
```bash
pip install hidapi
python3 -I firmware/esp32p4-dongle/tools/dongle_test.py          # 48 checks over the vendor-HID channel
cc -std=c11 -Wall -Wextra -Icommon firmware/esp32p4-dongle/tests/test_octant.c -lm -o /tmp/test_octant && /tmp/test_octant   # from the repo root
```
`dongle_test.py` covers host-to-device writes (valid, invalid, stale), device-to-host events, bypass, profile slots, factory reset, the real scaling code against an independent reference, and persistence across a reboot. It leaves the dongle at factory defaults. Protocol constants are parsed from [`common/amv_dongle_proto.h`](../../common/amv_dongle_proto.h), which is the protocol's reference.

**Status of the debug commands:** `TEST_SCALE` and `TEST_SELF_CHANGE` are in the firmware only for this testing; they do not move your cursor. They should be gated out of release builds.
