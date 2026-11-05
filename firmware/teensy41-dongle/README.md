# AnyMouse Pro Dongle (Teensy 4.1)

This board is the **AnyMouse Pro Dongle**; the ESP32-P4 board is the plain **AnyMouse Dongle**. It shows up as `AnyMouse Pro Dongle` on USB, and announces itself with the `AMV_CAP_PRO` (0x0004) bit in its `HELLO` capabilities, which the Studio and `./anymouse` use for the name and the 8 kHz link rate.

The AnyMouse dongle on a Teensy 4.1: mice or their receivers plug into the Teensy's USB host port, every report is scaled with the 8-octant weights, and the PC sees one ordinary USB mouse. Both Teensy ports are **High-Speed**, so unlike the ESP32-P4 board (Full-Speed toward the PC, 1000 Hz cap) the PC gets an **8 kHz** mouse.

Firmware 0.2.1 (0.2.0 plus the Pro naming) does everything the ESP32-P4 dongle does: the `0xFF4D` vendor channel with the `HELLO` handshake (the Studio and `./anymouse` find it and drive it like the ESP32), four profile slots in EEPROM, the mouse list in tiers, and **OpenMouse's own drivers running unchanged in QuickJS-ng** (the same bundle as the ESP32), so known mice report and set their real CPI, polling rate, battery, lift-off distance and Motion Sync. The ESP32-P4 firmware is in [../esp32p4-dongle/](../esp32p4-dongle/README.md); project history is in [../../PROGRESS.md](../../PROGRESS.md).

```
 mice / receivers          Teensy 4.1 (600 MHz Cortex-M7, 8 MB PSRAM)                               PC
 ─────────────────▶ host port (HS, EHCI, hubs)                          main micro-USB (HS device) ─────▶
                     │ host interrupt: parse (amv_hid_parse), scale            ▲ 8-byte report, 125 us endpoint
                     │ (amv_octant_scale), merge, queue ─────▶ pc_out ─────────┘ sent from interrupts only
                     │ other reports ──▶ per-interface queues                    CDC serial console
                     ▼                                                           vendor HID 0xFF4D ◀──▶ Studio / ./anymouse
                 loop(): mice.cpp (list, tiers, events) ── js_engine.c (QuickJS-ng + OpenMouse bundle, PSRAM heap)
                         amv_proto_teensy.cpp (vendor protocol) ── amv_store_teensy.cpp (EEPROM: slots, boot, CPI labels)
```

## Status (2026-11-05, firmware 0.2.1)

| | |
|:---|:---|
| **PC side** | 8-byte report (16 buttons, 16-bit X/Y, 8-bit wheel and AC Pan; the ESP32's format) on a 125 us endpoint. Self-test: **8000 frames/s**, p50 spacing 125 us |
| **Mouse side** | every HID interface of every device on the host port (hubs through `USBHub`); several mice merged, each with its own sub-pixel remainder; every button change in its own frame; boot-protocol fallback |
| **Vendor channel** | the ESP32's protocol v1 command set, byte-compatible replies. `dongle_test.py`: **55/55** with no mouse (2 ESP32-only board checks skipped) |
| **OpenMouse** | the ESP32's bundle (OpenMouse `326f292`, every family) loads in 2.2 s into a 7 MB PSRAM heap (4.3 MB used). See [OpenMouse engine](#openmouse-engine) |
| **Profiles** | 4 slots, CRC-checked boot record and auto-save in EEPROM emulation; CPI labels for generic mice per VID:PID |
| **Host tools** | `./anymouse --dongle-status`, the GTK app and the Studio work unchanged: they use the handshake and capability replies, not the board |

## Hardware

- Teensy 4.1 with **one 8 MB PSRAM chip** (the OpenMouse engine needs it; without PSRAM the dongle still vectors and reports generic mice) and the USB host pigtail soldered to the host pads: from the 5V pad **5V, D−, D+, GND, GND** (red 5V, white D−, green D+, black GND). Swapping D+ and D− makes a full-speed mouse look like a low-speed one and enumeration fails; never plug the pigtail into a PC.
- The **main micro-USB** goes to the PC: it is the mouse, the vendor channel, the serial console and the flashing port at once.
- A mouse must be **High-Speed** to report faster than 1 kHz (a Full-Speed device is limited to one report per 1 ms frame). The Finalmouse ULX dongle is High-Speed; wired mice usually are not.

## Build and flash

```bash
cd firmware/teensy41-dongle
tools/make_core.sh                         # once: builds ./fw (the Teensy framework with the AnyMouse USB type)
~/.platformio/penv/bin/pio run -t upload   # build and flash (opens the Teensy Loader app, see below)
make test                                  # host unit tests: protocol handler and EEPROM store
```

- **Upload uses the Teensy Loader app** (`upload_protocol = teensy-gui`): the image is about 2 MB (the OpenMouse bytecode alone is 1.18 MB) and the `teensy_loader_cli` 2.2 that PlatformIO bundles keeps images in a 1 MB buffer (`HEX parse error line 65552`, whatever `--mcu` is given). The app opens a small window, reboots the board into its bootloader and programs it; the first time it takes 10 to 20 s. If it says it is busy, run the upload again after a few seconds. The board can always be recovered: press its button and upload.
- **`tools/make_core.sh`** copies the installed Teensy 4 core into `./fw` (everything else is symlinked; the installed framework is not modified) and patches it: the `USB_AMV_DONGLE` USB type (VID:PID `16c0:0489`, manufacturer `AnyMouse`, product `AnyMouse Pro Dongle`; CDC serial, the mouse at `bInterval` 1 and a 64-byte vendor HID interface on usage page `0xFF4D`), the 16-bit mouse report, a non-blocking mouse send with a transmit-complete hook, and a non-blocking RawHID send. Re-run it after a PlatformIO framework update.
- **Linker script** `teensy41_flash_js.ld` is the Teensy 4.1 script with one rule added: QuickJS-ng's code and constants (`qjs_*.o`, `js_engine.c.o`, about 650 KB) stay in flash. The default rules copy all code to ITCM and all constants to DTCM, which share 512 KB.
- **udev:** the manufacturer string `AnyMouse` matches the dongle rule from [the ESP32 README](../esp32p4-dongle/README.md#linux-permissions-webhid-and-anymouse-without-root) (`71-anymouse-dongle.rules`), so a normal user, Chrome and `./anymouse` can open the vendor interface with no Teensy-specific rule.

## Results

### 2026-11-04 bring-up (firmware before 0.1.0, Finalmouse ULX dongle `361d:0100` set to 8000 Hz)

| | Measured |
|:---|:---|
| PC side, self-test | **8000 frames/s** for 5 s (`measure_pc.py`, kernel timestamps), 39,997 frames, spacing p1 123 / p50 125 / p99 127 / max 169 us |
| PC side, live mouse, hard motion | up to 7096 frames/s, p50 spacing 125 us (a mouse only reports while its sensor moves) |
| Host side | the dongle reports on a High-Speed interrupt endpoint with `bInterval` 1. Peak 7918 reports/s in one second; interrupt spacing floor 124 us |
| One report per interrupt | with the controller's interrupt threshold at 0 every interrupt delivers one report; with USBHost_t36's default (1 micro-frame) half of them carried two reports 7 us apart, which halved the rate the PC saw |
| Cost | host interrupt 5.2 % of the CPU at 7.4k reports/s; `amv_octant_scale()` 0.25 us average, 0.66 us worst |
| Integrity | the mouse's delta sums equal the sums sent to the PC at weight 1.0, exactly half at 0.5 |

### 2026-11-05, firmware 0.2.0 (same code path in 0.2.1), Finalmouse UltralightX dongle (real mouse on the host pigtail)

| | Measured |
|:---|:---|
| Recognition | KNOWN through OpenMouse's Finalmouse driver on the first probe: "Finalmouse UltralightX", 1600 CPI, 8000 Hz, battery 83 %, firmware 8.0.3, Motion Sync on, link connected, writable; 95 refreshes in 8 minutes with no failed exchange (call time 114 ms, max 126 ms) |
| Desktop app | `./anymouse --dongle-status` shows the same, with the USB interval (8000 Hz) kept apart from the mouse's set rate |
| Protocol suite | `dongle_test.py` with the mouse attached: **66/66** (2 ESP32-only checks skipped), including the mouse list, descriptor download and capabilities. One check was relaxed: OpenMouse's Finalmouse driver publishes no CPI range (0..0), which now counts as "unpublished" (the options fall back to 50..64000) |
| Live motion | motion in equals motion out in every one-second window (`[sum ]`); one report per host interrupt, interrupt spacing floor 124 us; peak 6030 reports/s in the hardest second (hand speed) |
| Cost | host interrupt 2.3 % of the CPU at 6k reports/s (5.2 % at 7.4k in the bring-up: the shared decoder is cheaper than USBHost_t36's generic parser) |
| Writes through OpenMouse | polling 8000 to 4000 and back, CPI 1600 to 3200 and back: each landed on the mouse and was restored. The mouse applies a write after the driver's own read-back, so the first read-back was stale by up to a whole 5 s refresh; the firmware now re-reads 700 ms after a write and the new value shows about 1 s later |

### 2026-11-05, firmware 0.2.0 (no mouse)

| | Measured |
|:---|:---|
| PC side, self-test (interrupt-driven output, 16-bit report) | 8000 frames/s, 39,986 frames in 5 s, p50 spacing 125 us (p1 86 / p99 164 / max 281 us: the PC's timestamps scattered more than in the 2026-11-04 run; the frame count is exact) |
| `loop()` | at most 25 us per pass with nothing attached (motion does not depend on it any more) |
| Vendor channel | `dongle_test.py` 55/55, round trip 0.62 ms median (the ESP32: about 2 ms) |
| OpenMouse engine | bundle loads in 2189 ms; engine heap 4302 KB (peak 4304 KB) of a 7167 KB PSRAM block |
| OpenMouse drivers (console `fake`: OpenMouse's real Finalmouse driver against a simulated ULX dongle, as `tests/js_bridge/finalmouse_fake.test.mjs`) | driver chosen in 9 ms; readStatus 98 ms (3200 CPI, 2000 Hz, battery, Motion Sync, firmware, rates 500 to 8000); set polling 8000 Hz and CPI 1600 each about 95 ms, both read back; close 336 ms; heap steady at 4318 KB over repeated runs |
| 8 kHz while the engine works | self-test running during three back-to-back `fake` runs (about 2 s of `loop()` busy in the engine): 39,981 frames in 5 s, p50 125 us, max 381 us (10 gaps of 250 to 500 us, none longer) |
| Image | flash: 1.82 MB constant data + 0.13 MB code in flash, 132 KB code in ITCM; RAM1 free for the stack 232 KB |

## How it works

**Motion (interrupts only).** `host.cpp` registers one `USBHIDInput` that claims every top-level collection of every HID interface, so USBHost_t36 hands it the raw report of every interface. In the host interrupt a report on a mouse interface is decoded with the layout `amv_hid_parse` (the ESP32's parser, compiled in place) built from its descriptor, scaled with `amv_octant_scale()` (per-mouse remainder) and fed to `pc_out.cpp`, which sends a frame at once if the PC endpoint is idle; when the PC collects a frame, the transmit-complete hook (device interrupt) sends the next. Several mice: motion summed, buttons OR-ed; each change of the combined button state is queued (16 deep) and sent in its own frame; deltas beyond ±32767 are carried. So `loop()` can spend milliseconds in a driver call without the cursor noticing. The host controller's interrupt threshold is set to 0 (one report, one interrupt).

**Everything else (`loop()`).** Non-motion reports (vendor replies, notifications) go to a 16-report queue per interface while a driver listens. The transport the drivers use is WebHID-shaped: output reports on the interrupt OUT endpoint when there is one (padded to the endpoint size, as an OS does), else `SET_REPORT`; feature reports through `SET_REPORT`/`GET_REPORT` on the control pipe. Each transfer blocks `loop()` until it completes or times out (1 s control, 200 ms OUT): USBHost_t36 never reports a STALLed control transfer to the driver, so a refused request is a timeout.

**The mouse list (`mice.cpp`)** is the ESP32's registry: a device is taken in once all its interfaces have been quiet for 400 ms; one entry per mouse interface (GENERIC, BOOT, or KNOWN once a driver answered), then one entry per device that is not a mouse (IGNORED, reason not a mouse or absolute pointer). `EVENT_MOUSE` on attach, removal and change; measured rate is the highest of the last ten one-second samples. Replies are the ESP32's byte for byte (`GET_MOUSE`, `_CAPS`, `_DESC`, `_MODEL`, `_SETTINGS`, `_OPTIONS`, `SET_MOUSE_CPI`, `_POLLING`, `_SETTING`).

## OpenMouse engine

`js_engine.c` is the ESP32's `amv_js.c` without an RTOS, running the same bundle (`../esp32p4-dongle/components/amv_js/generated/openmouse.bc`, embedded with `.incbin` by `src/openmouse_bc.S`) and the same bridge (`js/bridge.js`, `js/hid_device.js`): OpenMouse's registry picks the driver from the mouse's interfaces exactly as its WebHID control panel does, so every family OpenMouse supports works without a port.

- **Engine:** QuickJS-ng 0.17.0, the vendored sources compiled in place (`src/qjs_*.c`) with `__DJGPP` defined, which selects the engine's no-threads, no-atomics, `gettimeofday` build; nothing in the engine is modified. Code and tables run from flash.
- **Heap:** one 7 MB block of PSRAM from `extmem_malloc`, served by `psram_heap.c` (exact 8-byte size classes up to 2 KB, first fit with splitting above). The core's `extmem_malloc` searches its pool linearly, which is far too slow for the tens of thousands of allocations a bundle load makes.
- **No engine task:** a driver call (readStatus, a write, close) is started and then advanced by `js_call_poll()` from `loop()` until its promise settles (jobs, input reports, timers), up to 15 s for a sleeping receiver; `loop()` keeps answering the vendor channel meanwhile. One call at a time, as on the ESP32. A driver's own transfer blocks only for that transfer.
- **Sessions:** a device whose VID:PID has an engine in the catalog gets a session over all its HID interfaces (x = −1 the mouse's own, 0.. the others); readStatus right away, then every 5 s; a failed first probe is retried when the mouse has produced reports since (or every period for a receiver), after four failures only every sixth period. A KNOWN mouse whose readStatus starts failing keeps its last values, is flagged `DRV_ERROR`, and a receiver link is reported asleep.
- **Write policy:** the ESP32's (README there, write policy): reads for every model, writes only for models verified through a dongle (`1532:0086` Basilisk Ultimate wired, `046d:c547` the G Pro's receiver, `361d:0100` the Finalmouse ULX dongle). A write is started when the request arrives and its reply is sent when the driver has finished (`AMV_ST_PENDING` inside the firmware); a second write meanwhile gets `BUSY`. CPI is range-checked and polling rate, LOD and Motion Sync are checked against what the mouse offers before the driver is called.

## Console and measuring

The Teensy's USB serial (`/dev/ttyACM0`) only talks while the host holds DTR; `tools/console.py` does that. Opening it does **not** reset the board.

```bash
python3 -I tools/console.py 4 i                 # firmware, weights, host port, the mouse list, engine status
python3 -I tools/console.py 10 p                # once-a-second statistics: host, interrupts, motion in/out, PC frames
python3 -I tools/console.py 8 "t 5"             # 8 kHz self-test to the PC, no mouse needed
python3 -I tools/console.py 6 "set 0 rate 8000" # a write through the driver (as the Studio does)
python3 -I tools/measure_pc.py 20 --pid 0489    # what the PC receives (reads the evdev node, no grab)
```

Commands: `h` help, `p` statistics on/off, `i` info, `m` mice, `t [s]` self-test, `b 0|1` bypass, `w a` or `w e ne n nw w sw s se` weights (0.01 to 5.0), `c <n>` host interrupt threshold, `j 0|1|2` driver log (2 = every `console.*` line from the drivers), `set <mouse> <cpi|rate|lod|ms> <value>`, `lbl <mouse> <cpi>` (a generic mouse's CPI label), `fake` (OpenMouse's Finalmouse driver against a simulated ULX dongle: an engine self-test that needs no mouse). Console `w`, `b` and `set` go through the protocol handler with source DEVICE, so the Studio and `./anymouse` see them as events.

## Tests

```bash
make test                                                            # protocol handler and EEPROM store (host)
python3 -I ../esp32p4-dongle/tools/dongle_test.py                    # the 0xFF4D protocol against the board
```

`dongle_test.py` picks the Teensy profile from the product string: only the ESP32's eFuse MAC and chip revision checks are skipped (firmware 0.1.x also skipped the mouse capability, descriptor and self-change checks). It needs the `hidapi` Python module on the hidraw backend.

## Findings that cost time

- **USBHost_t36 hands raw reports only to the first collection's driver,** so one driver claims every collection of every interface (`AllHid`). A `USBHIDInput` must also call `USBHIDParser::driver_ready_for_hid_collection()` or it is never offered anything, and `hid_input_begin()` runs once per Input item, not per report.
- **`Transfer_t::length` is the requested length, not the received one;** the actual length is `length − ((qtd.token >> 16) & 0x7FFF)`.
- **A STALLed control transfer is reported to `enumeration_error()`,** which re-runs enumeration (a no-op once the device is configured) and never tells the driver: the transport times out instead.
- **Interrupt threshold:** USBHost_t36 sets ITC to 1 micro-frame, which batches 8 kHz reports in pairs; the firmware sets 0.
- **Image size:** see Build and flash (Teensy Loader app, linker rule, PSRAM heap).

## Not done

- **Weights on a live mouse** (`w 0.5`, `w 1.5` against real motion, comparing the `[sum ]` lines) were not measured: the mouse was not moving during the runs. The same scaling code was verified with a live mouse at weights 1.0 and 0.5 on 2026-11-04 and by the unit tests; only the new host layer feeding it is untested at other weights.

- **Only one host port:** several mice and hubs are handled in firmware but were only tested with the mouse alone (no hub was at hand).
- **Long runs** and the PC link while the PC sleeps or resumes are untested.
- **The Attack Shark R3 receiver, a Razer and a Logitech receiver** have not been on this board; the engine is the same as the ESP32's, where the Razer and G Pro worked.
- Per-family loading of the OpenMouse bundle (PLAN §4.0 item 4) is not needed with the 8 MB PSRAM chip and was not built; the full registry fits with 2.8 MB to spare.
- The VID:PID is Teensyduino's shared `16c0:0489` (a development id, as the ESP32's `303a:4d56`).
