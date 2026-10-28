# AnyMouse ESP32-P4 Dongle

A USB man-in-the-middle that runs the 8-octant vectoring in hardware. The mouse (or its 2.4 GHz receiver) plugs into the ESP32-P4's USB-A port. The P4 reads its HID reports, scales them with the same cosine-interpolated octant weights as `./anymouse`, and presents itself to the PC as an ordinary USB mouse. The profile lives in flash, and the dongle is configured over a **vendor HID channel on the same USB cable** from:

1. **A web page (WebHID)**: [AnyMouse Studio](../../web/README.md). Nothing to install; open it in Chrome, Edge or Brave and click Connect.
2. **The native `./anymouse` app**: it finds and pairs with the dongle by itself, and is the only front-end that can do live FpsAimForge auto-calibration (the game telemetry lives on the PC).

The dongle works standalone: once a profile is saved it vectors with no software on the PC. There is no Wi-Fi, no web server on the dongle and no USB networking (dropped by decision, 2026-10-25). Because the PC only ever sees a standard HID mouse, no OS input-interception driver is needed on Windows.

This README describes the dongle **as built**. What is still to do is in [PLAN.md](PLAN.md); the dated history is in [../../PROGRESS.md](../../PROGRESS.md).

## Status (2026-10-27, firmware 0.7.0)

- **Working with real mice** (Razer Basilisk Ultimate wired, Logitech G Pro "PRO X Wireless" through its Lightspeed receiver): the PC-facing link, the High-Speed host, per-report scaling, 1 kHz output, flash profiles, the vendor protocol and the Studio. The M0 and M1 exit tests (stability run, PHY reset, the hardware checklist) were signed off by the project owner on 2026-10-27; the only measured figures on file are those in the log entries of PROGRESS.md.
- **Mouse information** in three tiers (known protocol, generic HID, boot fallback) plus a catalog of 810 OpenMouse device ids, so every mouse OpenMouse knows is named. Drivers: Razer (read, CPI/rate writes verified on the Basilisk Ultimate), Finalmouse ULX and Attack Shark R3 GearHub (read only), Logitech HID++ 2.0 (read; CPI and rate writes on the G Pro receiver).
- **New in 0.7.0: OpenMouse runs on the dongle unchanged.** OpenMouse's own TypeScript drivers (every family) run in the QuickJS-ng engine (`components/amv_js`) instead of C ports; see [OpenMouse engine](#openmouse-engine-firmware-070). Flashed and booting; **not yet tried with a real mouse** (only against a fake Razer on the host). The C ports remain behind `CONFIG_AMV_OPENMOUSE_JS=n`.
- **Not built, by decision:** firmware update over the vendor channel (DFU), app rollback, watchdog and soak testing (the M5 milestone was closed on 2026-10-27). Reflash over the UART USB-C. The two 4 MB OTA slots stay in the partition table so DFU can be added later without a layout change.
- **Open:** calibration through the dongle, the Linux-side checks, the rest of OpenMouse coverage and more. See [PLAN.md](PLAN.md).

## Documents
| File | Purpose |
|:---|:---|
| [PLAN.md](PLAN.md) | Open work only: Linux and calibration items, OpenMouse coverage, mouse-side experiments, Windows, stretch goals, risks |
| [spikes/m0_phy_swap/README.md](spikes/m0_phy_swap/README.md) | The M0 proof of concept (build and check steps) |
| [../../README.md](../../README.md#13-esp32-p4-hardware-dongle-phase-4) | Project-level overview of the dongle |
| [../../web/README.md](../../web/README.md) | AnyMouse Studio |
| [../../common/amv_dongle_proto.h](../../common/amv_dongle_proto.h) | The vendor-HID wire protocol (the reference), shared with the host tools |
| [../../common/amv_octant.h](../../common/amv_octant.h) | The octant scaling math shared by firmware and tests |
| [../../PROGRESS.md](../../PROGRESS.md) | Dated log and roadmap checklist |

## Cabling
The UART USB-C to the development computer (flashing and console), the "other" USB-C to the PC under test, and the mouse (or its receiver) in the **lower USB-A socket next to the Ethernet jack** with **both jumper caps on HOST** (that is the direct port). With the caps on DEVICE the P4 talks to the onboard hub and the other three sockets instead (the hub enumerates; a mouse behind it is untested). USB-A VBUS is always on, so a mouse powers up in any socket; only the data pair is switched.

## Verified hardware

Board: Waveshare **ESP32-P4-Module-DEV-KIT** (SKU -A/-B/-C not identified). Probed with `esptool` 4.12.0, the boot log, the eFuse summary, `ioreg` and the schematic.

| Item | Value | Consequence |
|:---|:---|:---|
| Chip | ESP32-P4 **rev v1.3** (pre-v3 silicon) | Build with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`; CPU max **360 MHz** |
| CPU | 2 x RISC-V HP cores, single-precision FPU | `atan2f`/`cosf` per report is cheap |
| Flash / PSRAM | 16 MB GigaDevice QIO 80 MHz / 32 MB in package | PSRAM unused; the hot path stays in internal SRAM |
| eFuses | No secure boot, no flash encryption, download mode on, JTAG on | A bad image can always be replaced over UART. **No eFuse has been burned.** |

| Port | Wired to | Used as |
|:---|:---|:---|
| USB-C, **UART** | CH343 bridge `1A86:55D3` | Flashing and console; always works, also the recovery path |
| USB-C, **the other one** | P4 Full-Speed OTG after a **software PHY swap** (by default it is USB Serial/JTAG `303A:1001`) | **The PC link.** It cannot flash or show a console while the firmware runs |
| USB-A, two stacked double-decker sockets | P4 High-Speed OTG through an FSUSB42 analog mux `U15` (SEL: H selects HSD2, L selects HSD1) | **Mouse side (HS host).** Jumper on HOST = the direct port; on DEVICE = the CH334 hub for the other three sockets. One controller feeds both stacks, so they are mutually exclusive |
| RJ45, 40-pin header | RMII Ethernet; `GPIO26/27` (OTG_FS pins) appear on the header net list | Unused; the header is a fallback for the PC link only |

- **PC link:** `usb_wrap_ll_phy_select(&USB_WRAP, 0)` before `tinyusb_driver_install()` on the Full-Speed port moves OTG_FS onto the "other" USB-C (spike `spikes/m0_phy_swap/`: enumerates as `303A:4D56`, HID mouse plus vendor HID on usage page `0xFF4D`, 20/20 vendor echoes at about 5 ms RTT). Whether a reset restores the default mapping so the "other" USB-C can flash again was part of the M0/M1 sign-off, but its result was not recorded: treat the UART USB-C as the only flashing path.
- **USB-A VBUS:** a `DIO7003HCST5` switch (U6) with EN pulled to 5 V, so VBUS is always on and there is no GPIO to enable (`AMV_USB_VBUS_EN_GPIO` is -1). A dark mouse means supply or cable, not firmware.
- **BOOT button = GPIO35**; RESET is `ESP_EN`.
- **Hard limit:** a Full-Speed interrupt endpoint polls at most every 1 ms, so **output to the PC is capped at 1000 Hz**. Input is read at the mouse's native rate; each native report is scaled on its own and the scaled deltas and the sub-pixel remainder are summed into the next 1 ms frame, so no motion is lost. FpsAimForge samples at 1 kHz, so calibration quality is unaffected.
- **Polarity note:** the first reading of the schematic had the jumper backwards; Waveshare's FAQ and the measurements agree with HOST = direct.

## How it works

```
            ┌───────────────────────── ESP32-P4 (360 MHz × 2) ────────────────────────┐
 Mouse /    │  HS OTG (USB Host, USB-A)                    FS OTG (USB Device, TinyUSB) │   PC
 receiver ──┼─▶ enum + HID report-descriptor parse           ├─ IF0 Mouse (16-bit X/Y, ─┼──▶ any OS, no driver
 (any)      │   interrupt IN @ native rate                   │   16 buttons, wheel, pan)│
            │        │                                       └─ IF1 Vendor HID (64 B) ◀─┼──▶ Studio (WebHID)
            │        ▼   core 0, highest priority                     ▲                 │    and/or ./anymouse
            │   amv_vector: per-report cosine C¹ scaling + remainder ──┘ 1 ms frames    │
            │        ▲  weights (seqlock)                                               │
            │   amv_store (NVS profiles) ◀──▶ amv_ctrl (single owner of state, gen counter)
            │                                      ▲                                    │
            │   core 1:  amv_proto (vendor HID commands)  +  driver / setup task        │
            └───────────────────────────────────────────────────────────────────────────┘
```

**Hot path (core 0).** The USB host client task receives each interrupt IN report and resubmits it at once. The report is decoded with the layout parsed from the mouse's descriptor at enumeration (report id, bit offsets, sizes and signedness for X, Y, wheel, AC Pan and buttons). `amv_octant_scale()` is the same math as `scale_vector()` in `src/evdev_rt.c` (atan2, octant position, cosine smoothstep, weight blend, remainder diffusion). Scaled deltas go into an accumulator under a `portMUX` spinlock, and the PC-side `tud_hid_report_complete_cb` sends the next frame as soon as the previous one completes. **Button edges are never merged**: a second change before a frame goes out is queued for the next frame. Deltas beyond int16 are carried over, not clamped. Several mice are all claimed: motion is summed and buttons OR-ed; unplugging one releases its buttons and never re-enumerates the dongle toward the PC.

**Control plane.** `amv_ctrl` owns the state (weights, 4 profile slots, bypass flag, CPI hint). Every writer (vendor HID, the BOOT button) goes through it; each change bumps a `weights_gen` counter, notifies every attached client with an async `EVENT_STATE`, and triggers a debounced NVS save (2 s). Weights reach the hot path through a **seqlock**: the writer bumps a sequence counter to odd, writes the eight weights and the bypass flag, bumps it to even, and the reader retries on an odd or changed counter, so it never blocks and never sees a half-written set. Weights are validated with the same rule as the Linux engine: 0.01 <= w <= 5.0, no NaN; bad input is rejected, not clamped. Holding BOOT for 1 s toggles **bypass** (all weights 1.0, profile untouched).

**Ownership between dongle and clients.** The dongle is the source of truth. A client reads weights and the generation on connect and adopts them; an edit carries the generation it was based on, and a stale edit gets a NACK with the current weights. A change from one client reaches the other as an event. Conflicts resolve last-writer-wins by generation. Linux allows several processes on one hidraw node, but Windows HID access is typically exclusive, so the Studio shows a "device in use by another app" message instead of failing silently.

| | Studio (WebHID) | `./anymouse` |
|:---|:---:|:---:|
| Edit weights, presets, profiles, bypass | yes | yes |
| Mouse information and settings | yes | CPI and polling (full settings list: see PLAN.md) |
| Live FpsAimForge auto-calibration | no (game telemetry is in `/dev/shm` on the PC) | yes |
| Needs install | no (Chromium only) | yes |
| Operating systems | Windows, macOS, Linux | Linux now, Windows later |

## Mouse discovery and information

**Discovery.** The USB host enumerates every device (hubs are supported through `CONFIG_USB_HOST_HUBS_SUPPORTED`; a real hub test was not recorded), parses each HID interface's report descriptor and claims every interface with a Generic Desktop / Mouse collection with relative X and Y. Keyboards, consumer control, vendor interfaces and absolute pointers are not vectored. No setup step: the first report after enumeration is scaled with the stored profile.

**Three tiers.** Every device is reported in one of:

| Tier | Mice | What the dongle reports | Control |
|:---|:---|:---|:---|
| **1. Known protocol (OpenMouse)** | Razer, Finalmouse ULX, Attack Shark R3 receiver, Logitech HID++ | Real CPI, polling rate, battery, firmware, name, link state (wired / connected / asleep) | CPI and polling (and LOD or Motion Sync where ported) for models that pass the write policy |
| **2. Generic HID** | Every other mouse that parses | USB name, VID:PID, nominal rate from `bInterval`, **measured** report rate, layout; a CPI the user enters once (kept per VID:PID in flash, only labels the effective-DPI readouts) | None |
| **3. Boot fallback** | An unusable descriptor on a boot-subclass interface | 3-button 8-bit motion with a visible warning | None |

Ignored devices are listed with a reason (not a mouse, absolute pointer, unparseable, no slot, claim failed). A raw report descriptor can be downloaded from the Studio (`GET_MOUSE_DESC`) and saved into `tests/descriptors/`.

**Full OpenMouse coverage (firmware 0.6.0, [OpenMouse](https://github.com/OpenMouse-Project/mouse-protocol) commit `326f292`, AGPL-3.0).**
- **Catalog:** `components/amv_catalog/amv_catalog.generated.h` is generated by `tools/openmouse/gen_catalog.sh`, which runs OpenMouse's own `SUPPORTED_HID_FILTERS` and registry `isSupported()` checks against synthetic devices. It holds 810 rows (716 named, 17 vendor-wide, 37 brands, 74 engines) and stamps the upstream commit. 53 of the 545 upstream filters matched no engine and are listed with engine "None". `amv_catalog_find()` looks up an exact id, then the vendor-wide row. A catalogued mouse with no driver is reported as **known model, driver not ported**.
- **Vendor interfaces:** a driver's `cfg_match` selects extra interfaces (e.g. the Logitech HID++ one). The host claims them, keeps interrupt IN reads in flight (routed to the driver, never to the vectoring) and sends output reports through an interrupt OUT endpoint or SET_REPORT, as WebHID does.
- **`amv_hl_*` shim:** WebHID's conventions (report id passed separately from the data; `send_report`, `send_feature`, `receive_feature`, `wait_input`, `flush_input`), so each C port reads line by line like its TypeScript source.
- **Settings protocol** (additive, still v1): `GET_MOUSE_MODEL`, `GET_MOUSE_SETTINGS`, `GET_MOUSE_OPTIONS`, `SET_MOUSE_SETTING`. Setting ids: CPI, POLLING, LOD, MOTION_SYNC, BATTERY, CHARGING, LINK; option kinds LIST and RANGE; units HZ, CPI, MM10, LEVEL, BOOL. The Studio renders whatever the mouse offers without knowing the brand. `SET_MOUSE_CPI` and `SET_MOUSE_POLLING` still work.
- **Receivers and sleep:** a driver that found no awake mouse is probed again every 5 s and the link state is reported. A mouse behind a receiver goes to sleep when idle (HID++ 1.0 errors or silence); moving it wakes it.
- **Logitech (`drv_logitech.c`):** HID++ 2.0 over the receiver's vendor interface: device-index discovery, name (0x0005), DPI (0x2201 and 0x2202), report rate (0x8060 and 0x8061), battery (0x1004 and 0x1000) and onboard profiles (0x8100, host control mode). The G Pro "PRO X Wireless" behind receiver `046d:c547` reports DPI 0x2201 (legacy), rate 0x8060 (legacy), CPI 100..25600 step 50 and rates 125/250/500/1000; it read 800 CPI, 1000 Hz and the battery level correctly in the Studio. A write sequence follows OpenMouse: check the DPI list, switch the mouse to host control (0x8100) if needed, write, read back. **Host control is a persistent change on the mouse**, as in OpenMouse.

### OpenMouse engine (firmware 0.7.0)
Instead of porting OpenMouse to C family by family, the dongle runs OpenMouse's driver registry itself. Feasibility was checked first: 185 of OpenMouse's 188 runnable test files pass unchanged under QuickJS-ng on a PC (about 2,570 tests; the few failures are test-only Node APIs and one `toLocaleString` message difference).
- **Engine:** QuickJS-ng 0.17.0, vendored unmodified in `components/amv_js/quickjs-ng/` (MIT). One task on core 1 (48 KB internal stack), heap in PSRAM through a counting allocator. Calls are serialized like the C drivers; between calls nothing runs in JS and input reports wait in the host's queues.
- **Bundle:** `tools/openmouse/gen_js_bundle.sh` bundles the bridge (`components/amv_js/js/`) with OpenMouse's registry (esbuild) and compiles it to QuickJS bytecode, `components/amv_js/generated/openmouse.bc`, embedded in the image. Bytecode only loads into the same engine version, so the script checks `qjsc` matches the vendored one.
- **Bridge (`js/`):** `prelude.js` supplies the browser globals the drivers use (timers driven by the C loop, `console`, `crypto.getRandomValues`, `structuredClone`, `localStorage`, `TextDecoder`/`TextEncoder`). `hid_device.js` is a WebHID `HIDDevice` per HID interface: collections parsed from the report descriptor, feature reports returned with the report ID at byte 0 as Chrome does, input reports as `inputreport` events. `bridge.js` exposes `navigator.hid.getDevices()` (all interfaces of the attached mice), picks the driver with OpenMouse's own `clientSupportScore`/`createSupportedClient`, and flattens `MouseStatus` for C.
- **Driver (`amv_usb_host/drv_openmouse.c`):** matches any id in the catalog with an engine, claims every other HID interface (up to 4), maps the mouse's own interface to SET/GET_REPORT (x = -1) and claimed interfaces to `amv_hl_*`, and fills the usual context (CPI, rate, rates, battery, charging, link, LOD as levels 1-3, Motion Sync). Reported as driver `OPENMOUSE` (5).
- **Measured on the board (no mouse yet):** bundle loads at boot in 1.2 s; engine heap 4.34 MB in PSRAM; 4.3 KB of the task stack used; internal RAM free 463 KB; image 2.28 MB (engine 0.62 MB code, bytecode 1.18 MB).

**Write policy.** Probing and the 5 s refresh are read-only for every driver. A setting is written only when allowed: today the Razer Basilisk Ultimate wired (`1532:0086`) and anything behind the Logitech receiver `046d:c547` (the G Pro). The decided rule for adding models is that OpenMouse marks the exact model `verified: true` **and** its C port passes the transcript tests; that harness is not built yet (see PLAN.md), so nothing new qualifies until it is. Finalmouse and GearHub stay read-only until then.

## Protocol

The authority is [`common/amv_dongle_proto.h`](../../common/amv_dongle_proto.h): 64-byte frames, an 8-byte header `{magic 'A' 'M', proto_ver, cmd, seq, status, len, reserved}` plus up to 56 bytes of payload, protocol version 1. Replies echo `cmd` and `seq` and carry a status. Commands: `GET_INFO`, `GET_WEIGHTS`, `SET_WEIGHTS` (with `base_gen`), `SAVE_PROFILE`, `LOAD_PROFILE`, `SET_BYPASS`, `GET_STATS`, `REBOOT`, `FACTORY_RESET`, `GET_SLOTS`, `HELLO`, `GET_MOUSE`, `GET_MOUSE_CAPS`, `GET_MOUSE_DESC`, `SET_MOUSE_CPI`, `SET_MOUSE_POLLING`, `GET_MOUSE_MODEL`, `GET_MOUSE_SETTINGS`, `GET_MOUSE_OPTIONS`, `SET_MOUSE_SETTING`, and the debug-only `TEST_SCALE` and `TEST_SELF_CHANGE`. Async events: `EVENT_STATE` (tagged with its source) and `EVENT_MOUSE`. `STREAM_DIR` and `PUSH_CALIB` are not implemented (PLAN.md).

**Identity is proven by the protocol, never by USB VID:PID.** The `0xFF4D` vendor collection only makes an interface a *candidate*; the host then sends `HELLO` with a random 8-byte nonce and the dongle answers with the identity word `AMVD`, the same nonce, protocol and firmware versions, capability bits (`AMV_CAP_VECTORED_MOUSE`, `AMV_CAP_MOUSE_HOST`) and its MAC. A different protocol version is reported as incompatible. Firmware older than `HELLO` (0.3.x) is recognised by a well-formed `GET_INFO`. The vendor interface sits on usage page `0xFF4D`, distinct from the Finalmouse `0xFF00` page so `src/hid_driver.c` never confuses them.

**USB identity:** Espressif VID `0x303A` with the placeholder PID `0x4D56`, product "AnyMouse Vectoring Dongle", serial from the P4 MAC. The board's own Serial/JTAG uses `303A:1001`, so the dongle's PID must differ. KDE keys per-device pointer settings on VID:PID: set the dongle to the Flat profile once; on Windows turn off "Enhance pointer precision".

**Double-scaling guard (Linux app).** The mouse interface on the same USB device as a dongle candidate is flagged `is_dongle` and never auto-grabbed, and the GUI shows "Vectoring: Dongle". It fails safe: an unverified, silent, unopenable or incompatible candidate still guards its mouse. Explicitly choosing the dongle's mouse is refused with a message.

## Components
| Component | Role |
|:---|:---|
| `amv_common` | Header-only link to the repo-level `common/` (scaling math, wire protocol) |
| `amv_store`, `amv_ctrl` | Flash profiles (CRC32, 4 slots) and the state owner (weights, generation counter, lock-free hot-path read, mouse count) |
| `amv_proto` | Vendor-HID command handler task |
| `amv_usb_device` | PC side: Full-Speed device on the non-UART USB-C (PHY swap); 16-button / 16-bit mouse report (3-byte boot fallback) plus the vendor channel |
| `amv_catalog` | Generated catalog of OpenMouse's supported devices; exact and vendor-wide lookup |
| `amv_hid_parse` | Pure-C report-descriptor parser (also built on the host for tests) |
| `amv_usb_host` | Mouse side: High-Speed host; enumeration, interface claims, interrupt transfers, tier decision, `amv_hl_*` shim, the `GET_MOUSE*` / `SET_MOUSE_*` handlers, and the drivers: `drv_openmouse.c` (default, runs OpenMouse in `amv_js`) or the C ports `drv_razer.c`, `drv_finalmouse.c`, `drv_gearhub.c`, `drv_logitech.c` (`CONFIG_AMV_OPENMOUSE_JS=n`) |
| `amv_js` | QuickJS-ng engine running OpenMouse's own drivers from embedded bytecode (`generated/openmouse.bc`); the bridge in `js/` |
| `amv_vector` | Hot path: scales every native report, sums into 1 ms frames, merges several mice, never merges button edges |

`tools/`: `dongle_test.py` (68 board checks), `gen_proto_js.py` (generates `web/proto.js`), `capture_fixtures.py` (records frames for the web tests), `openmouse/` (catalog and JS bundle generators). `tests/`: `test_octant.c`, `test_hid_parse.c`, `js_bridge/` (the bridge against a fake Razer, run with `qjs`), `descriptors/` (real captured descriptors). `build/`, `managed_components/` and `sdkconfig` are git-ignored.

## Toolchain and quickstart
**ESP-IDF v5.5.4, pinned** (it supports pre-v3 P4 silicon; never update IDF mid-milestone). Managed component: `espressif/esp_tinyusb`; the host stack is IDF's built-in `usb_host`, and HID handling is ours because we need the raw descriptor. Key `sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32p4"`, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, 16 MB flash, 360 MHz, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_FREERTOS_HZ=1000`. Partitions: `nvs` 24 KB, `otadata`, `ota_0` and `ota_1` 4 MB each, `coredump` 64 KB. PSRAM is enabled for the engine heap (`CONFIG_SPIRAM=y`, caps allocation only, so `malloc()` stays internal). The image is about 2.3 MB (0.4 MB without the engine).

Install (macOS and CachyOS alike): `brew install cmake ninja dfu-util ccache python@3.12`, clone `espressif/esp-idf` at `v5.5.4` into `~/esp/esp-idf`, run `install.sh esp32p4`.
```bash
. ~/esp/esp-idf/export.sh
cd firmware/esp32p4-dongle
idf.py set-target esp32p4        # first time only
idf.py build
idf.py -p <UART USB-C port> flash monitor     # macOS: /dev/cu.usbmodem<serial>; Linux: /dev/ttyACM0 or /dev/ttyUSB0
```
**Always flash and monitor through the UART USB-C.** While the firmware runs, the other USB-C is the PC link and cannot flash. Download mode is not fused off (hold BOOT, tap RST), so a bad image can always be replaced. Regenerating the catalog needs Node and an OpenMouse checkout: `OPENMOUSE_DIR=<checkout> tools/openmouse/gen_catalog.sh`. Regenerating the engine bundle also needs `qjsc` 0.17.0 (`brew install quickjs-ng`): `OPENMOUSE_DIR=<checkout> tools/openmouse/gen_js_bundle.sh`.

The console prints every 5 s: input reports, frames out, raw vs out counts, and per mouse VID:PID, report count and errors. IDF logs one harmless `USB HOST: Enqueue URB error: ESP_ERR_INVALID_STATE` when a mouse disappears while a read is being re-queued.

## Bring-up switches and diagnostics
- **`-DAMV_HOST_ONLY`** (`idf.py -DCMAKE_C_FLAGS=-DAMV_HOST_ONLY build`): leaves the PC-side USB device off, to test the host alone. Clean `build/` afterwards, because CMake remembers the flag.
- **Host port register:** the 5 s status line and a 20 Hz change logger print the High-Speed controller's `HPRT` (connected, enabled, power, D-/D+ line state, speed). Line state `00` with nothing plugged in means no device pulls the data pair up; a connect shows `conn=1` and the speed (0 high, 1 full, 2 low).
- **Debug logs for the host stack:** connect and enumeration events appear only at debug level; set `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` and `esp_log_level_set("*", ESP_LOG_DEBUG)` in `amv_usb_host_start()` for a session, then revert.
- **Saving a mouse's report descriptor:** the console prints each HID interface's descriptor as hex; use the Studio's *Download descriptor* button or paste it into `tests/descriptors/<vendor>_<model>_<vid>_<pid>_if<N>.hex` and add a case to `tests/test_hid_parse.c`.
- **Logitech write logging:** every write step is logged at INFO with the HID++ error codes, and the boot log prints the feature indexes and the CPI range.

## Tests
With both USB-C cables connected and the firmware flashed:
```bash
pip install hidapi
python3 -I firmware/esp32p4-dongle/tools/dongle_test.py          # 68 checks over the vendor-HID channel
cc -std=c11 -Wall -Wextra -Icommon firmware/esp32p4-dongle/tests/test_octant.c -lm -o /tmp/test_octant && /tmp/test_octant   # from the repo root
(cd firmware/esp32p4-dongle && cc -std=c11 -Wall -Wextra -Icomponents/amv_hid_parse/include tests/test_hid_parse.c components/amv_hid_parse/amv_hid_parse.c -o /tmp/test_hid_parse && /tmp/test_hid_parse)   # descriptor parser, 30 checks
(cd web && node --test tests/*.test.mjs)                          # Studio, 23 tests
(cd firmware/esp32p4-dongle && qjs --std --module tests/js_bridge/razer_fake.test.js)   # OpenMouse bridge vs a fake Basilisk Ultimate, 11 checks
make test-dongle                                                  # Linux only: 61 link and guard checks against a fake dongle
```
`dongle_test.py` covers the `HELLO` handshake, host-to-device writes (valid, invalid, stale), device-to-host events, bypass, profile slots, factory reset, the real scaling code against an independent float64 reference, persistence across a reboot, and the mouse list, descriptor and refusal paths with a Razer attached. It leaves the dongle at factory defaults. Its last full run was on firmware 0.5.0; **the four 0.6.0 commands (`GET_MOUSE_MODEL`, `GET_MOUSE_SETTINGS`, `GET_MOUSE_OPTIONS`, `SET_MOUSE_SETTING`) have no checks yet** (PLAN.md).

**Debug commands:** `TEST_SCALE` and `TEST_SELF_CHANGE` exist only for this testing; they do not move your cursor and should be gated out of release builds (PLAN.md, release housekeeping).

## Measured
- **Mouse side (Razer Basilisk Ultimate, wired, full speed, 1 ms interval):** enumerates in under 0.3 s; about 1000 reports/s in and 1000 frames/s out; 0 USB errors; `raw` equals `out` exactly at neutral weights; cursor, buttons and wheel work on the Mac; vectors set from the Studio take effect. Its interface 0 is the mouse (16-bit X/Y, wheel, 5 buttons, vendor bytes skipped); interfaces 1 and 2 are macro-key keyboards and are left alone, which is why the claim is per interface, not per device.
- **Control plane:** round trip about 2 ms median from a macOS host; 282 scaling steps matched an independent reference exactly (drift 0).
- **Writes through the dongle:** Razer CPI write about 85 ms; Logitech CPI writes (800 to 1000 and back) were read back correctly; Logitech report-rate writes (500, then 1000 Hz) returned `ESP_OK` and motion kept flowing, but only while the mouse was awake (see PLAN.md for the sleeping-mouse message still to do).
- **Space:** the control plane plus PC-side USB was 283 KB; adding the USB host stack with hubs added 69 KB and cost 6 KB of free internal RAM (559 to 553 KB). The 0.6.0 image with the catalog and four drivers is about 0.4 MB of a 4 MB slot, so porting more OpenMouse families is limited by effort, not space (estimate: 0.7 to 1.0 MB with every family).
- **OpenMouse engine (0.7.0, no mouse attached):** load at boot 1.2 s; heap 4.34 MB in PSRAM (6.6 MB on a 64-bit PC); task stack 4.3 KB used of 48 KB; internal RAM free 463 KB (was 553 KB); idle for over 5 minutes with no errors. Per family loaded alone (PC, scaled to 32-bit by the measured 0.66): engine baseline about 90 KB, median family about 130 KB, Logitech about 640 KB; bytecode median 18 KB, largest 130 KB.
