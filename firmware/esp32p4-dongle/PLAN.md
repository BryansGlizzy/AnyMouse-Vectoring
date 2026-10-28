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
| Section 10, full OpenMouse coverage | **Direction changed 2026-10-27:** OpenMouse's own drivers run on the dongle in QuickJS-ng (firmware 0.7.0, flashed, loads at boot); real-mouse test pending | §4.0 |

---

## 2. Needs the Linux box (CachyOS)

### 2.1 M2: the Linux engine on the shared math
- Move `scale_vector()` from `src/evdev_rt.c` into use of `common/amv_octant.h` (already shared with the firmware), behaviour-identical.
- Add `make test-common`: a fixed set of `(dx, dy, weights) -> (out, remainder)` golden vectors checked by both the Linux build and the firmware unit test (`tests/test_octant.c`), so the two cannot drift. `Makefile` also needs `-Icommon`.
- **Exit:** golden vectors pass on both targets.
- Unrelated drift noticed earlier and still true: README section 7 says the UDP weight channel was removed, but `src/evdev_rt.c` still listens on UDP 42069 (`UDP_PORT` in `src/core.h`). The dongle sync does not depend on it.

### 2.2 The dongle link against the real board
The Linux client (`src/dongle.c`, the guard, CLI, GUI badge) was built and tested against a fake dongle (`make test-dongle`, 61 checks) but never run over hidraw against the board.
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

Open, in order:
1. **First real-mouse run** (none was on hand on 2026-10-27): Razer Basilisk Ultimate, then the G Pro receiver. Check OpenMouse picks the driver, readStatus values match the 0.6.0 C ports, CPI and rate writes from the Studio, unplug/replug (session closed, heap returns), and log per-call time and heap peak.
2. Re-check the write allowlist against the engine (`drv_openmouse.c` keeps the 0.6.0 list: `1532:0086`, `046d:c547`), then decide whether OpenMouse's own `verified`/`valuesVerified` flags plus its passing tests are enough to open writes to other models (§4.2 rule).
3. Run OpenMouse's test suite under QuickJS as part of the bundle script, so a bump is checked the way it was by hand (`tools/openmouse/qjs_suite/`; the three real-timer files need `QJS_REAL_TIMERS=1`).
4. **Per-family loading** (for the Teensy 4.1 port, and faster boot here): compile each family to its own bytecode module, map VID:PID to candidate families through the catalog, load only those, let OpenMouse's scoring pick, free the runtime on unplug; the full registry stays as the fallback. Shared vendor ids (`0x3554` Pulsar/Lamzu/Scyrox, `0x3151` GearHub/Fantech, `0x258a` Glorious/Redragon) load every candidate. Needs a small C module loader for esbuild's shared chunks. Measured budget: engine about 90 KB, median family about 130 KB, Logitech about 640 KB (32-bit estimate).
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
- **Attack Shark R3 (GearHub):** the same: verify through the dongle, then writes. The relay handshake takes seconds, so it runs as an asynchronous job.
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
