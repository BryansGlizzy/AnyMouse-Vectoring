# AnyMouse Teensy 4.1 Bridge (bring-up)

The Teensy 4.1 port of the dongle's mouse path: USB host in, octant vectoring, USB device out, **both ports High-Speed**, so unlike the ESP32-P4 (Full-Speed toward the PC, 1000 Hz cap) it can present an **8 kHz** mouse to the PC. This first version is a bring-up: it proves 8 kHz on both sides and measures what the vectoring costs. Its vendor HID transport currently echoes frames until the protocol handler lands, so Studio support and the OpenMouse engine are still pending (see [Not done](#not-done)). The ESP32-P4 firmware is in [../esp32p4-dongle/](../esp32p4-dongle/README.md); project history is in [../../PROGRESS.md](../../PROGRESS.md).

```
 mouse / receiver ──▶ Teensy USB host pigtail (HS, EHCI)      Teensy main micro-USB (HS device) ──▶ PC
                      USBHost_t36, interrupt IN, bInterval 1  ◀── one 8-byte mouse report in flight, bInterval 1
                              │  ISR: timestamp, parse, amv_octant_scale() (common/amv_octant.h)  │
                              └──────────────▶ accumulator (sum of everything since the last report) ┘
```

## Result (2026-11-04, Finalmouse UltralightX dongle `361d:0100`, set to 8000 Hz)

| | Measured |
|:---|:---|
| **PC side, self-test** (`t 5`, no mouse): 8 kHz +1/-1 X steps | **8000 frames/s** for 5 s on the PC (`measure_pc.py`, kernel timestamps), 39,997 frames, spacing p1 123 us / p50 125 us / p99 127 us / max 169 us |
| **PC side, live mouse, hard motion** | up to 7096 frames/s, p50 spacing 125 us (the rate follows how fast the hand moves; a mouse only reports while its sensor moves) |
| **Host side, live mouse** | the dongle reports at its set rate on a High-Speed interrupt endpoint (`bInterval` 1 = 125 us). Peak 7918 reports/s in one second (ITC 1 run), 7413/s in the final run; the interrupt spacing floor is 124 us |
| **One report per interrupt** | after setting the controller's interrupt threshold to 0, every host interrupt delivers exactly one report (0 interrupts with 2 or more reports); with USBHost_t36's default (threshold 1 micro-frame) about half the interrupts carried two reports 7 us apart and then a 250 us gap, which halved the rate the PC saw |
| **Cost** | whole host interrupt about 5.2 % of the CPU at 7.4k reports/s (600 MHz); report to accumulated 4.1 us (generic HID parser 3.9 us + scale), `amv_octant_scale()` alone 0.25 us average and 0.66 us worst |
| **Latency added** | motion arrives to report queued: 3.1 us average, at most 62 us (the worst case is the main loop being busy for a moment) |
| **Integrity** | sum of the mouse's deltas equals the sum sent to the PC at weight 1.0 (exact); at weight 0.5 the output is exactly half. `[sum ]` line in the console |

Not shown by these runs: a steady 8000.0 reports/s from the mouse. By hand the ULX produced 7.4k to 7.9k per second at best; the figure the mouse can reach is limited by the sensor seeing motion in every 125 us.

## What is needed

- Teensy 4.1 with the USB host pigtail soldered (green D+, white D-; see the dongle README's Teensy section for the wiring trap), the mouse or its receiver on that pigtail, and the **main micro-USB** to the PC. The micro-USB is also the console and the flashing port.
- The **mouse must be High-Speed**, or it is limited to 1 kHz by the USB frame (a Full-Speed device cannot report faster). This build was run with the ULX dongle only.
- PlatformIO (already installed on this box); no Teensyduino install.

## Build and flash

```bash
cd firmware/teensy41-dongle
tools/make_core.sh                    # once: builds ./fw with the AnyMouse USB type and mouse patches
~/.platformio/penv/bin/pio run -t upload
```

`pio run -t upload` reboots the running program into the HalfKay bootloader by itself. `fw/` and `.pio/` are git-ignored; `make_core.sh` can be re-run any time. The installed framework is not modified.

**Patched core and USB type.** The firmware builds with `-D USB_AMV_DONGLE`. `make_core.sh` copies the Teensy 4 core and adds this type with VID:PID `16c0:0489` and product string `AnyMouse Vectoring Dongle (Teensy 4.1)`. It exposes exactly CDC serial, the existing 8-byte mouse interface, and a vendor HID interface based on Teensyduino's RawHID implementation. The mouse keeps `bInterval` 1 (125 us at High-Speed, 1 ms at Full-Speed) and `usb_mouse_idle()` keeps exactly one report in flight. Vendor HID uses usage page `0xFF4D`, usage `0x01`, no report IDs, and 64-byte IN and OUT interrupt reports, both with `bInterval` 1. Keyboard, joystick and SEREMU are not included. The patched copy is picked up with `platform_packages = framework-arduinoteensy @ symlink://./fw`.

## Console and measuring

The Teensy's USB serial (`/dev/ttyACM0`) only talks while the host holds DTR; the helper does that. Opening it does **not** reset this board.

```bash
python3 -I tools/console.py 10 i          # info: host port, speed, weights; then 10 s of the once-a-second stats
python3 -I tools/console.py 10 "r 8000"   # set a Finalmouse dongle's polling rate (also: q to read CPI, rate, battery, firmware, RSSI)
python3 -I tools/console.py 8 "t 5"       # 8 kHz output self-test, no mouse needed
python3 -I tools/measure_pc.py 20         # what the PC receives from the bridge's mouse (reads the evdev node, no grab)
```

Commands: `h` help, `p` stats on/off, `i` info, `t [s]` self-test, `b 0|1` bypass, `w a` / `w e ne n nw w sw s se` weights (0.01 to 5.0, E,NE,N,NW,W,SW,S,SE), `c <n>` host interrupt threshold, `q` and `r <hz>` for a Finalmouse dongle. The stats lines: `[host]` reports/s and spacing histogram, `[isr ]` interrupts and reports per interrupt, `[sum ]` motion in vs. out, `[ pc ]` frames/s sent, queue delay and scale cost.

To run the whole check: `measure_pc.py 20` in one terminal, `console.py 20` in another, and move the mouse hard.

## Findings that cost time

- **Register the driver.** A `USBHIDInput` subclass must call `USBHIDParser::driver_ready_for_hid_collection(&obj)` in `setup()`, or it is never offered the collection (nothing in the base class does it).
- **`hid_input_begin()` runs once per Input item, not per report.** Buttons, X/Y and wheel are separate items; resetting in `begin()` kept only the last. Reset at the end of `hid_input_end()`.
- **Interrupt threshold.** USBHost_t36 sets `USBHS_USBCMD` ITC to 1 micro-frame. At 8 kHz that batches reports in pairs; the firmware clears it (`c 0` at run time shows the difference).
- **The mouse's own USB cable.** A `361d:0102` "UltralightX mouse" (Full-Speed) was also enumerated on the PC during these runs, so the mouse was cabled to the PC, and its dongle still delivered reports to the Teensy (RSSI -18 to -49 dBm). If a mouse goes quiet, check whether it has switched to wired mode.
- Console polling rate on a Finalmouse dongle needs its OUT endpoint: `USBHIDParser::sendPacket()` uses it (see the ESP32-P4 finding in [../esp32p4-dongle/PLAN.md](../esp32p4-dongle/PLAN.md) section 4.0).

## Not done

- **No vendor protocol handler yet:** the `0xFF4D` channel only echoes frames, so the Studio and `./anymouse` cannot configure it. Weights are set from the serial console and are lost at reboot (no flash profile slots).
- **Only one mouse collection** from one device is claimed; keyboards, absolute pointers and vendor interfaces are not handled. Hubs are supported by USBHost_t36 but untested here.
- **Output is the Teensy core's 8-byte report:** 8-bit X/Y, 8 buttons. A frame beyond +-127 counts carries the rest to the next frame, nothing is lost, but 16-bit output is the right long-term format.
- **A press and release inside one 125 us slot would merge** (only the latest button state is sent).
- No OpenMouse engine (CPI and rate for known mice) beyond the hand-written Finalmouse polling command; that needs the per-family loading in the ESP32 PLAN section 4.0.
- Not tested: the Attack Shark R3 receiver, a Razer, a hub, long runs, and the PC link while the PC sleeps.
