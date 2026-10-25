# AnyMouse ESP32-P4 Dongle — Development Plan

A USB man-in-the-middle that runs the 8-octant vectoring in hardware. The mouse (or its 2.4 GHz receiver) plugs into the ESP32-P4. The P4 reads its HID reports, scales them with the same cosine-interpolated octant weights as `./anymouse`, and presents itself to the PC as an ordinary USB mouse. It stores the profile in flash and is configured over a **vendor HID channel on the same USB cable**, from either:

1. **A web page (WebHID).** Nothing to install; open the page in Chrome/Edge, click Connect.
2. **The native `./anymouse` app.** It auto-detects and pairs with the dongle, and is the only front-end that can do live FpsAimForge auto-calibration (the game telemetry lives on the PC).

The dongle works standalone: once a profile is saved, it vectors with no software running on the PC at all. There is no Wi-Fi, no web server on the dongle, and no USB networking (all dropped by decision on 2026-10-25).

This replaces the RP2040 concept in README Phase 4, and removes the need for an OS input-interception driver on Windows, because the PC only ever sees a standard HID mouse.

---

## 0. Verified Hardware (2026-10-25)

Board: Waveshare **ESP32-P4-Module-DEV-KIT** (SKU -A/-B/-C not yet identified; the wiki doesn't say how they differ). The earlier draft of this plan was written against the ESP32-P4-WIFI6 wiki by mistake; that is corrected here.

Probed from the Mac with `esptool` v4.12.0, the boot log, the eFuse summary and `ioreg`:

| Item | Value | Consequence |
|:---|:---|:---|
| Chip | **ESP32-P4, revision v1.3** (ROM `esp32p4-eco2-20240710`) | Pre-v3 silicon: build with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, CPU max **360 MHz** |
| CPU | 2 × RISC-V HP cores @ 360 MHz, single-precision FPU | `atan2f`/`cosf` per report is cheap; LUT optional |
| Flash | 16 MB GigaDevice (`c8:4018`), QIO 80 MHz | Plenty for dual OTA slots |
| PSRAM | 32 MB in-package | Available for logs/histograms; keep the hot path in internal SRAM |
| eFuses | No secure boot, no flash encryption, download mode enabled, JTAG enabled | A bad app image can always be replaced; leave it this way during development |
| Existing firmware | Arduino "Haltech Multi-Gauge" (IDF v5.5.4) | User confirmed disposable; no backup taken |

### USB ports on this board
| Port | Wired to | Verified | Usable as |
|:---|:---|:---:|:---|
| USB-C, **UART** | CH343 bridge, `1A86:55D3` (`/dev/cu.usbmodem5B5E1281591` on the Mac) | Yes | Flashing + console only (cannot be a HID mouse) |
| USB-C, **the other one** | ESP32-P4 built-in **USB Serial/JTAG**, Espressif `303A:1001`, serial = MAC `30:ED:A0:EA:6C:AB` | Yes (2026-10-25) | Flashing, console, JTAG. **Fixed-function: cannot be a HID mouse** |
| USB-A, 2 stacked double-decker receptacles (4 ports) | The P4's HS OTG D+/D− goes through an **FSUSB42 analog mux** (`SEL: H→2, L→1`, driven by the HOST/DEVICE jumper and `R37` 47 K): position HOST = **port 1, direct** to the P4; position DEVICE = a **CH334 hub** feeding ports 2–4 | Schematic (2026-10-25) | **Mouse side: HS host.** Jumper stays on HOST |
| RJ45 Ethernet | IP101GRI over RMII | Wiki | Unused |
| 40-pin header | `GPIO26` and `GPIO27` (OTG_FS pins, module pins 53/54) appear in the header net list; the physical pin numbers are not confirmed | Schematic (net names only) | Candidate source of the PC-side FS OTG pins |

**Schematic facts (Waveshare ESP32-P4-Module-DEV-KIT.pdf, page 1 net labels):**
- UART USB-C: `USB0_5V` + `USB_DBG_D_P/N` → CH343P. Other USB-C: `USB1_5V` + `USB1P1_P/N` → module pins `GPIO24/25`. Matches the `ioreg` results.
- Host-mode VBUS for the USB-A ports (`VBUS_OUT`) comes from an `AO3401` P-FET and a `DIO7003` power switch. Its enable pin is not readable from the text extraction.
- The P4's one HS controller feeds **both** USB-A stacks through the mux, so they are mutually exclusive, not two independent ports. Both stacks are host-facing receptacles; neither can be the output to the PC.
- eFuse `USB_PHY_SEL` exists and is unset (0). `USB_OTG11_EXCHG_PINS` and `USB_DEVICE_EXCHG_PINS` (D+/D− swaps) are also unset.

The P4 has two OTG controllers (one High-Speed, one Full-Speed). The FS controller shares two FS PHYs with USB Serial/JTAG; by default Serial/JTAG sits on GPIO24/25 and OTG_FS on GPIO26/27. The schematic matches this: the "other" USB-C is on GPIO24/25.

### The remaining hardware question (gate for everything else)
The mouse goes into the USB-A port (HS host). The **PC-facing link needs the FS OTG controller acting as a device**, and neither USB-C port gives us that today. Three ways, in order of preference:

| Option | What it takes | Risk |
|:---|:---|:---|
| **A. Header breakout** | Wire OTG_FS D+/D− (expected GPIO26/27) from the 40-pin header to a USB cable (plus VBUS → the 5 V terminal and GND). | Needs those GPIOs actually exposed on the header (schematic check). Reversible. |
| **B. Software PHY swap** | `usb_wrap_ll_phy_select(&USB_WRAP, 0)` (writes `LP_SYS.usb_ctrl.sw_usb_phy_sel`) puts OTG_FS on GPIO24/25 so the "other" USB-C becomes the PC link. | **Verified working on this board, 2026-10-25** (see result below). Reversible. |
| **C. eFuse PHY swap** | Burn the USB PHY-select eFuse so OTG_FS drives GPIO24/25 and Serial/JTAG moves to 26/27. | **Irreversible.** Needs explicit user approval; the UART USB-C would remain for flashing, but that is unverified until the schematic confirms it. Last resort. |

M0 does not burn any eFuse. If A or B work, we never need C.

### Option B result (spike: `spikes/m0_phy_swap/`, ESP-IDF v5.5.4, `esp_tinyusb` 3.0.0)
The spike calls `usb_wrap_ll_phy_select(&USB_WRAP, 0)` before `tinyusb_driver_install()` on `TINYUSB_PORT_FULL_SPEED_0`, with a composite descriptor (HID mouse + vendor HID on usage page `0xFF4D`). Observed from the Mac:
- The "other" USB-C stopped being Espressif `303A:1001` (Serial/JTAG) and enumerated as `303A:4D56` "AnyMouse Vectoring Dongle (M0 spike)", Full Speed, serial `M0-0001`.
- macOS created two HID devices: Generic Desktop / Mouse, and usage page `0xFF4D` (the vendor channel).
- Vendor round trip with `hidapi`: **20/20 echoes correct**, median RTT about 5 ms (includes macOS scheduling and 1 ms FS polling each way; fine for control traffic).
- No eFuse was touched. The mouse-side (HS host on the USB-A port) has not been exercised yet; that is the second half of M0.

**Consequences:**
- The PC link is the existing "other" USB-C. **No header wiring is needed**, so GPIO26/27 stay free and Option A is a fallback only.
- That connector can no longer be used for flashing or console while the firmware runs. The UART USB-C (CH343) remains the flashing and console port. This is also the recovery path if a build misbehaves.
- Serial/JTAG moves to the other FS PHY (GPIO26/27, unconnected on this board). Whether a chip reset restores the default mapping (so the ROM download mode on the "other" USB-C works again) has not been tested; assume it does not until verified, and keep the UART cable available.
- The board is powered through whichever USB-C is connected (VBUS present on that connector). Since the PC supplies it, no external 5 V is needed for the PC side. Whether the USB-A port can supply the mouse receiver while powered from the "other" USB-C is still to be checked in the host-side spike.

**Hard limit:** a Full-Speed interrupt endpoint polls at most every 1 ms, so **output to the PC is capped at 1000 Hz**. Input is still read at the mouse's native rate (the High-Speed side handles 8 kHz receivers), and each native report is scaled individually, then scaled deltas and the sub-pixel remainder are summed into the next 1 ms frame. No motion is lost. FpsAimForge's calibrator samples the camera at 1 kHz, so calibration quality is unaffected.

---

## 1. Architecture

```
            ┌───────────────────────── ESP32-P4 (360 MHz × 2) ────────────────────────┐
 Mouse /    │  HS OTG (USB Host, USB-A)                    FS OTG (USB Device, TinyUSB) │   PC
 receiver ──┼─▶ enum + HID report-descriptor parse           ├─ IF0 Mouse (16-bit X/Y, ─┼──▶ any OS, no driver
 (any)      │   interrupt IN @ native rate                   │   buttons, wheel, pan)   │
            │        │                                       └─ IF1 Vendor HID (64 B) ◀─┼──▶ Web page (WebHID)
            │        ▼   core 0, highest priority                     ▲                 │    and/or ./anymouse
            │   amv_vector: per-report cosine C¹ scaling + remainder ──┘ 1 ms frames    │
            │        ▲  weights (atomic, double-buffered)                               │
            │        │                                                                  │
            │   amv_store (NVS profiles) ◀──▶ amv_ctrl (single owner of state, gen counter)
            │                                      ▲                                    │
            │   core 1:  amv_proto (vendor HID commands, DFU)                           │
            └───────────────────────────────────────────────────────────────────────────┘
```
No radio, no network stack, no HTTP server. The ESP32-C6 on the board stays unused.

### Data path (hot path, core 0)
1. The USB host client task (highest app priority, pinned to core 0) receives each interrupt IN transfer and resubmits it immediately.
2. The report is decoded using the layout parsed from the mouse's report descriptor at enumeration: report ID, bit offsets, sizes and signedness for X, Y, wheel, AC Pan and buttons.
3. `amv_octant_scale()` runs per native report. It is the same math as `scale_vector()` in `src/evdev_rt.c`: atan2 → octant position → cosine smoothstep → weight blend → remainder diffusion.
4. Scaled integer deltas are added to a pending accumulator under a `portMUX` spinlock.
5. On the PC side, `tud_hid_report_complete_cb` sends the next frame as soon as the previous one completes, so the output runs at the 1 kHz frame rate. An idle → active transition kicks a send immediately.
6. **Button edges are never merged.** If a button changes twice before a frame goes out, the second change is queued for the following frame, so clicks are never swallowed.
7. Deltas exceeding int16 are carried over to the next frame, not clamped.

### Control plane (core 1)
- **`amv_ctrl` owns the state:** weights, 4 profile slots, bypass flag, CPI hint. Every writer (vendor HID, BOOT button) goes through it. Each change bumps a `weights_gen` counter, notifies subscribers (an async HID event to every attached client), and triggers a debounced NVS save.
- **Weights reach the hot path through a seqlock:** the writer bumps a sequence counter to odd, writes the eight weights (as atomic bit patterns) and the bypass flag, then bumps it to even. The reader copies and retries if the counter was odd or changed. It never takes a lock or blocks the control plane, and never sees a half-written set. (This replaced the double-buffer idea, which can hand a reader a buffer that is being rewritten after two quick updates.)
- **Weights are validated** with the same rule as the host UDP path: 0.01 ≤ w ≤ 5.0, no NaN. Bad input is rejected, not silently clamped.

### Ownership between dongle and clients
The dongle is the **source of truth**. A client reads weights + generation on connect and adopts them. Edits carry the generation they were based on; a stale edit gets a NACK with the current weights. A change from one client (e.g. the web page) reaches the other (the native app) as an async `WEIGHTS_CHANGED` event. Conflicts resolve last-writer-wins by generation, which is deterministic and fine with one human editing.

**Two clients at once:** Linux allows several processes on one hidraw node, but Windows HID access is typically exclusive. The web page must show a clear "device in use by another app" error instead of failing silently.

### What each front-end can do
| | Web page (WebHID) | `./anymouse` |
|:---|:---:|:---:|
| Edit weights, presets, profiles, bypass | ✅ | ✅ |
| Live input-direction needle, rates, latency stats | ✅ | ✅ |
| Firmware update | ✅ | ✅ |
| Live FpsAimForge auto-calibration | ❌ (game telemetry is in `/dev/shm` on the PC) | ✅ |
| Needs install | No (Chromium browser only) | Yes |
| Windows / macOS / Linux | Yes | Linux now, Windows later |

---

## 2. Repository Layout

All firmware lives in this folder. The math and the wire protocol are shared with the Linux engine through `common/` at the repo root, so each has one source of truth.

```
AnyMouse-Vectoring/
├── common/                          # NEW: header-only, shared by src/ and firmware/
│   ├── amv_octant.h                 # scale math (moved out of src/evdev_rt.c, behaviour-identical)
│   └── amv_dongle_proto.h           # vendor HID protocol: commands, structs, versions
├── src/                             # Linux engine (gains dongle.c, see §5)
├── web/                             # NEW (repo root): static WebHID Studio, hosted separately from the dongle
└── firmware/
    └── esp32p4-dongle/
        ├── PLAN.md                  # this file
        ├── README.md                # build / flash / wire-up quickstart
        ├── CMakeLists.txt           # ESP-IDF project root
        ├── sdkconfig.defaults       # rev<3, 360 MHz, USB, rollback
        ├── partitions.csv           # nvs, otadata, ota_0, ota_1, coredump
        ├── main/                    # boot, task layout, watchdog
        ├── components/
        │   ├── amv_vector/          # wraps common/amv_octant.h + accumulator + button-edge queue
        │   ├── amv_hid_parse/       # report-descriptor parser (pure C, host-testable)
        │   ├── amv_usb_host/        # HS OTG: enumeration, interface claim, IN transfers
        │   ├── amv_usb_device/      # FS OTG: TinyUSB composite descriptors + callbacks
        │   ├── amv_ctrl/            # state owner, gen counter, subscribers
        │   ├── amv_store/           # NVS profiles, debounced writes, CRC + schema version
        │   └── amv_proto/           # vendor HID command handler + firmware-update (DFU) handler
        ├── spikes/
        │   └── m0_phy_swap/         # M0 proof of concept: PC-facing USB on the second USB-C (see its README)
        ├── tests/                   # host-side unit tests (plain gcc/clang, run on Mac or Linux)
        │   ├── descriptors/         # real report descriptors captured from ULX, R3, others
        │   └── golden/              # octant-math golden vectors shared with the Linux engine
        └── tools/
            ├── capture_descriptors.sh   # dumps /sys/class/hidraw/*/device/report_descriptor
            └── gen_proto_js.py          # generates web/ protocol constants from common/amv_dongle_proto.h
```

`web/` sits at the repo root, not under `firmware/`, because it is a static site with its own deployment, not firmware. Its protocol constants are **generated** from the C header, so the JS and C can't drift.

`build/`, `managed_components/` and `sdkconfig` are git-ignored (see `.gitignore` here).

---

## 3. Toolchain

- **ESP-IDF v5.5.4, pinned.** This exact version already ran on this chip (it built the Haltech sketch), and it supports pre-v3 P4 silicon via `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Re-evaluate v6.x only after M3, and only if a needed fix lands there.
- **Install on macOS** (same on CachyOS):
  ```bash
  brew install cmake ninja dfu-util ccache python@3.12
  mkdir -p ~/esp && git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf
  ~/esp/esp-idf/install.sh esp32p4
  . ~/esp/esp-idf/export.sh
  ```
- **Managed components** (pinned in `idf_component.yml`): `espressif/esp_tinyusb` (device). The USB host stack is IDF's built-in `usb_host`; HID class handling is our own because we need the raw descriptor. No Wi-Fi, esp_hosted, mDNS or HTTP components.
- **Key `sdkconfig.defaults`:** `CONFIG_IDF_TARGET="esp32p4"`, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, 360 MHz CPU, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_FREERTOS_HZ=1000`.
- **Partition table:** `nvs` 24 KB, `otadata` 8 KB, `ota_0` 2 MB, `ota_1` 2 MB, `coredump` 64 KB (the firmware is small now that there is no network stack; the rest is spare).

---

## 4. Milestones

Each milestone has an exit test. Nothing moves on until its test passes on real hardware.

### Progress snapshot (2026-10-25)
| Milestone | State |
|:---|:---|
| M0 PC-facing link | **Done** (device half). Host half (mouse on the USB-A port) waits for a real mouse |
| M1 passthrough | Not started: needs the host half (descriptor parser, `usb_host`, accumulator, button queue) |
| M2 vectoring + persistence | **Mostly done without a mouse:** `common/amv_octant.h`, seqlock weights, NVS slots with CRC, auto-save. Not done: wiring scaled deltas to the PC mouse report; switching `src/evdev_rt.c` to the shared header (cannot be built or tested off-Linux) |
| M3 vendor protocol + engine | **Firmware side done and tested** (44/44 checks, `tools/dongle_test.py`). Not done: `src/dongle.c` in the Linux engine, the double-scaling guard, FAF calibration through the dongle |
| M4 Web Studio | **Built** in [`web/`](../../web/README.md): chart, weights, presets, slots, import/export, demo mode, 16 Node tests against recorded frames. Not yet exercised in a real browser against the board; firmware update (DFU) and the live calibration mirror are not in it yet |
| M5 hardening / DFU | Not started (partition table with two OTA slots already in place) |

**Built so far** (firmware 0.3.0; protocol additions since the first test: `GET_SLOTS`, `AMV_USB_VID/PID` in the header; 48 firmware checks): components `amv_common` (links repo-level `common/`), `amv_store`, `amv_ctrl`, `amv_proto`, `amv_usb_device`; `tools/dongle_test.py`, `tools/gen_proto_js.py`, `tools/capture_fixtures.py`; `tests/test_octant.c`; and the web app in `web/`. Debug-only protocol commands `TEST_SCALE` and `TEST_SELF_CHANGE` let the host exercise the real scaling code and the device-to-host event path without a mouse; they should be compiled out (or gated) in release builds.

### M0 — Bring-up and the PC-link gate *(blocks everything)*
1. Install ESP-IDF v5.5.4. Build and flash `hello_world` for `esp32p4` (rev<3 option) over **both** USB-C ports and confirm the console on each.
2. From the Waveshare schematic, confirm:
   - which GPIOs the "other" USB-C (Serial/JTAG) is on, and whether OTG_FS (GPIO26/27 expected) reaches the 40-pin header (Option A);
   - the USB-A port: how the host/device jumper works, and where its VBUS comes from (whether it can power a mouse receiver);
   - the BOOT button GPIO (for runtime use);
   - how the board is powered when the PC link is a header cable (5 V terminal).
3. **Done:** Option B (software PHY swap) works, so the "other" USB-C is the PC link and Option A is not needed. **Option C (eFuse) is not attempted without explicit approval.**
4. **Spike (device half done, host half remaining):** TinyUSB device on FS OTG enumerates on the PC as a dummy HID mouse + vendor HID (done), while IDF `usb_host` on HS OTG enumerates a real mouse and prints its descriptors, **at the same time** (to do; needs the mouse in USB-A port 1 and the HOST jumper, which is already set).
   - Confirm how `usb_host` selects the HS controller in IDF v5.5.4 and how `esp_tinyusb` picks the FS one. Espressif says the hardware supports both controllers at once; the software selection must be verified, not assumed.
   - **Fallback** if the two stacks can't coexist: TinyUSB dual-role (device on one rhport, host on the other, DWC2 driver), dropping IDF `usb_host`.
- **Exit:** both sides enumerate simultaneously and stay stable for 30 minutes of mouse movement, with no host-side transfer errors in the log.

### M1 — Transparent passthrough (weights fixed at 1.0)
1. `amv_hid_parse`: parse report descriptors and build the field layout for the mouse application collection.
   - Handle report IDs, Usage Page / Usage / Usage Min-Max, Logical Min/Max (signedness), Report Size/Count, and Push/Pop.
   - Locate X, Y, Wheel, AC Pan and Buttons 1–16.
   - Unit-test on the host against descriptors captured from the Finalmouse ULX and Attack Shark R3 (`tools/capture_descriptors.sh` on the CachyOS box).
2. `amv_usb_host`: claim every HID interface whose descriptor contains Generic Desktop / Mouse; send SET_IDLE 0; stay in report protocol (never boot protocol, which is 8-bit only); submit interrupt IN transfers; handle detach and re-attach.
3. `amv_usb_device`: composite descriptor.
   - IF0: mouse with 16-bit X/Y, 8 buttons, wheel and AC Pan, `bInterval=1`, and boot-protocol support for BIOS (downscaled 8-bit reports when the host selects boot protocol).
   - IF1: vendor HID, 64 B in/out on usage page `0xFF4D`, distinct from the Finalmouse `0xFF00` page so `src/hid_driver.c` never confuses them. Web-friendly: no report IDs.
4. Accumulator and button-edge queue (§1). The PC-side device stays enumerated when the mouse is unplugged, so the OS doesn't see churn.
5. Instrumentation: per-report timestamps (host IN → device frame sent), input rate, output rate, dropped and overflow counters, printed every 5 s on the console.
- **Exit:**
  - `evhz`/`evtest` on Linux show 1000 Hz from the dongle.
  - Total counts over a long swipe match a direct connection within ±0.1%.
  - Fast double-clicks are never lost in a 1000-click test.
  - Added latency is median ≤ 1.0 ms and p99 ≤ 1.5 ms (dongle-internal timestamps, confirmed once with GPIO toggles on a logic analyzer).

### M2 — Octant vectoring + persistent profile
1. Move the `scale_vector()` math from `src/evdev_rt.c` into `common/amv_octant.h` as a header-only pure function over a small state struct (`rem_x`, `rem_y`). The Linux engine switches to it in the same change, behaviour-identical.
2. Golden-vector test: a fixed set of (dx, dy, weights) → (out, remainder) cases checked by both the Linux build and the firmware unit test, so the two can't drift.
3. `amv_vector` applies the weights per native report. `amv_ctrl` handles the double-buffer swap and generation counter.
4. `amv_store`: NVS namespace `amv`; per slot a blob `{schema_ver, crc32, gen, weights[8], cpi_hint, name[24]}` for 4 slots, plus the active index. Saves are debounced 2 s after the last change. A corrupt or missing blob boots with neutral 1.0 weights and logs why.
5. Failsafe: holding BOOT for 1 s toggles **bypass** (all weights 1.0, profile untouched). Bypass is also reachable over vendor HID. The console logs the state on each change.
- **Exit:** a profile set over the console survives power cycles; a vertical-boost preset visibly changes vertical vs. horizontal sensitivity; golden-vector tests pass on both targets.

### M3 — Vendor HID protocol + Linux engine integration (live FpsAimForge calibration)
1. `common/amv_dongle_proto.h` (**implemented, protocol v1**): 64-byte frames, an 8-byte header `{magic 'A' 'M', proto_ver, cmd, seq, status, len, reserved}` plus up to 56 bytes of payload. Replies echo `cmd` and `seq` and carry a status code; changes are announced to every client by an async `EVENT_STATE` frame tagged with its source (host, device, load, boot). The implemented command set is `GET_INFO`, `GET_WEIGHTS`, `SET_WEIGHTS` (with `base_gen`, stale writes get `STALE` plus the current state), `SAVE_PROFILE`, `LOAD_PROFILE`, `SET_BYPASS`, `GET_STATS`, `FACTORY_RESET`, `REBOOT`, and the debug commands `TEST_SCALE` / `TEST_SELF_CHANGE`; the header is the authority. The original design list follows; items not yet implemented are `STREAM_DIR`, `PUSH_CALIB`, the DFU commands and the Wi-Fi ones (dropped). Original list:
   - `GET_INFO` (fw version, proto version, chip rev, downstream mouse VID:PID/name, input rate)
   - `GET_WEIGHTS`, `SET_WEIGHTS{base_gen, weights[8]}` → `ACK{new_gen}` / `NACK{reason, current_gen}`
   - `SAVE_PROFILE{slot}`, `LOAD_PROFILE{slot}`, `SET_BYPASS{on}`
   - `GET_STATS` (rates, latency histogram, counters)
   - `STREAM_DIR{on}` (30 Hz input-direction needle θ + effective scale for the radar)
   - `PUSH_CALIB{per-octant median r̃_k, n, last P₁/T}` (lets the web page mirror the PC's calibration panel)
   - DFU commands (see M5) and `REBOOT{to_bootloader?}`
   - Async IN event: `WEIGHTS_CHANGED{gen, weights[8]}`.
2. Linux engine, new `src/dongle.c`:
   - Find the dongle by VID:PID plus the `0xFF4D` vendor collection (reusing `descriptor_has_collection()` in `src/hid_driver.c`).
   - A sync thread polls `g_state.octant_weights` at 50 Hz, diffs, and pushes changes. Polling means none of the existing writers (GUI, calibration, presets, profile load, UDP) need to change.
   - It applies incoming `WEIGHTS_CHANGED` events to `g_state`, and forwards the calibration snapshot after each round.
3. **Double-scaling guard (critical):** `devices.c` gives the dongle a "never grab" rank. While a dongle is attached, `evdev_rt` stays idle instead of grabbing another mouse, unless the user picks one explicitly. The GUI shows "Vectoring: Dongle" instead of "Vectoring: Software".
4. CLI: `./anymouse --dongle-status`, `--dongle-bypass on|off`. `--install-udev` gains rules for the dongle (needed for hidraw access by the app **and** by the browser's WebHID on Linux).
- **Exit:**
  - A full `AnyMouse Calibration Suite` session in FpsAimForge with auto-adapt on converges as it does with software vectoring (r̃_k within noise of 1.0 by round 8–12).
  - Weights on both sides match by generation number after the session.
  - Pulling the dongle mid-session falls back cleanly.

### M4 — Web Studio (WebHID, port of the GTK GUI)
1. `web/`: a static single-page app in vanilla JS + canvas, no framework, no CDN. Target under 150 KB before gzip. Protocol constants generated from `common/amv_dongle_proto.h` by `firmware/esp32p4-dongle/tools/gen_proto_js.py`.
2. Features, in priority order:
   - **Connect:** `navigator.hid.requestDevice` filtered to the dongle's VID:PID and the `0xFF4D` usage page; auto-reconnect to a previously granted device on page load.
   - **Polar radar:** draggable handles, the same rendering conventions as `gui_gtk4.c`, and the live input-direction needle.
   - **Weight controls:** 8 sliders + numeric fields, master scale, ±symmetry links, presets (Neutral, Vertical Boost, Horizontal Boost, Diagonal ULX, Low, matching `core_apply_preset`).
   - **Profiles:** 4 slots, save/load, JSON export/import compatible with `profile.json` from the Linux engine.
   - **Status:** downstream mouse name and VID:PID, input/output rate, latency histogram, bypass, CPI hint for effective-DPI readouts.
   - **Calibration mirror:** per-octant r̃_k, trial progress and last P₁ vs T when the native app is pushing (read-only).
   - **Firmware update** (once M5 lands).
3. **Hosting.** Without Wi-Fi or USB networking the dongle can't serve the page itself. It is served as a static HTTPS site (e.g. GitHub Pages; WebHID needs a secure context) and cached for offline use as a PWA. The native app can also serve the same files on `localhost`. This is the "nothing to download" path: a bookmark and a click.
4. **Browser support:** WebHID is Chromium-only (Chrome, Edge, Brave, Opera). Firefox and Safari can't use the web page; they use the native app. The page detects this and says so. Flatpak browsers on Linux cannot see `/dev/hidraw*` without an override (same caveat as README §9).
5. WebHID blocks top-level mouse/keyboard collections, which doesn't affect us because the browser only ever opens the vendor interface (IF1).
- **Exit:** every control works from a fresh Chrome on Windows, macOS and Linux with no app installed. A web edit reaches the GTK GUI within 100 ms through `WEIGHTS_CHANGED`, and a JSON profile round-trips between the web page and the Linux engine unchanged. Opening it while the native app holds the device on Windows produces the clear "in use" message.

### M5 — Hardening, firmware update over HID, release
- **DFU over the vendor channel** (no Wi-Fi, so USB is the update path): `DFU_BEGIN{size, crc, version}`, `DFU_CHUNK{offset, data}`, `DFU_END`. Writes the inactive OTA slot, verifies size and CRC32, then reboots into it. Both front-ends can drive it. The Serial/JTAG and UART USB-C ports remain a wired recovery path via `esptool`.
- App rollback: a new image must pass a 30 s self-test (both USB sides up, vendor HID answering) before it is marked valid; otherwise it reverts.
- DFU is not signed. Anyone with physical USB access to the dongle can already reflash it over the programming ports, so signing adds nothing at this stage; revisit if the dongle is ever distributed.
- Task watchdog on the USB host task, brown-out detection, core dump to flash (readable via `GET_STATS`).
- 8-hour soak with the ULX and R3 receivers: hotplug both sides 100×, PC sleep/wake, and mouse receiver sleep.
- `README.md` here: wiring photos and pinout, flash steps, recovery. Update the top-level README §7 Phase 4 and PROGRESS.md.

### M6 — Stretch goals (after M5, in value order)
1. **Vendor-command tunnel (largely superseded by [section 9](#9-mouse-discovery-device-info-and-hardware-verification), where the dongle runs the OpenMouse drivers itself; a raw tunnel stays useful for mice with no known driver):** `TUNNEL_SET/GET_REPORT{iface, type, id, data}` forwards to the downstream mouse. `src/hid_driver.c` gets a transport abstraction (hidraw vs dongle tunnel), so Finalmouse CPI/polling and Attack Shark polling control keep working through the dongle, from the app and (later) from the web page.
2. **Extra downstream interfaces:** mirror keyboard/consumer collections (side buttons or DPI keys that some mice send as keystrokes) as extra device interfaces with cloned descriptors.
3. **Windows companion:** a `hidapi` build of the dongle sync for live calibration on Windows. Vectoring itself already works on Windows with no software at all.
4. **Optional identity cloning:** present the downstream mouse's VID:PID and strings, for OS per-device settings. Off by default, because cloning a supported vendor ID would make `src/hid_driver.c` talk the wrong protocol to the dongle.
5. **WebUSB landing page** (unverified whether current Chrome desktop still surfaces it): a hint to open the web page when the dongle is plugged in.

---

## 5. Linux Engine Changes (summary)

| File | Change | Milestone |
|:---|:---|:---|
| `common/amv_octant.h` | New: scale math shared with firmware | M2 |
| `src/evdev_rt.c` | Use `common/amv_octant.h`; stay idle when a dongle owns vectoring | M2, M3 |
| `src/dongle.c` (new) | Discovery, sync thread, event handling, calib push | M3 |
| `src/devices.c` | Exclude the dongle from auto-grab; flag it in `--list-devices` | M3 |
| `src/gui_gtk4.c` | "Vectoring: Dongle" badge, dongle status row | M3 |
| `src/main.c` | `--dongle-status`, `--dongle-bypass` | M3 |
| `src/steam_install.c` (udev) | Add dongle VID:PID to the uaccess rules | M3 |
| `Makefile` | Add `-Icommon`; `make test-common` runs the golden vectors | M2 |

Unrelated doc drift noticed while planning: README §7 says the UDP weight channel was removed, but `src/evdev_rt.c` still listens on UDP 42069. Left alone; the dongle sync doesn't depend on it.

---

## 6. USB Identity

- **Development:** Espressif VID `0x303A` with a placeholder PID, product string "AnyMouse Vectoring Dongle", and serial number from the P4 MAC. Note that the board's built-in Serial/JTAG already uses `303A:1001`, so the dongle's own PID must differ.
- **Before sharing builds with anyone else:** request a free PID from Espressif's `espressif/usb-pids` program for `0x303A`.
- **KDE note:** KWin keys per-device pointer settings on VID:PID (the same issue the engine solved with `EVIOCGID` cloning). Set the dongle to the Flat profile once in System Settings. On Windows, turn off "Enhance pointer precision".

---

## 7. Risks

| Risk | Likelihood | Mitigation |
|:---|:---:|:---|
| No reversible way to get OTG_FS to a PC-facing connector (header pins not exposed, no software PHY swap) | Medium | Checked first in M0. Fallbacks: eFuse swap with explicit approval (irreversible), or a different board with two OTG connectors. |
| IDF `usb_host` and esp_tinyusb can't share the chip | Medium | TinyUSB dual-role fallback (M0) |
| An 8 kHz receiver misbehaves behind the P4 host (NAK storms, gaps) | Medium | Measure input rate in M1. Mouse firmware accumulates deltas, so a gap lowers the rate but loses no motion. Tune with multiple in-flight transfers. |
| USB-A VBUS can't power a receiver in host mode | Low–Med | Checked in M0 against the schematic; fallback is an external 5 V feed |
| WebHID unavailable (Firefox, Safari, Flatpak browsers) | Certain for those | Native app covers them; page detects and explains |
| Windows exclusive HID access when app and web page both run | Medium | Clear "in use" message; generation counter keeps state consistent across sequential use |
| DFU interrupted mid-update | Low | Inactive slot only, CRC check, rollback; UART/Serial-JTAG recovery always works |
| Pre-v3 silicon errata or dropped support in newer IDF | Low–Med | Pin IDF v5.5.4; never update IDF mid-milestone |
| Double scaling (dongle + software engine both active) | High if unguarded | M3 guard + GUI badge + test |
| Unusual report descriptors (12-bit packed X/Y, multiple report IDs, vendor-defined mouse) | Medium | Table-driven parser plus a descriptor test corpus; log unknown layouts |
| Competitive-play rules | Low | Directional sensitivity scaling is in the same category as Raw Accel or on-mouse sensitivity; check the specific tournament's hardware rules |

---

## 8. Immediate Next Actions

1. ~~Install ESP-IDF v5.5.4; PC-link spike.~~ Done 2026-10-25 (toolchain at `~/esp/esp-idf`, `spikes/m0_phy_swap/`).
2. Connect the UART USB-C as well (flashing/console), then run the **host-half spike**: IDF `usb_host` on the HS port enumerating a real mouse (the Finalmouse ULX receiver or the Attack Shark R3 receiver in USB-A port 1) while the FS device side stays up. Print its report descriptor and input rate.
3. Find the USB-A VBUS enable (`DIO7003` / `AO3401`) and the BOOT GPIO from the schematic.
4. Test whether a chip reset restores the default PHY mapping (hold BOOT, tap RST with only the "other" USB-C connected, check for `303A:1001`).
5. On the CachyOS box, run `tools/capture_descriptors.sh` with the ULX and R3 plugged in and commit the descriptors to `tests/descriptors/` (feeds M1; once the dongle can dump descriptors itself, section 9.1 item 5, this is no longer needed).
6. **When a mouse is available, work through [section 9](#9-mouse-discovery-device-info-and-hardware-verification):** automatic discovery of every attached mouse, immediate vectoring with no setup, real CPI and polling rate for mice with a known protocol, and a generic fallback for the rest. Its checklist (9.2) is the acceptance test for M1.

---

## 9. Mouse Discovery, Device Info and Hardware Verification

*Requested 2026-10-25. Built and verified in the first session with real mice.*

**Goal:** plug in any mouse (or several) and the dongle finds them on its own and starts vectoring immediately, with no setup step. For mice with a known vendor protocol it reads, and can set, the real CPI and polling rate, the way the desktop app does with the OpenMouse protocol. For every other mouse it falls back to a generic mode that still works.

### 9.1 What gets built

1. **Discovery of every mouse, with hotplug.** The USB host enumerates every device on the port and parses each HID interface's report descriptor. It claims each interface that has a Generic Desktop / Mouse collection with relative X and Y, and ignores keyboards, consumer-control, vendor configuration and absolute-pointer (tablet) interfaces, in the same spirit as the desktop app's ranking in `src/devices.c` (a keyboard or macro pad that exposes an auxiliary mouse interface is not treated as the mouse). Several mice at once are all claimed: each report is scaled, then motion is summed into one output stream and the buttons are OR-ed together. Unplugging a mouse releases its buttons (no stuck clicks) and never re-enumerates the dongle toward the PC.
2. **Immediate start.** No configuration step: the first report after enumeration is scaled with the stored profile. Target (to be measured): first scaled report within 500 ms of attach, most of which is USB enumeration.
3. **Hubs.** With the HOST jumper only USB-A port 1 reaches the P4, so more than one mouse needs a hub in that port. Whether ESP-IDF v5.5.4's USB host supports hubs is unverified (see 9.3).
4. **Device information in three tiers.** The web page and the native app show a Mouse panel with the tier, name, VID:PID, polling rate, CPI and (where known) battery for each mouse.

   | Tier | Mice | What the dongle reports | Control |
   |:---|:---|:---|:---|
   | **1. Known protocol (OpenMouse)** | Finalmouse UltralightX `361D:0100` (CPI, polling 500 to 8000 Hz, battery, Motion Sync, RSSI, firmware). Attack Shark R3 receiver `3151:402D` (polling 125 to 8000 Hz through the GearHub 2.4G relay; CPI read/write is not implemented in the desktop app either) | Real values read from the mouse | Set CPI and polling from the web page or the app |
   | **2. Generic HID mouse** | Everything else that parses | Name and manufacturer from the USB string descriptors, VID:PID, nominal polling rate from the endpoint `bInterval` and bus speed, **measured** report rate, report layout (axis widths, buttons, wheel). CPI is unknown, so the user enters it once; it is kept per VID:PID in the dongle's flash and only labels the effective-DPI readouts | None |
   | **3. Unparseable descriptor** | Unusual or broken descriptors | Boot-protocol fallback (8-bit relative motion), with a visible warning | None |

   **Why the OpenMouse drivers must run on the dongle:** once the mouse is behind the dongle, the PC cannot reach it, so the desktop app's `src/hid_driver.c` and any browser-based configurator see only the dongle. The dongle is the USB host, so it speaks the vendor protocol itself and exposes the results through new protocol commands (`GET_MOUSES`, `GET_MOUSE_INFO`, `SET_MOUSE_POLLING`, `SET_MOUSE_CPI`). The protocol framing, checksums and decoding in `src/hid_driver.c` move into a transport-agnostic module under `common/`, with two transports: Linux hidraw (the desktop app) and the ESP USB host (the dongle). The R3's relay handshake takes seconds, so it runs as an asynchronous job that reports progress by event, never blocking the control channel.
5. **Descriptor capture.** A `GET_MOUSE_DESC` command streams a mouse's raw report descriptor over the vendor channel, so any mouse tried can be added to `tests/descriptors/` without needing the Linux machine.

### 9.1a Decisions (2026-10-25)

- **License: AGPL-3.0** for the whole repository, matching OpenMouse (`LICENSE`). That lets us use OpenMouse's TypeScript codecs and device catalog directly rather than rewriting them.
- **All three layers ship together:** universal information (A), known-vendor drivers (B) and the catalog of recognized names (C). Layer B has two routes that coexist:
  1. **Tunnel:** the dongle forwards raw reports to and from a mouse's vendor interface and the web page presents it as a virtual WebHID device, so OpenMouse's TypeScript codecs run unchanged in the browser. This covers every brand OpenMouse supports with no per-brand firmware.
  2. **Drivers in C on the dongle:** for standalone use with no PC software. The ULX and R3 first (the logic exists in `src/hid_driver.c`), then other popular families, each only after it is tested on hardware, following OpenMouse's own rule that a model counts as supported only once that exact product ID and connection path has been exercised.
- **Safety rule:** the dongle sends vendor commands only to devices in the registry, read-only on probe, and never writes a setting unless the user asks.
- **A way to measure CPI for any mouse:** the *Measure CPI* wizard (drag a known distance, counts divided by distance) is part of layer A.

### 9.2 Verification checklist (the acceptance test for M1)

Run it with at least: the Finalmouse ULX receiver, the Attack Shark R3 receiver, and one ordinary wired mouse that has no known protocol.

| # | Test | How | Pass when |
|:--:|:---|:---|:---|
| 1 | Find and identify | Plug each mouse in | Each is listed with the right VID:PID and name, and its tier; descriptor saved to `tests/descriptors/` |
| 2 | Start with no setup | Plug in, move, with a non-neutral profile saved | Output is scaled from the first report; attach-to-first-report time recorded |
| 3 | Count parity (bypass on) | Same long swipe set direct vs through the dongle | Total counts within 0.1% |
| 4 | Scaling effect (weights 0.5) | Same swipe set | Counts halve (within 1 count per second of motion) |
| 5 | Input rate | Move continuously at each mouse's polling rates (125, 500, 1000, 2000, 4000, 8000 Hz where supported) | Measured input rate within 2% of nominal; output holds 1000 Hz; no dropped reports |
| 6 | Buttons and wheel | All buttons, side buttons, wheel, tilt; 1000-click double-click test | Nothing lost or stuck |
| 7 | Latency | Dongle-internal timestamps, once with a logic analyzer | Median at most 1.0 ms, p99 at most 1.5 ms |
| 8 | Known-mouse info parity | Read CPI and polling through the dongle; compare with the desktop app on a direct connection (the ULX last read 3200 CPI, 2000 Hz on 2026-10-21) | Values match; setting polling through the dongle is confirmed on a direct connection afterward |
| 9 | Generic fallback | The ordinary wired mouse | Tier 2 info shown, measured rate plausible, CPI entry saved per VID:PID, nothing breaks |
| 10 | Several mice and a hub | Two mice at once, then through a hub | Both vectored, buttons merged, no interference |
| 11 | Hotplug | 50 unplug/replug cycles, plus unplug while a button is held | PC-side device never re-enumerates; no stuck buttons; recovers every time |
| 12 | Other USB devices | Plug in a keyboard and a headset | Ignored; no cursor effects |
| 13 | Power | A wireless receiver and a wired mouse with lighting, from USB-A port 1 | Both work; VBUS enable understood |
| 14 | Wireless sleep and wake | Let the mouse sleep, then wake it | Resumes vectoring with no action |
| 15 | PC side | Windows, macOS and Linux | Seen as an ordinary mouse; Studio and the native app both connect |

### 9.3 Open questions to settle first

- ~~Does ESP-IDF v5.5.4's USB host support hubs?~~ Yes: it is the `CONFIG_USB_HOST_HUBS_SUPPORTED` option, and a build with it enabled links and runs (section 9.4). Whether it works with a real hub and mice is still for test 10.
- Does a High-Speed host handle the ULX's 8 kHz receiver while the Full-Speed device side stays up? The ULX uses a single combined interface for motion and configuration (usage page `0xFF00`), so the dongle must claim it and send vendor reports on it while motion keeps flowing.
- The R3 receiver exposes three interfaces; only the first carries motion. Confirm which to claim, and that the configuration interfaces can be used for the relay at the same time.
- How the USB-A power switch (`DIO7003`/`AO3401`) is enabled.

### 9.4 Space and memory budget (measured 2026-10-25)

The question: is there room on the dongle for discovery, the device catalog, the tunnel and drivers? **Yes, with a wide margin.** Flash is not the constraint; what limits the work is the effort of porting drivers, not space.

**Measured**

| Item | Flash image | Free internal RAM (board, idle) |
|:---|---:|---:|
| Current firmware (control plane + PC-side USB), 0.3.0 | 283 KB (13.8% of a 2 MB OTA slot) | 559 KB |
| Same plus the USB host stack **with hub support** (a throwaway build run on the board) | 352 KB (17%): **+69 KB** | 553 KB: **-6 KB** |

The chip has 16 MB flash, 576 KB internal RAM (about 575 KB shown as free at boot) and 32 MB PSRAM. The current partition table uses 2 x 2 MB for the two OTA slots and leaves about 12 MB of flash unused. PSRAM is not enabled in the build; the hot path should stay in internal RAM anyway.

**Estimated** (not measured; our existing control plane is 7.7 KB of code for 12 commands, which is the yardstick)

| Item | Estimate |
|:---|---:|
| HID report-descriptor parser, discovery, rate measurement, CPI wizard, descriptor dump, tunnel | 20 to 40 KB |
| Device catalog (OpenMouse's 391 HID IDs: VID:PID, brand, model, flags) | about 10 to 15 KB |
| One C vendor driver (framing, checksum, a handful of commands) | about 2 to 8 KB each |
| **Every one of OpenMouse's 49 families ported to C, worst case** | 250 to 500 KB |

Adding all of it keeps the image around 0.7 to 1.0 MB, inside a 2 MB slot. Per-mouse RAM is small (descriptors of a few hundred bytes, 64-byte report buffers).

**Recommendations**
1. **Enlarge the OTA slots to 4 MB each now.** It costs nothing (the flash is there) and a partition layout cannot be changed by an over-the-air update later, only by a USB reflash. This is a one-line change in `partitions.csv`.
2. **Port only a few drivers to C.** The tunnel plus the browser-side codecs already cover the rest, which keeps firmware growth tiny.
3. **Keep PSRAM off** unless something needs it.

**Not yet measured:** RAM with real devices attached (an 8 kHz mouse streaming plus the hub), and CPU headroom at 8 kHz. Those belong to the checklist above (tests 5 and 10), not to this space question.
