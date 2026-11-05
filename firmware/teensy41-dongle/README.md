# AnyMouse Teensy 4.1 Bridge (bring-up)

The Teensy 4.1 port of the dongle's mouse path: USB host in, octant vectoring, USB device out, **both ports High-Speed**, so unlike the ESP32-P4 (Full-Speed toward the PC, 1000 Hz cap) it can present an **8 kHz** mouse to the PC. The bring-up proved 8 kHz on both sides and measured vectoring overhead. Firmware 0.1.0 adds the `0xFF4D` vendor HID channel, profile persistence on EEPROM emulation, and WebHID Studio / `./anymouse` support; hardware verification by the operator is pending (see [Verifying the vendor channel](#verifying-the-vendor-channel)). The ESP32-P4 firmware is in [../esp32p4-dongle/](../esp32p4-dongle/README.md); project history is in [../../PROGRESS.md](../../PROGRESS.md).

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

## Vendor channel and persistence (firmware 0.1.0)

Vendor protocol firmware version is 0.1.0 (HELLO and GET_INFO). Boot restores
weights, bypass and active slot from the CRC-checked EEPROM boot record. State
changes are saved from loop() after 2 seconds without another change; failed
saves retry after another 2 seconds. Console `w` and `b` use the same handler as
Studio, bump generation, and emit EVENT_STATE with source DEVICE. `i` shows
version, generation, active slot and valid-slot mask.

GET_STATS uses the shared ESP32 32-byte protocol-counter layout: sets_ok,
stale_rejects, bad_value_rejects, saves, loads, events_sent, events_dropped,
test_scales. All eight fields currently return zero: none corresponds to the
host-report or PC-transmit counters. Those real counters remain in the console
stats. The `[ pc ]` line also reports `vendor-loop max` in microseconds for loop
iterations that receive/retry a request or drain an event. Hardware must verify
8 kHz performance under vendor traffic and EEPROM writes.

Vendor requests and EEPROM calls run only in setup()/loop(), never the host ISR.
Replies retry under backpressure; events may drop. Reboot waits until the reply
has been queued and all vendor IN transfers complete, without blocking motion.

### Vendor mouse information

GET_MOUSE, GET_MOUSE_DESC and GET_MOUSE_MODEL expose the claimed collection,
its captured raw HID descriptor, and the shared OpenMouse catalog name. The
bridge claims one collection as GENERIC. Other HID interfaces visible through
the three USBHost_t36 parsers are IGNORED (claim unavailable); devices the host
library does not expose cannot be listed. Product strings and descriptor bytes
are captured at claim time. Loop snapshots drive both mouse_count and
EVENT_MOUSE; report interrupts only increment the measurement counter.
Measured Hz is the maximum of ten one-second samples (zero after attach until
one sample completes). Unsupported layout facts and device settings stay unknown.

The catalog wrapper compiles the ESP32 catalog C source and generated header
in place, without copies or edits. With Teensy platform 6.0.0 / GCC 15.2.1,
comparison against an empty catalog lookup implementation gave:

| Resource | Shared catalog | Empty lookup | Increment |
| --- | ---: | ---: | ---: |
| Flash (code + data + headers) | 118780 | 93180 | 25600 bytes |
| RAM1 variables | 51904 | 27328 | 24576 bytes |
| RAM1 code + padding | 98304 | 98304 | 0 bytes |
| RAM2 variables | 16160 | 16160 | 0 bytes |

The default Teensy linker places catalog constants in RAM1. The comparison
kept all mouse protocol and capture code present; only catalog lookups were
replaced temporarily. The final build uses the full catalog.

## Verifying the vendor channel

Agents have no physical Teensy attached to test hardware directly; the protocol handler, profile store, and host integrations are verified through unit and mock integration tests. The numbered checklist below is for the human operator to verify the vendor channel on real hardware with a Teensy 4.1 and an attached High-Speed mouse (such as the Finalmouse UltralightX dongle `361d:0100`).

### 1. Build patched core and upload firmware

Build the core patch containing the `USB_AMV_DONGLE` USB type and flash the Teensy 4.1:

```bash
cd firmware/teensy41-dongle
tools/make_core.sh
pio run -t upload
```

**Expected result:** `tools/make_core.sh` copies the Teensy 4 core into `./fw` and configures the `USB_AMV_DONGLE` descriptors (VID:PID `16c0:0489`, product string `AnyMouse Vectoring Dongle (Teensy 4.1)`). `pio run -t upload` triggers HalfKay soft-reboot, flashes the image, and the board re-enumerates as `16c0:0489` with CDC serial, 8-byte mouse (`bInterval` 1), and `0xFF4D` vendor HID.

### 2. Install udev rule for 16c0:0489

Grant non-root seat access to the Teensy vendor hidraw interface:

```bash
sudo tee /etc/udev/rules.d/71-anymouse-teensy.rules >/dev/null <<'EOF'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="16c0", ATTRS{idProduct}=="0489", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger --action=add --subsystem-match=hidraw
```

*(Alternatively, run `sudo ./anymouse --install-udev`, which matches the `AnyMouse` manufacturer string).*

**Expected result:** The `/dev/hidraw*` node for usage page `0xFF4D` is accessible by normal users without `sudo` (verified with `ls -l /dev/hidraw*` or by opening without permission errors).

### 3. Check status and identity (`./anymouse --dongle-status`)

Verify dongle identity and attached mouse status from `./anymouse`:

```bash
./anymouse --dongle-status
```

*Note on `--dongle-info`: `./anymouse` provides `--dongle-status`, which queries and displays full identity (firmware version, protocol version, MAC), vectoring status, weights, profiles, and attached mouse details in one output; there is no separate `--dongle-info` CLI flag. For serial console info, run `python3 -I tools/console.py 5 i`.*

**Expected result:** `./anymouse --dongle-status` finds the vendor interface and outputs:
```text
AnyMouse dongle on /dev/hidrawX
  Identity   : fw 0.1.0, protocol v1, id <mac_address>
  Vectoring  : in hardware (software engine stays idle)
  Weights    : gen 1, slot 0, bypass off
  E=1.000 NE=1.000 N=1.000 NW=1.000 W=1.000 SW=1.000 S=1.000 SE=1.000
  Profiles   : [1 empty] [2 empty] [3 empty] [4 empty]
  Mouse 0    : Finalmouse UltralightX [361d:0100] generic HID, CPI unknown 0, polling rate not reported by the mouse [USB interval 8000 Hz, measured <hz> Hz]
      Details   : extended mouse information not supported by this dongle
      Catalog   : Finalmouse UltralightX
      Link       : connected
```
The link speed ceiling is reported as 8000 reports/s (High-Speed device connection).

### 4. Run automated protocol verification (`dongle_test.py`)

Run the bidirectional protocol test suite against the Teensy:

```bash
python3 -I firmware/esp32p4-dongle/tools/dongle_test.py --profile teensy
```

*(Note: `--profile auto` also selects the Teensy profile automatically by detecting HELLO firmware version `0.1.0` or product string `"Teensy 4.1"`).*

**Expected result:**
- **With Finalmouse ULX dongle attached:** Exactly **56/56 checks pass; 10 skipped**:
  - The 10 skips represent commands specific to ESP32 or not supported by Teensy:
    - 2 board/chip checks: `MAC is the board's efuse MAC` (ESP32-specific) and `chip rev reported` (Teensy returns 0).
    - 4 mouse descriptor and capabilities checks: `report descriptor retrieved in pieces`, `descriptor declares a Generic Desktop Mouse`, `generic mouse has no vendor capabilities`, `polling cannot be set on a generic mouse`.
    - 4 self-originated change checks: `self-change scheduled`, `device-originated event received`, `event carries the new weight`, `host readback matches event`.
- **Without mouse attached:** Exactly **51/51 checks pass; 6 skipped** (the 2 board/chip checks and 4 self-change checks).
- In both cases: **0 failed checks**.

### 5. Connect WebHID Studio and modify weights

1. Open Chrome/Chromium and navigate to `web/index.html` (or the deployed Studio URL).
2. Click **Connect AnyMouse Dongle** and select `"AnyMouse Vectoring Dongle (Teensy 4.1)"`.
3. Adjust octant weights using the polar radar drag handles or individual sliders (e.g. set North to 1.50).

**Expected result:** The Studio connects immediately, displaying product string `"AnyMouse Vectoring Dongle (Teensy 4.1)"` and firmware `0.1.0`. Weight adjustments update the device live with `SET_WEIGHTS`; generation counter increments; the dongle applies the new weights via `amv_plat_t::apply` and emits `EVENT_STATE` with source `HOST`.

### 6. Save profile to slot

1. In the Studio's **Profiles** panel, select Slot 1 (or Slot 2).
2. Click **Save current to slot**.

**Expected result:** The slot indicator dot turns green, confirming the profile was written to Teensy EEPROM storage (`amv_store_teensy`). Querying `./anymouse --dongle-status` now displays `[1 saved, active]`.

### 7. Power cycle and verify boot state persistence

1. Disconnect the Teensy 4.1 main micro-USB cable from the PC and reconnect it (power cycle).
2. Reconnect the Studio, or run `./anymouse --dongle-status`.

**Expected result:** The Teensy boots, reads the CRC32-checked boot record from EEPROM (`load_boot`), and restores the saved active slot, weights, and bypass state (`bypass` is reset to off). The weights and active slot saved in Step 6 are accurately preserved.

### 8. Change Finalmouse ULX polling rate from Studio

1. In the Studio, find the attached mouse controls for the Finalmouse UltralightX.
2. Change the polling rate setting (e.g. from 1000 Hz to 2000 Hz, 4000 Hz, or 8000 Hz).

**Expected result:** The Studio sends `SET_MOUSE_SETTING` for `AMV_SET_POLLING` over usage page `0xFF4D`. The Teensy handler forwards the command to the ULX receiver over the host OUT endpoint (report 17, `0x11`) and reads back status with `0x60`. The new polling rate is confirmed and updated in the Studio UI and in `./anymouse --dongle-status`.

### 9. 8 kHz throughput and latency regression check under vendor traffic

Confirm that active vendor channel communication and Studio polling do not degrade 8 kHz forwarding performance:

1. Keep the Studio connected in Chrome.
2. In Terminal 1, start the PC evdev frame measurement:
   ```bash
   python3 -I tools/measure_pc.py 20
   ```
3. In Terminal 2, start the serial console monitor:
   ```bash
   python3 -I tools/console.py 20
   ```
4. Perform hard, continuous motion with the 8000 Hz mouse across the pad during the 20-second run.
5. Also execute the firmware self-test:
   ```bash
   python3 -I tools/console.py 8 "t 5"
   ```

**Pass thresholds and expected results:**
- **PC self-test threshold:** **8000 frames/s** over the 5-second test (39,997+ frames on the PC; spacing p1 123 us / p50 125 us / p99 127 us / max < 200 us).
- **PC live mouse hard motion threshold:** Median report spacing **p50 is 125 us** (the 8 kHz High-Speed microframe interval; peak rate follows hand motion up to 7000+ frames/s).
- **Vendor loop processing:** Console `[ pc ]` vendor-loop max metric remains small (tens of microseconds) without causing missed microframes or interrupt latency.
- **Integrity:** `[sum ]` counts confirm delta motion in equals delta motion out (exact at 1.0, half at 0.5); 0 dropped reports.

## Not done

- **Output is the Teensy core's 8-byte report:** 8-bit X/Y, 8 buttons. A frame beyond +-127 counts carries the rest to the next frame, nothing is lost, but 16-bit output is the right long-term format.
- **Button-edge merging:** a press and release inside one 125 us slot would merge (only the latest button state is sent).
- **Multiple mice:** only one mouse collection from one device is claimed; multiple mice, hubs, and non-mouse devices are not handled concurrently.
- **Extra USB interfaces:** keyboards, consumer controls, absolute pointers, and extra vendor interfaces are not forwarded or exposed.
- **OpenMouse engine:** no OpenMouse driver engine (CPI and rate for arbitrary known mice) beyond the hand-written Finalmouse polling command; full coverage needs the per-family loading in the ESP32 PLAN section 4.0.
- **Untested configurations:** the Attack Shark R3 receiver, a Razer, a hub, long runs, and the PC link while the PC sleeps.
