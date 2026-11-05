# M0 spike: PC-facing USB on the "other" USB-C

Proves that the ESP32-P4's Full-Speed OTG controller can be moved, in software, onto the board's second USB-C connector (normally USB Serial/JTAG) and used as a USB device. Part of milestone M0 in [../../PLAN.md](../../PLAN.md).

## What it does
- Calls `usb_wrap_ll_phy_select(&USB_WRAP, 0)` before `tinyusb_driver_install()`, routing the Full-Speed OTG controller to GPIO24/25.
- Enumerates as `303A:4D56` "AnyMouse Vectoring Dongle (M0 spike)" with two HID interfaces: a mouse (it sends **no motion**) and a vendor channel on usage page `0xFF4D`.
- Echoes every report received on the vendor channel back with byte 0 set to `0xE0`.
- Logs `mounted`/`ready` state every 2 s on UART0 (the CH343 USB-C).

## Result (2026-10-25)
The Mac saw both HID devices; 20/20 vendor echoes were correct, median round trip about 5 ms. No eFuse was touched. The mouse-side host half has **not** been tested.

## Build and flash
Needs ESP-IDF v5.5.4 (`~/esp/esp-idf`, see [README.md](../../README.md#toolchain-and-quickstart)), plus `cmake` and `ninja`.
```bash
. ~/esp/esp-idf/export.sh
cd firmware/esp32p4-dongle/spikes/m0_phy_swap
idf.py set-target esp32p4
idf.py build
idf.py -p <UART-USB-C port> flash       # macOS: /dev/cu.usbmodem<serial>; Linux: /dev/ttyACM0 or /dev/ttyUSB0
```
**Flash through the UART USB-C.** While this firmware runs, the other USB-C is the PC link and cannot flash. `SPIKE_FS_PHY_IDX` in `main/main.c` selects the PHY (`0` = GPIO24/25, `1` = default GPIO26/27, `-1` = leave alone).

## Check it
```bash
ioreg -p IOUSB -l -w0 | grep -A8 "AnyMouse Vectoring"      # macOS: 303A:4D56, Full Speed
```
Vendor echo test: open the `0xFF4D` interface with `hidapi`, write a 64-byte report (report ID 0), read the reply; byte 0 comes back as `0xE0`.

## Notes
- Uses `espressif/esp_tinyusb` 3.0.0 (new API: `TINYUSB_CONFIG_FULL_SPEED`, `tinyusb_port_t`). `managed_components/`, `build/` and `sdkconfig` are git-ignored.
- The VID `0x303A` with this placeholder PID is for development only; see PLAN.md section 8.
