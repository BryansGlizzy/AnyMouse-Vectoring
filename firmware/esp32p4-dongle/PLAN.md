# AnyMouse ESP32-P4 Dongle: Open Work

The dongle is a USB man-in-the-middle that runs the 8-octant vectoring in hardware: the mouse plugs into the ESP32-P4, the P4 scales each report with the same cosine-interpolated octant weights as `./anymouse`, and the PC sees an ordinary USB mouse. It is configured over a vendor HID channel on the same cable, from the WebHID Studio ([`web/`](../../web/README.md)) or from `./anymouse`.

**This file lists only what is still to do.** How the dongle is built (verified hardware, architecture, protocol, mouse tiers, OpenMouse coverage so far, tests, measurements) is in [README.md](README.md). The dated history is in [../../PROGRESS.md](../../PROGRESS.md). Cleaned up on 2026-10-27: completed work and the design notes behind it moved to those two files.

---

## 1. Where things stand (2026-10-27)

| Milestone | State | Open work |
|:---|:---|:---|
| M0 PC-facing link, M1 passthrough | **Done.** Signed off by the project owner on 2026-10-27 | None |
| M2 vectoring and persistence | Done except one item | §2.1 |
| M3 vendor protocol and Linux engine | Firmware and Linux client built and tested against fakes | §2.2 and §3.2 |
| M4 Web Studio | Built; used in Chrome on macOS | §2.3 |
| M5 hardening and DFU | **Closed on 2026-10-27. Not implemented**: no DFU, rollback self-test, watchdog, brown-out or soak run. Flash over the UART USB-C | None. The 4 MB OTA slots stay so DFU can be added later |
| M6 stretch goals | Open | §7 |
| Teensy 4.1 port | **Bridge running 2026-11-04, 8 kHz both sides; vendor channel & profile store built (0.1.0), hardware verification pending** ([`../teensy41-dongle/`](../teensy41-dongle/README.md): 0xFF4D vendor HID, Studio & ./anymouse support, 4 EEPROM slots + boot restore, Finalmouse polling-rate setting; hardware checklist in README). Open: 16-bit output, button-edge merging, multiple mice, OpenMouse per-family loading | §4.0 item 4 |
| Section 10, full OpenMouse coverage | **Direction changed 2026-10-27:** OpenMouse's own drivers run on the dongle in QuickJS-ng (firmware 0.7.0, flashed, loads at boot); real-mouse test pending | §4.0 |

---

## 2. Needs the Linux box (CachyOS)

### 2.1 M2: the Linux engine on the shared math
- Move `scale_vector()` from `src/evdev_rt.c` into use of `common/amv_octant.h` (already shared with the firmware), behaviour-identical.
- Add `make test-common`: a fixed set of `(dx, dy, weights) -> (out, remainder)` golden vectors checked by both the Linux build and the firmware unit test (`tests/test_octant.c`), so the two cannot drift. `Makefile` also needs `-Icommon`.
- **Exit:** golden vectors pass on both targets.
- Unrelated drift noticed earlier and still true: README section 7 says the UDP weight channel was removed, but `src/evdev_rt.c` still listens on UDP 42069 (`UDP_PORT` in `src/core.h`). The dongle sync does not depend on it.

### 2.2 The dongle link against the real board
The Linux client (`src/dongle.c`, the guard, CLI, GUI badge and, since 2026-11-04, the slot panel) was built and tested against a fake dongle (`make test-dongle`, 104 checks). On 2026-11-04 the daemon linked to the real board as root and the GUI showed "Vectoring: Dongle"; as a normal user every open failed with *permission denied* because no udev rule was installed (the dongle README, Linux permissions, has the rule). Still to run against the board: `--dongle-slot load|save`, the GUI slot panel and bypass switch, and the udev rule on this machine.
- Run `./anymouse --dongle-status`, the GUI and the double-scaling guard with the real dongle attached; confirm the GUI shows "Vectoring: Dongle" and the dongle's mouse is never auto-grabbed.
- `./anymouse --install-udev` (hidraw `uaccess` rule matching the `AnyMouse` manufacturer string): try it on a real udev system.
- `./anymouse` UI for the settings list and catalog model names (the CPI and polling controls already route through the dongle; the Studio already shows the full list).
- Pulling the dongle mid-session must fall back cleanly.

### 2.3 Studio across platforms and hosting
- Chrome on Linux with the udev rule, and Chrome on Windows: every control works with no app installed. Opening the Studio while the native app holds the device on Windows must produce the clear "in use" message.
- Host `web/` on a static HTTPS site (e.g. GitHub Pages; WebHID needs a secure context) and cache it as a PWA. The native app could also serve the same files on `localhost`.
- Firefox, Safari and Flatpak/Snap browsers cannot use WebHID; the page already detects and says so.

---

## 3. Calibration

### 3.1 Multi-round convergence with the current engine
Run a full multi-round session in FpsAimForge's `AnyMouse 8-Octant Center-Out`, watch the per-octant median landing ratios r̃_k across rounds (they were within noise of 1.0 by round 8 to 12 on 2026-10-22), and fine-tune the submovement thresholds and the learning rate α (0.35, +/-5% clamp per round) if needed. Also run `AnyMouse 8-Octant Center-Out Ghost` and `AnyMouse 8-Octant Wide Flick` with auto-adapt off. Review `logs/flicks.csv`.

### 3.2 Calibration with the dongle in the loop (M3 remainder)
The weights already travel through the dongle: calibration writes `g_state.octant_weights` and the 50 Hz sync thread in `src/dongle.c` pushes changes with `SET_WEIGHTS`. What is untested is the whole loop on hardware.
- **Exit:** a full `AnyMouse Calibration Suite` session with auto-adapt on converges as it does with software vectoring (r̃_k within noise of 1.0 by round 8 to 12); weights on both sides match by generation number after the session; unplugging the dongle mid-session falls back cleanly.
- `STREAM_DIR` and `PUSH_CALIB` are not implemented; only the Studio would use them, so they are tracked in §9.

---

## 4. Full OpenMouse coverage on the dongle (section 10)

*Goal (2026-10-26): the dongle recognises every mouse the OpenMouse protocol library covers and, for each, reads (and where safe, sets) DPI, polling rate, battery, lift-off distance (LOD) and Motion Sync, as far as that mouse's protocol offers them. Lighting, button remapping, macros, onboard profiles, firmware update and the rest of OpenMouse are out of scope.*

### 4.0 OpenMouse engine on the dongle (decided 2026-10-27, user)
Porting TypeScript to C family by family needed hardware tests for every port. Instead the dongle now runs OpenMouse's driver registry **unchanged** in QuickJS-ng (README, [OpenMouse engine](README.md#openmouse-engine-firmware-070)). This replaces the C ports (kept behind `CONFIG_AMV_OPENMOUSE_JS=n`) and makes the transcript harness below unnecessary: OpenMouse's own test suite runs under the same engine (185 of 188 runnable files pass on a PC). Phases C, D and E reduce to re-running `tools/openmouse/gen_js_bundle.sh` on an OpenMouse update.

**Found and fixed 2026-11-04, first real run with a Finalmouse UltralightX dongle (`361d:0100`, High-Speed, `bInterval` 1):** the OpenMouse Finalmouse driver was selected but `readStatus` failed after its full 1 s wait, so the mouse stayed tier 2 and the Studio and desktop app showed no battery, Motion Sync or real polling rate (the "8000 Hz" was only the endpoint interval). Cause: the ULX's interface has an **interrupt OUT endpoint (0x01, 64 bytes)** and ignores the control-pipe `SET_REPORT` that `amv_host_set_report` always used for a mouse's own interface; Linux `hidraw write()` and Windows `WriteFile` send on the OUT endpoint when one exists. It was not the move from the C ports (`tests/js_bridge/finalmouse_fake.test.mjs` runs the real bundle against a fake ULX with the real descriptor and passes) and not input routing (the console's `reports=0` showed nothing came back). **Fix (built and flashed to the board):** output reports on a mouse's own interface now go to its OUT endpoint when it has one, with `SET_REPORT` as the fallback, and the boot log names each interface's endpoints. Result on the board: `readStatus ok in ~120 ms`, `"Finalmouse UltralightX": 1600 CPI, 2000 Hz, battery 29%, firmware "Mouse 8.0.3-77b"`, refreshed every 5 s with 0 USB errors; `./anymouse --dongle-status` shows the same plus Motion Sync on and link connected. Still open from this:
- **Retry of a failed first probe** (built, **never exercised**: the probe now succeeds first time). `refresh_known()` used to re-probe a failed driver only when it had claimed vendor interfaces; an own-interface driver is now retried when the mouse has produced reports since the last attempt, backing off to every sixth period after four failures. Test it by making a probe fail (a receiver with no mouse behind it, then waking the mouse).
- **ULX writes: allowlisted (user's decision) and verified (2026-11-04).** `361d:0100` is in `drv_openmouse.c`'s `VERIFIED`. The user confirmed that the desktop app and the Studio both change the ULX's CPI and polling rate and read the correct values back. A separate raw-hidraw script run from this side (a one-off, not kept) saw real changes acknowledged (status OK) but the readback unchanged, which contradicts that; the cause was not found and is probably that script (it read replies on a second open of the same node while the daemon also held it, and checked only 0 to 11 s later), but it is not proven. If a ULX write ever seems to do nothing, check it in the Studio before suspecting the firmware. Motion Sync and lift-off writes are allowed by the same entry but were not exercised; the desktop app has no control for them yet. Battery read 29%, 25% and 19% over about 30 minutes (derived from millivolts, so noisy, but the mouse was low).
- **Other mice whose commands go to an OUT endpoint** are covered by the same change; the Razer (feature reports) and Logitech (claimed interface) paths are untouched. Re-run the Razer and G Pro checks on this build when convenient.

Open, in order:
1. **First real-mouse run** (none was on hand on 2026-10-27): Razer Basilisk Ultimate, then the G Pro receiver. Check OpenMouse picks the driver, readStatus values match the 0.6.0 C ports, CPI and rate writes from the Studio, unplug/replug (session closed, heap returns), and log per-call time and heap peak.
2. Re-check the write allowlist against the engine (`drv_openmouse.c` keeps the 0.6.0 list: `1532:0086`, `046d:c547`), then decide whether OpenMouse's own `verified`/`valuesVerified` flags plus its passing tests are enough to open writes to other models (§4.2 rule).
3. Run OpenMouse's test suite under QuickJS as part of the bundle script, so a bump is checked the way it was by hand (`tools/openmouse/qjs_suite/`; the three real-timer files need `QJS_REAL_TIMERS=1`).
4. **Per-family loading** (for the Teensy 4.1 port, and faster boot here; the Teensy's hardware passed bring-up on 2026-11-03 with 8 MB of PSRAM and a working USB host port, see [README.md](README.md#teensy-41-port-target-hardware-bring-up-2026-11-03); with one 8 MB chip, the 4.3 MB full-registry heap measured on the P4 would fit, but only just): compile each family to its own bytecode module, map VID:PID to candidate families through the catalog, load only those, let OpenMouse's scoring pick, free the runtime on unplug; the full registry stays as the fallback. Shared vendor ids (`0x3554` Pulsar/Lamzu/Scyrox, `0x3151` GearHub/Fantech, `0x258a` Glorious/Redragon) load every candidate. Needs a small C module loader for esbuild's shared chunks. Measured budget: engine about 90 KB, median family about 130 KB, Logitech about 640 KB (32-bit estimate).
5. A sleeping receiver mouse: the engine reports a failed readStatus as a driver error, not as `AMV_LINK_ASLEEP` the way the C Logitech driver did.

**Built (see [README.md](README.md#mouse-discovery-and-information)):** the generated catalog of 810 ids (every known model is named), 4 MB OTA slots, the settings protocol and its Studio UI, the vendor-interface claim with the WebHID-shaped `amv_hl_*` shim, the receiver/asleep state, the Razer, Finalmouse, GearHub and Logitech drivers, Logitech CPI and rate writes verified on a G Pro.

### 4.1 What OpenMouse covers (source scan of commit `326f292`)
48 brand folders and about 100 driver classes in `src/drivers/registry.ts`. A port takes only what OpenMouse reads for that family among the five settings; the scan is a first pass, to be confirmed per family while porting.

| Group | Families (folder) | Notes |
|:---|:---|:---|
| **Hardware on hand** | `razer` (add LOD), `logitech` (LOD, wired path), `finalmouse` (ULX: LOD and Motion Sync), `gearhub` (Attack Shark R3 writes) | Verifiable here |
| **Largest catalogs (most product ids)** | `atk` (ATK/Compx engine, many rebadges), `mchose`, `keychron` (mice only), `steelseries` (24 client classes over a few wire formats), `gwolves`, `incott`, `lamzu` + `lamzu-atlantis`, `rawm`, `redragon`, `pulsar`, `endgame`, `asus`, `vaxee`, `ajazz` (mice) | One engine often covers many brands and dozens of ids |
| **Long tail** | `bytech` (IPI), `coolermaster`, `corsair` (+ Bragi), `dareu`, `delux`, `fantech`, `fater`, `glorious` (+ classic, core2), `gravastar`, `hitscan`, `hyperx`, `ksnake`, `microsoft`, `moddo`, `motospeed`, `ninjutso`, `noir`, `orbital`, `rapoo`, `ryunix`, `scyrox`, `teevolution`, `vgn`, `wallhack` (mouse), `wlmouse`, `zaunkoenig` | Small drivers, typically 100 to 700 lines of TypeScript |
| **Excluded** | `wooting`, `ajazz` AK820, `wallhack` keyboard, keyboard-only Keychron entries | Keyboards: the dongle only passes mice through |

Size: the relevant subset is estimated at 15,000 to 25,000 lines of TypeScript, becoming roughly 10,000 to 20,000 lines of C, which still fits the 4 MB slots.

### 4.2 Decisions (2026-10-26, user)
1. **Write policy.** A model's settings may be written when OpenMouse itself marks that exact model `verified: true` **and** its C port passes the transcript tests (§4.4). A test through the dongle is no longer required first. Until the transcript harness exists nothing new qualifies, so the writable models stay the ones already enabled (Razer Basilisk Ultimate wired, and the G Pro behind receiver `046d:c547` at the user's request). Probing and refresh stay read-only for every engine.
2. **No browser tunnel**, not even as a developer tool. Transcripts are captured by the Download descriptor and Download transcript buttons and the Node recorder instead.

### 4.3 Order of work
Each phase ships on its own: every known mouse is already named, and each engine after that adds real values.

| Phase | Content | State | Exit test |
|:---|:---|:---|:---|
| **A. Infrastructure** | 4 MB slots, catalog, shim, multi-interface claim, settings protocol, receiver/asleep state | **Done** apart from: the `./anymouse` settings list (§2.2), the transcript generator and replay harness (§4.4), the catalog carrying OpenMouse's `verified` flag, board checks for the new commands | Every OpenMouse id resolves to a name (done); the Razer port passes transcripts; the board checks pass on 0.6.x |
| **B. Hardware on hand** | Logitech, Razer LOD, Finalmouse ULX, GearHub R3 | **Partly done:** Logitech read plus CPI and rate writes verified on the G Pro | Verified on the G Pro, ULX and R3 through the dongle; their writes enabled |
| **C. Largest engines** | ATK/Compx, MCHOSE, Keychron mice, SteelSeries, G-Wolves, Incott, Lamzu, RAWM, Redragon, Pulsar, Endgame Gear, ASUS, VAXEE, AJAZZ mice | Not started | Transcripts pass; read-only unless OpenMouse marks the model verified |
| **D. Long tail** | The remaining families in §4.1 | Not started | Same |
| **E. Upkeep** | Pin and bump the OpenMouse commit, regenerate catalog and transcripts, port the diffs; look at the 53 upstream filters that matched no engine | Not started | A CI-style script flags upstream drift |

### 4.4 Open items

*Superseded by §4.0 where it concerns C ports; kept for the record.*

**Verification without owning every mouse (gates all further writes).**
- **Transcript tests:** `tools/openmouse_transcripts/` runs each upstream TypeScript client in Node against the fake devices OpenMouse's own tests define (e.g. `FakeAsusMouse`, `fakeBitmouse`), records every report exchanged plus the values the client decoded, and writes them to `tests/openmouse/<engine>.json`. A host-built C test replays each transcript: the port must send **byte-identical requests** and decode **identical values**. It is the same method that keeps `web/` honest against frames recorded from the board, and re-running the generator after an upstream update shows exactly what changed.
- The catalog generator carries OpenMouse's `verified` flag per model, so the write policy (§4.2) can be applied automatically.
- The Studio gets a *Download transcript* developer button next to *Download descriptor*.

**Logitech.**
- Report "mouse asleep, move it and retry" instead of "refused" when a write meets a sleeping mouse.
- Gate writes per mouse model, not per receiver id: `VERIFIED_PIDS = {0xc547}` currently covers anything behind that receiver.
- LOD: not ported (the level mapping depends on the onboard profile format). The wired path and the onboard-profile fallback for direct-connect mice are untried.
- The very first rate-write "lockup" on 2026-10-26 was never captured; a sleeping mouse is the likely cause (a captured console showed the host-control read going unanswered while the mouse was idle, and rate writes worked once it was awake), but that is not proven. A same-rate write was not separately tested.

**Other families.**
- **Razer:** LOD. A wireless Razer through its receiver (`1532:0088`, extended polling rates): the driver paths are written but untested.
- **Finalmouse ULX:** verify the read path and the 8 kHz receiver through the dongle, add LOD and Motion Sync, then decide on enabling writes (the §4.2 rule would need relaxing on the user's evidence that OpenMouse drives it, or wait for the transcripts).
- **Attack Shark R3 (GearHub):** the same: verify through the dongle, then writes. The relay handshake takes seconds, so it runs as an asynchronous job. The **wired** R3 is `3151:4026` ("R3 Gaming Mouse", seen on the Teensy on 2026-11-03), not only the receiver `3151:402d`; test the wired path too.
- **Catalog names for shared ids:** the catalog names `3151:4026` "A5 V3 Ultra+", but a wired Attack Shark R3 reports that same id. The engine (GearHub) is right; the name is not. Rebadged OEM stacks reuse ids, so when one id covers several models, show the mouse's own USB product string (or name every candidate) instead of a single catalog name.
- **Phases C, D and E** as in §4.3.

**Generic mice (no known protocol).** The *Measure CPI* wizard decided on 2026-10-25 is not built: drag the mouse a known distance and divide the counts by it, so a mouse with no driver can still get a real CPI for the effective-DPI readouts (today the user types the value once and it is stored per VID:PID).

**Tests.** Re-run the 68-check `tools/dongle_test.py` on 0.6.x (last full run was 0.5.0) and add checks for `GET_MOUSE_MODEL`, `GET_MOUSE_SETTINGS`, `GET_MOUSE_OPTIONS` and `SET_MOUSE_SETTING`, which have been built and flashed but never exercised over the vendor channel from a host.

---

## 5. Mouse-side hardware

### 5.1 Attack Shark R3 CPI writes
Implement R3 CPI writes (`SET 0x54` DPI table) in `src/hid_driver.c` (and the dongle's GearHub driver). The desktop app does not write R3 CPI today; only the polling rate is written (hardware-verified 2026-10-22).

### 5.2 Firmware flashing experiment (project Phase 3)
Vectoring on the mouse's own MCU (Attack Shark R3, nRF52840 + PAW3395):
- Test the SWD pinout on the R3 PCB (`VDD`, `GND`, `DIO`, `CLK` pads).
- Evaluate an open ZMK/UFA firmware build ([aroum/ufa](https://github.com/aroum/ufa)).
- Explore binary trampoline injection on the stock firmware with `morluto/rea` and Ghidra.

---

## 6. Windows

- **Research spike:** input interception method (Interception driver vs. virtual HID vs. vectoring-only), FpsAimForge on Windows, anti-cheat implications, a `hidapi` port. The dongle already vectors on Windows with no software, because the PC only sees a standard HID mouse.
- **Windows companion** (also an M6 item): a `hidapi` build of the dongle sync for live calibration on Windows.

---

## 7. M6: stretch goals (in value order)

1. **Vendor-command tunnel**, for mice with no known driver: `TUNNEL_SET/GET_REPORT{iface, type, id, data}` forwards to the downstream mouse, and `src/hid_driver.c` gets a transport abstraction (hidraw vs dongle). Largely superseded by §4, where the dongle runs the OpenMouse drivers itself. The browser-tunnel stopgap was decided against (§4.2).
2. **Extra downstream interfaces:** mirror keyboard and consumer collections (side buttons or DPI keys that some mice send as keystrokes) as extra device interfaces with cloned descriptors.
3. **Windows companion** (see §6).
4. **Optional identity cloning:** present the downstream mouse's VID:PID and strings, for per-device OS settings. Off by default, because cloning a supported vendor id would make `src/hid_driver.c` talk the wrong protocol to the dongle.
5. **WebUSB landing page** (unverified whether current desktop Chrome still surfaces it): a hint to open the web page when the dongle is plugged in.

---

## 8. Release housekeeping (before sharing builds)

- Request a free PID from Espressif's `espressif/usb-pids` program for `0x303A`; the dev PID `0x4D56` is a placeholder. Host software already identifies the dongle by the `HELLO` handshake, so changing it breaks nothing.
- Gate the debug commands `TEST_SCALE` and `TEST_SELF_CHANGE` out of release builds.
- Save the real report descriptor of every new mouse that passes by into `tests/descriptors/` (the Studio's Download descriptor button does it).

---

## 9. Low priority: WebHID Studio and FpsAimForge tuning

*Added 2026-10-27. Not important at the moment; do it after §3.1 shows which calibration parameters actually need tuning.*

**Goal:** see and tune the FpsAimForge calibration from the Studio, with a live direction needle and a per-octant landing-ratio view next to the weight chart.

**Constraint:** the game's telemetry exists only on the PC running it. `libanymouse_faf.so` writes `/dev/shm/anymouse_faf_telemetry` (`src/faf_telemetry.h`: run state, round id, a `faf_submovement_event_t` ring), and `src/calibration.c` turns it into per-octant median landing ratios r̃_k and damped weight updates at round end. A browser cannot read shared memory, so a native process has to relay it.

| Route | What it is | For | Against |
|:---|:---|:---|:---|
| **1. Through the dongle** | `STREAM_DIR{on}` (30 Hz input-direction needle and effective scale for the radar) and `PUSH_CALIB{per-octant median r̃_k, n, last P1 and T}` commands; `./anymouse` pushes the snapshot after each round (`src/dongle.c`) and the Studio shows a **read-only calibration mirror** | No extra service; works on a second screen; small | The native app still runs the algorithm, so the Studio can watch but not tune; needs `./anymouse` running and the Linux box |
| **2. Localhost bridge** | `./anymouse` serves the Studio files and a WebSocket on `localhost` (a secure context) streaming r̃_k, trial progress, round state and flick events; the Studio can also drive alpha, the clamp, the trial gate and start/stop of auto-adapt | Interactive tuning and flick-level data | The native app must run; browser-to-localhost security care; Linux only until the Windows port |
| **3. Calibration in the browser** | Run the calibration math in the page from streamed events | None worth the cost | Still needs a native relay, and duplicates `src/calibration.c` |

**Recommendation:** route 1 first (it is small and fits the dongle design), then route 2 only if interactive tuning turns out to be wanted. Both can be built and tested here against recorded telemetry (`logs/flicks.csv` or a captured shared-memory dump); only the live check needs a Linux machine with the game.

---

## 10. Risks still open

| Risk | Likelihood | Mitigation |
|:---|:---:|:---|
| An 8 kHz receiver misbehaves behind the P4 host (NAK storms, gaps) | Medium | Untested: needs the ULX receiver. Measure the input rate; the mouse accumulates deltas, so a gap lowers the rate but loses no motion. Tune with more transfers in flight |
| Double scaling (dongle and software engine both active) | High if unguarded | The guard is built, fails safe and has 61 tests against a fake dongle; confirm on real hardware (§2.2) |
| A write that persists on the mouse (Logitech host control; any onboard-profile write) | Low to medium | Write policy (§4.2), every write step logged, and the persistence is documented as in OpenMouse |
| WebHID unavailable (Firefox, Safari, Flatpak and Snap browsers) | Certain for those | The native app covers them; the page detects and explains |
| Windows exclusive HID access when the app and the web page both run | Medium | The Studio shows a clear "in use" message; the generation counter keeps state consistent across sequential use |
| Unusual report descriptors (12-bit packed X/Y, multiple report ids, vendor-defined mouse) | Medium | Table-driven parser plus a descriptor corpus; unknown layouts are logged and fall to the boot fallback or the ignored list |
| Pre-v3 silicon errata, or dropped support in newer ESP-IDF | Low to medium | IDF v5.5.4 is pinned; never update IDF mid-task |
| Competitive-play rules | Low | Directional sensitivity scaling is in the same category as Raw Accel or on-mouse sensitivity; check the specific tournament's hardware rules |

## 11. Second board: Waveshare ESP32-P4-Pico (planned by the user, 2026-11-04)
Read from Waveshare's [wiki](https://www.waveshare.com/wiki/ESP32-P4-Pico) and its [schematic PDF](https://files.waveshare.com/wiki/ESP32-P4-Pico/ESP32-P4-Pico-datasheet.pdf) (page 1, rendered and read, not guessed). Nothing has been flashed to it yet.

| Item | ESP32-P4-Pico | Same as the dev kit? |
|:---|:---|:---|
| Chip | ESP32-P4NRW32, 32 MB PSRAM in package; flash **32 MB** NOR (dev kit: 16 MB) | PSRAM yes; flash larger |
| **Chip revision** | not stated anywhere | **unknown: read it first** (`esptool --chip esp32p4 chip-id`); our build selects pre-v3 silicon (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, dev kit is v1.3) |
| USB-C | **UART only**: D+/D- go to a CH343P bridge (`USB_DBG_D_P/N`), TXD GPIO38, RXD GPIO37, DTR/RTS auto-reset to EN and BOOT (GPIO35) | Same role as the dev kit's UART USB-C. It is **not** a native USB port |
| Mouse side (HS host) | **P1, a 4-pin MX1.25 connector**: pin 1 VCC_5V, 2 USBD_N, 3 USBD_P, 4 GND. Wired straight to the chip's dedicated HS pins (DM/DP, pins 49/50): no mux, no hub, no jumper | Simpler than the dev kit. Needs an MX1.25 to USB-A pigtail (check the box). 5V is the board's `VCC_5V` rail, fed from the UART USB-C through a P-FET, so it is on whenever the board is powered |
| **PC side (FS device)** | **No connector.** GPIO24/GPIO25 (chip pins 52/53, nets `USBD1_N`/`USBD1_P`) are broken out only on the 2x20 header, **pins 19 (D-) and 20 (D+)** (the Pico-pinout "GP14/GP15" positions) | The same pins the dev kit's "other USB-C" uses after the software PHY swap, so the firmware's swap is unchanged; the cable is the new part |
| BOOT / RESET | BOOT = GPIO35, RST = EN, buttons present, CH343 auto-reset | Same |
| PSRAM / CPU | 200 MHz hex PSRAM and 360 MHz are the build defaults | Same |

**What this means.** The firmware should run unchanged apart from two things, and the hardware work is the PC-facing cable:
1. **Wire the PC link yourself:** a USB-A (or USB-C) breakout or cut cable to header pin 19 (D-), pin 20 (D+) and GND; the PC's 5V is not needed for data. D+/D- polarity and a short, twisted pair matter at Full-Speed. This replaces the dev kit's second USB-C.
2. **Power (schematic read 2026-11-04).** The Type-C's VBUS goes to net `USB0_5V` (through a TVS). A P-FET (`Q2`, AO3401) with a small PNP pair (`U2`, MMDT3906) joins `USB0_5V` to `VCC_5V`. `VCC_5V` feeds the 3.3 V buck (MP1658), the mouse connector P1 pin 1 and header pin 39 (`VSYS`); header pin 40 (`VBUS`) is `USB0_5V` itself. My reading of the circuit, not bench-tested: with only the Type-C present Q2 is on and `VCC_5V` follows it; with 5V supplied only on `VCC_5V` Q2 turns off, so nothing back-feeds the Type-C; **with both present Q2 is on and the two sources are tied together.** So: (a) the PC-facing data cable needs **no 5V** while the board is powered over the UART USB-C, so leave its 5V wire unconnected (or on a removable jumper); (b) a standalone dongle powered by the PC cable alone can take its 5V on pin 39 or pin 40 and run the 3.3 V rail and the mouse from it, but **never with the UART USB-C plugged into another computer at the same time**; (c) the budget is the PC port's 500 mA for the board plus the mouse (not measured; the board's own draw at 360 MHz with PSRAM and the mouse's RGB are the unknowns). Whether the Full-Speed device needs a VBUS-valid signal wired from the PC cable is unknown: on the dev kit nothing was wired, and the Pico's header has no VBUS sense pin for these GPIOs, so if the PC does not enumerate it with only D+/D- connected, check the VBUS-valid override in the USB device setup first.
3. **Chip revision first.** If `chip-id` reports v3.x, the pre-v3 setting is wrong for that board and the image needs the v3 configuration (a different `sdkconfig` value, not a code change); if it reports v1.x it is the same as the dev kit. Do not flash before reading it.
4. **Flash size:** the image and partition table (about 8.1 MB used) fit the 16 MB the firmware is configured for, so the board runs it unchanged and the extra 16 MB is unused. Setting `CONFIG_ESPTOOLPY_FLASHSIZE_32MB` is optional.
5. **Flashing:** a fresh board needs the full image: `pio run -e p4`, then `.pio/build/p4/firmware.factory.bin` at 0x0 (bootloader, partition table, app) through the UART USB-C; it starts at default weights with empty slots. Back the board's existing flash up first if it has anything on it (`esptool read-flash`), since the stock image is not otherwise recoverable.
6. **First checks on the board:** `esptool chip-id` and `flash-id`; the console boot log (`amv_host`: High-Speed host up, a mouse in P1 enumerating with `HPRT` speed 0); then the PC link through the header wiring (the dongle should show up as `303a:4d56`).

### 11.1 Alternative: Waveshare ESP32-P4-Core-DEV-KIT (compared 2026-11-04)
Read from its [wiki](https://docs.waveshare.com/ESP32-P4-Core-DEV-KIT) and [schematic PDF](https://files.waveshare.com/wiki/ESP32-P4-Core-DEV-KIT/ESP32-P4-Core-DEV-KIT.pdf) (page 1, rendered and read). Same chip family and sizes as the Pico (ESP32-P4NRW32X, 32 MB flash, 32 MB PSRAM, revision not stated), but the USB wiring is the dev kit's topology, not the Pico's:

| Connector | Core-DEV-KIT | Role for the dongle |
|:---|:---|:---|
| Type-C `H2` "Type-C UART" | `USB0_5V`, D+/D- to a **CH343P** (`USB_DBG_D_P/N`), TXD GPIO38, RXD GPIO37, DTR/RTS auto-reset (EMH4T2R) to EN and BOOT (GPIO35) | Flashing and console, like the dev kit |
| Type-C `H1` "Type-C" | `USB1_5V`, D+/D- (`USB1P1_N/P`) to chip pins 37/38 = **GPIO24/GPIO25** | **The PC link, after the same software PHY swap. No wiring needed.** |
| 4-pin P1 (MX1.25) | VCC_5V, GND and `USBD1_N/P` to the chip's dedicated HS pins 35/36 (`DM`/`DP`), no mux or jumper | Mouse side; needs a pigtail to USB-A |

- Each Type-C feeds `VCC_5V` through its own P-FET circuit (`USB0_5V` through Q2, `USB1_5V` through Q3), so **the PC-link Type-C alone powers the board and the mouse connector**: a standalone dongle needs one cable. Plugging both Type-Cs into two different computers ties their 5V rails together through the two FETs, the same caution as on the dev kit.
- Open: the chip revision (read it with `esptool chip-id` before flashing; the build assumes pre-v3 silicon); the P1 connector's housing (the symbol says MX1.25 but the BOM text says "GH1.25 4P", which are different housings, so check the cable in the box); the current budget of the PC port for board plus mouse; flash is 32 MB, of which the firmware uses the first 16 MB unchanged.
- Verdict for this project: the Core-DEV-KIT needs no PC-link wiring, no external power handling and no VBUS-valid worry beyond what the dev kit already had, so it is the lower-risk second board. The Pico remains workable with a hand-wired GPIO24/25 cable (§11 above).
