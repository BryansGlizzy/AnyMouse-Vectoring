# AnyMouse-Vectoring: Development Progress & Milestones

Sister document to [`README.md`](README.md) tracking operational checkpoints, active implementations, architecture decisions, and roadmap progression with chronological timestamps.

---

## Project Status Overview

| Phase | Description | Status | Current Milestone |
| :---: | :--- | :---: | :--- |
| **Phase 1** | OS-Level Vectoring Daemon (Linux `evdev` + `uinput`) | **Complete** | Consolidated into unified C binary (`./anymouse`) with `EVIOCGID` 1:1 KWin parity |
| **Phase 1.5** | Native Mouse Hardware Protocol Integration (`OpenMouse`) + Universal PnP | **Complete** | Auto-detect any mouse, Finalmouse ULX CPI/polling, Attack Shark R3 polling (hardware-verified), Steam auto-install, auto-saved profile |
| **Phase 2** | `FpsAimForge` (FAF) Native Telemetry Plugin & Ballistic Calibration (KovaaK's Sunset/Retained) | **Verified / Active** | Live 3D submovement capture verified in Steam `FpsAimForge` (`libanymouse_faf.so` + `/dev/shm/anymouse_faf_telemetry`) |
| **Phase 3** | Mouse MCU On-Board Firmware (Attack Shark R3) | **Planned** | Testing ZMK/UFA (`aroum/ufa`) and Ghidra trampoline patching |
| **Phase 4** | Universal Hardware USB Pass-Through Dongle (ESP32-P4) | **In development (active)** | Real mouse (Razer Basilisk Ultimate) read and vectored through the dongle; Studio works in a real browser; next: mouse info (polling rate, CPI) on both front-ends; see `firmware/esp32p4-dongle/PLAN.md` |

---

## Chronological Progress Log

### [2026-10-20 22:36] - Baseline Formulation & Mathematical Modeling
- **Milestone:** Formulated anisotropic 8-octant polar sensitivity model to address human wrist/forearm biomechanical asymmetry (radial vs. ulnar deviation, flexion/extension, pronation/supination).
- **Core Equations Formulated:**
  - Standardized Cartesian polar angle calculation: $\theta = \text{atan2}(-\Delta y, \Delta x) \in [0, 2\pi)$.
  - $C^1$ continuous cosine interpolation between adjacent octant anchors:
    $$\tilde{t} = \frac{1 - \cos(t\pi)}{2}, \quad S(\theta) = M_{k_0}(1 - \tilde{t}) + M_{k_1}\tilde{t}$$
  - Sub-pixel remainder accumulation (error diffusion) to prevent integer truncation truncation dead-zones during slow micro-adjustments.
  - Telemetry metric: Primary ballistic submovement inflection detection ($v(t) < 0.15 \cdot v_{\text{peak}}$) + radial landing ratio projection ($r_i = \frac{\mathbf{P}_1 \cdot \mathbf{T}}{\|\mathbf{T}\|^2}$) with damped learning rate update rule.
- **Hardware Realities Defined:** Disproved feasibility of mid-swipe optical sensor CPI register changes (SPI bus contention, frame stalls, 50 CPI steps, 2-axis only). Established standard: Lock hardware optical sensor to high fixed CPI (3200 or 6400) and scale displacement vectors in software/firmware.
- **Artifact Created:** Complete foundational documentation in [`README.md`](README.md).

---

### [2026-10-20 22:45] - Hardware Identification & WebHID Linux Permissions
- **Milestone:** Identified silicon architecture and verified USB hardware descriptors on CachyOS / Arch Linux:
  - **Finalmouse UltralightX (ULX):** `361D:0100` (`hidraw6`, single combined motion/config endpoint).
  - **Attack Shark R3:** `3151:402D` (`hidraw11` motion, `hidraw12`/`hidraw13` config endpoints; Yichip/Royuan/Fantech OEM stack).
  - Both devices confirmed running **Nordic nRF52840 MCU + PixArt PAW3395 sensor**.
- **Resolved systemd-udevd Quirks:**
  - Case-sensitivity between USB subsystem (`idVendor=="361d"`) and HID subsystem (`HID_ID=0003:0000361D:00000100`).
  - Addressed `udevadm trigger --action=change` limitation by enforcing `--action=add --subsystem-match=hidraw --subsystem-match=usb`.
  - Configured `/etc/udev/rules.d/70-mouse-webhid.rules` granting `uaccess` and `0666` permissions to hidraw nodes and `/dev/uinput`.

---

### [2026-10-20 22:51] - Phase 1 Real-Time C Daemon Implementation
- **Milestone:** Created zero-dependency, low-latency POSIX C daemon (`octant_daemon.c`):
  - Interface directly with `<linux/input.h>` and `<linux/uinput.h>` using exclusive grab (`ioctl(EVIOCGRAB)`).
  - `SCHED_FIFO` real-time scheduling priority ($80$) and memory page locking (`mlockall`) for microsecond-level wake latency on CachyOS.
  - Batched kernel event draining: Reads up to $64$ events per `read()` syscall, transforms vectors in CPU registers, and flushes frame via a single `write()` syscall.
  - Non-blocking UDP IPC socket on `127.0.0.1:42069`: Listens for 32-byte updates (`float weights[8]`) mid-swipe with zero mutex contention.
- **Compilation:** Compiled with `gcc -O3 -march=native -ffast-math -flto -Wall -Wextra -lm` into an $18\text{ KB}$ native binary (`octant_daemon`).

---

### [2026-10-20 22:58] - Kernel Module Hot-Fix & Launcher Script
- **Challenge:** Running `octant_daemon` produced `open /dev/uinput: No such device` (`ENODEV`).
- **Root Cause Identified:** CachyOS kernel packages were upgraded to `7.2.10-1-cachyos`, but the active running kernel was `7.2.9-2-cachyos`. The package upgrade deleted `/usr/lib/modules/7.2.9-2-cachyos/`, causing `modprobe uinput` to fail on the un-rebooted system.
- **Resolution:**
  - Extracted the matching binary `uinput.ko` directly from `/var/cache/pacman/pkg/linux-cachyos-7.2.9-2-x86_64_v4.pkg.tar.zst`.
  - Created [`start_daemon.sh`](start_daemon.sh) with automatic kernel module loading (`insmod uinput.ko`) and device auto-detection.
  - Updated [`Makefile`](Makefile) with build and run targets (`make run-ulx`, `make run-shark`).
- **Verification:** Successfully executed `sudo ./start_daemon.sh`, verified hardware grab, and confirmed live UDP updates with `send_weights.py` (`vertical_boost` and `neutral` presets).

---

### [2026-10-20 23:05] - Interactive PyQt6 Polar Studio GUI
- **Milestone:** Implemented complete graphical calibration interface (`gui.py`):
  - **Interactive Polar Radar Widget:** Real-time polygon rendering of the 8-octant sensitivity shape with interactive mouse drag handles on all 8 spokes.
  - **Granular Controls:** 8 individual sliders and DoubleSpinBoxes with dynamic Effective DPI readouts ($DPI = CPI \times weight$).
  - **Hardware Sensor Configurator:** Selectable hardware baseline CPI ($1600$, $3200$, $6400$, etc.).
  - **Master Scaling & Symmetry Linking:** Global multiplier slider + toggles for Left/Right and Up/Down mirror symmetry.
  - **Preset Management:** One-click application of `Neutral`, `Vertical Boost`, `Horizontal Boost`, `Diagonal ULX`, and `Low`.
  - **Profile Persistence:** JSON profile export and import.
  - **Seamless IPC Integration:** Added `--gui` flag to `send_weights.py` and added `make gui` target. Every UI adjustment emits instantaneous UDP packets to port $42069$.

---

### [2026-10-20 23:22] - Phase 1.5 OpenMouse Native Protocol Integration
- **Milestone:** Reverse-engineered and integrated [`OpenMouse-Project/mouse-protocol`](https://github.com/OpenMouse-Project/mouse-protocol) directly into Python without browser or WebHID dependencies.
- **Hardware Protocol Analysis:**
  - **Finalmouse ULX (`0x361D:0x0100`):** Communicates over `/dev/hidrawX` using Output Report `0x04` and Input Report `0x05`. Command `0x60` (`wakeAll`) triggers a burst of telemetry reports:
    - Command `0x03` / `0x10`: Physical sensor CPI (`u16le`).
    - Command `0x04` / `0x11`: Polling rate in Hz (`u16le`).
    - Command `0x05`: Battery voltage in mV (`u16le`).
    - Command `0x0b` / `0x0c`: Dongle & mouse firmware strings.
    - Command `0x0d`: 2.4 GHz RF RSSI in dBm (`int8`).
    - Command `0x0e`: Hardware unit serial number.
    - Command `0x12`: Motion Sync hardware status (`bool`).
  - **Attack Shark R3 / GearHub (`0x3151:0x402D`):** Uses Bit7 checksum framing (`byte[7] = 255 - (sum(bytes 0..6) & 0xFF)`) with 2.4 GHz relay handshaking (`0xF6 0x05` $\to$ `0xF7` $\to$ command $\to$ `0xFC`).
- **Implementation & Live Telemetry Verification:**
  - Created `mouse_protocol.py`: Scans `/sys/class/hidraw/`, queries hardware over raw Linux file descriptors, and decodes telemetry in $< 50\text{ ms}$.
  - Verified live on connected hardware:
    - Detected: `Finalmouse UltralightX (Serial: MA234903747) on /dev/hidraw6`
    - Sensor DPI: `3200 CPI`
    - Polling Rate: `2000 Hz`
    - Battery: `59% (3.88V)`
    - Motion Sync: `ON`
    - RSSI: `-19 dBm`
- **GUI Integration:**
  - Updated `gui.py` with dynamic hardware badges (device identity, polling rate, battery, Motion Sync, RSSI).
  - Physical CPI auto-sync: Automatically updates the locked sensor CPI dropdown and calculates true Effective DPI for all 8 octants.
  - Added direct hardware control buttons (`Set 3200 CPI`, `Set 6400 CPI`) to write physical sensor registers via HID reports directly from the app.
  - Background `QTimer` refreshes telemetry every 8 seconds.

---

### [2026-10-21 17:08] - Unified Multi-Threaded C Consolidation (`./anymouse`)
- **Milestone:** Eliminated all split Python scripts (`gui.py`, `mouse_protocol.py`, `send_weights.py`) and inter-process UDP requirements by consolidating the entire stack into a single $86\text{ KB}$ multi-threaded POSIX C binary (`./anymouse`):
  - **Shared Lock-Free Core ([`src/core.h`](src/core.h), [`src/main.c`](src/main.c)):** Uses `_Atomic float octant_weights[8]`, `_Atomic int hardware_cpi`, and a $65,536$-slot lock-free Single-Producer Single-Consumer (`SPSC`) ring buffer (`tick_ring`) in shared process memory.
  - **Thread 1 — Real-Time Vectoring Hot-Path ([`src/evdev_rt.c`](src/evdev_rt.c)):** Runs at `SCHED_FIFO` priority 80 with `mlockall()`, performs `EVIOCGRAB`, executes Cosine $C^1$ interpolation and sub-pixel error diffusion (`scale_vector()`), writes to `/dev/uinput`, and pushes every `SYN_REPORT` frame to `tick_ring` lock-free.
  - **Native C OpenMouse `/dev/hidraw` Driver ([`src/hid_driver.c`](src/hid_driver.c)):** Direct C port of the Finalmouse ULX (`0x361D:0x0100`) and Attack Shark R3 (`0x3151:0x402D`) HIDraw telemetry reader and hardware CPI writer (both preset buttons and CPI dropdown write directly to the mouse sensor).
  - **Thread 3 — Native GTK4 + Cairo Studio GUI ([`src/gui_gtk4.c`](src/gui_gtk4.c)):** Hardware-accelerated Cairo polar radar chart with draggable handles, live $\mathbf{P}_1$ vs. $\mathbf{T}$ vector overlay, uniform octant scalar controls (`1:1 (1.0x)`, `Half (0.50x)`, `Quarter (0.25x)`), symmetry linkers, and JSON profile persistence.
- **Resolved KDE Plasma / KWin Wayland $2\times$ Acceleration Quirk:**
  - **Root Cause:** In `~/.config/kcminputrc`, the physical Finalmouse ULX (`Vendor=13853`, `Product=256`, `Name="Finalmouse UltralightX dongle Mouse"`) was configured with `PointerAccelerationProfile=1` (Flat / unaccelerated) and `PointerAcceleration=-0.200`. Because the virtual `uinput` device previously used `Product=0x0101` (`257`), KWin Wayland treated it as an unconfigured mouse and applied `libinput`'s default Adaptive acceleration curve (`Profile=2`, $2.0\times$ incline).
  - **Fix:** Updated [`setup_uinput_device()`](src/evdev_rt.c) to clone the exact `EVIOCGID` (`bustype`, `vendor`, `product`, `version`) and `EVIOCGNAME` from the grabbed physical mouse so KWin Wayland applies the exact same Flat (`Profile=1`) settings for true $1:1$ sensitivity parity.

---

### [2026-10-21 17:55] - Phase 2 KovaaK's UE4.26 Static Analysis & Session-Gated Telemetry
- **Milestone:** Installed [`morluto/rea`](https://github.com/morluto/rea) (`v6.7.0`) and **Ghidra 12.1.2**, extracted UE4.26 (`++UE4+Release-4.26`) reflection tables from `FPSAimTrainer-Win64-Shipping.exe` (`Build 3.8.7`), and implemented strict session-gated round-to-round telemetry in [`src/kovaaks_offsets.h`](src/kovaaks_offsets.h) and [`src/calibration.c`](src/calibration.c).
- **Verified UE4.26 & `/Script/GameSkillsTrainer` Offsets:**
  - **Global Pointers (`.data`):** `GWorld` at RVA `0x05533750` (`VA 0x145533750`), `FNamePool` at RVA `0x053C3340` (`VA 0x1453C3340`).
  - **Engine Hierarchy:** `UWorld::PersistentLevel` (`0x30`), `UWorld::GameState` (`0x120`), `UWorld::OwningGameInstance` (`0x180`), `UGameInstance::LocalPlayers` (`0x38`), `UPlayer::PlayerController` (`0x30`), `AController::ControlRotation` (`0x288`), `APlayerController::AcknowledgedPawn` (`0x2A0`), `APlayerController::PlayerCameraManager` (`0x2B8`), `APlayerCameraManager::CameraCachePrivate.POV` (`0x1AF0`).
  - **`AMetaGameState` (`GameState`):** `TArray<AMetaCharacter*> Characters` at `+0x02F0` (enumerates the player character plus all spawned bot characters).
  - **`AMetaPlayerController` (`PlayerController`):** `UserYawSensitivity` (`+0x0608`), `UserPitchSensitivity` (`+0x060C`), `MyCharacter` (`+0x0618`), `PauseMenuLocation` (`+0x0678`, `0` during active unpaused gameplay), `LastEnemyAttacked` (`+0x06A8`).
  - **`AMetaCharacter` (Player & Bots):** `RootComponent` (`+0x0130` $\to$ `RelativeLocation` at `+0x011C`), `KillCount` (`+0x09C8`), `KillStatistics` (`+0x09D0`), `TotalOverShots` (`+0x09E0`), `DamageDone` (`+0x0A20`), `LastScore` (`+0x0A28`), `ChallengeStartTime` (`+0x0A38`, `FDateTime` ticks), `LastShotFiredTime` (`+0x0A78`), `Health` (`+0x0E58`, $> 0$ when bot is alive).
- **Strict Session Gating & Inter-Round Lifecycle ([`src/calibration.c`](src/calibration.c)):**
  - Tracks 4 explicit states: `KOVAAKS_STATE_OFFLINE`, `KOVAAKS_STATE_MENU_OR_LOBBY` (`bot_count == 0`), `KOVAAKS_STATE_PAUSED` (`PauseMenuLocation != 0`), and `KOVAAKS_STATE_ACTIVE_ROUND` (`MyCharacter != NULL && bot_count > 0 && PauseMenuLocation == 0`).
  - **Zero Desktop False-Positives:** When not in `KOVAAKS_STATE_ACTIVE_ROUND`, `tick_ring` is immediately flushed (`ring_tail = ring_head`) so desktop mouse movements are never tracked.
  - **Round-to-Round Aggregation & Adaptation:** Detects Challenge round start/restart via `ChallengeStartTime` (`+0x0A38`) and `KillCount` (`+0x09C8`) transitions, and detects Challenge completion via `inotify` on `FPSAimTrainer/stats/*.csv`, applying damped weight updates ($\alpha = 0.35$, $\pm 5\%$ clamp) strictly **between rounds**.

---

### [2026-10-21 18:50] - Phase 2 Transition to `FpsAimForge` (FAF) Native Telemetry Plugin & Custom 8-Octant Suite
- **Milestone:** Transitioned primary Linux Phase 2 calibration from KovaaK's (due to Linux anti-cheat incompatibility; KovaaK's code in [`src/kovaaks_offsets.h`](src/kovaaks_offsets.h) and [`src/calibration.c`](src/calibration.c) is preserved/sunset behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing) to **`FpsAimForge` (`v0.7.0`)**, a native Linux C++23/SDL3 open-source aim trainer installed on Steam.
- **Native In-Process Telemetry Plugin ([`src/faf_plugin.c`](src/faf_plugin.c) $\to$ `libanymouse_faf.so`):**
  - Hooks `FpsAimForge`'s 13 `Scenario` / `BaseScenario` vtables in `.data.rel.ro` (`OnTickStart`, `OnDetach`, `UpdateState`, `OnScenarioDone`) in-process with zero external memory sniffing.
  - Installed native Steam launcher wrapper ([`src/faf_launcher.c`](src/faf_launcher.c)) at `/mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge/FpsAimForge` so `libanymouse_faf.so` loads automatically whether `FpsAimForge` is launched from Steam or directly.
  - Streams live scenario state (`FAF_STATE_MENU`, `FAF_STATE_WAITING_FOR_CLICK_TO_START`, `FAF_STATE_START_COUNTDOWN`, `FAF_STATE_RUNNING`, `FAF_STATE_DONE`), round timers, hit/shot/kill counters, 3D camera `(yaw, pitch)`, and active 3D target coordinates over POSIX shared memory (`/dev/shm/anymouse_faf_telemetry`, defined in [`src/faf_telemetry.h`](src/faf_telemetry.h)).
  - **True 3D Target-Aware Submovement Extraction:** Records $1000\text{ Hz}$ camera `(yaw, pitch)` trajectory during active rounds, matches each click/kill to the exact 3D target center $\mathbf{T}$ from `TargetManager::last_removed_target_`, isolates the primary ballistic submovement $\mathbf{P}_1$ (velocity drop $< 16\%$ of $\omega_{\text{peak}}$ or corrective velocity valley $< 42\%$ of $\omega_{\text{peak}}$), and emits verified `faf_submovement_event_t` packets into a lock-free ring buffer.
- **Custom `AnyMouse` Calibration Bundle ([`faf/AnyMouse.bundle.json`](faf/AnyMouse.bundle.json)):**
  - Installed into `~/.local/share/FpsAimForge/bundles/AnyMouse.bundle.json`:
    1. **`AnyMouse 8-Octant Center-Out`**: Alternates between neutral wrist center `(0, 0)` and gap-balanced randomized targets across all 8 polar octants (`E, NE, N, NW, W, SW, S, SE`). Center return clicks automatically reset trajectory origin without polluting outward ballistic statistics.
    2. **`AnyMouse 8-Octant Center-Out Ghost`**: 2-target preview mode (`newestTargetIsGhost: true`) for rhythmic center-out calibration.
    3. **`AnyMouse 8-Octant Wide Flick`**: Wide-angle forearm + wrist calibration drill across all 8 octants.
    4. **`AnyMouse Calibration Suite`** playlist.

---

### [2026-10-21 19:20] - Live `FpsAimForge` Steam Verification & Linux `fs.protected_regular` Shared-Memory Fix
- **Milestone:** Verified `libanymouse_faf.so` injecting cleanly into Steam's `FpsAimForge` inside `SteamLinuxRuntime_4` (`pressure-vessel`) and capturing live 3D camera + target ballistic submovements in real time (`57` submovement events captured during an initial `AF Static3` test run at `45.0 cm/360`).
- **Resolved Linux Sticky `/dev/shm` Permission Quirk (`fs.protected_regular = 1`):**
  - **Root Cause:** When `FpsAimForge` (running as unprivileged user `future`) created `/dev/shm/anymouse_faf_telemetry` (`0666`), `./anymouse` (running as `root` via `sudo`) called `shm_open(ANYMOUSE_FAF_SHM_NAME, O_CREAT | O_RDWR, 0666)`. Under Linux kernel `sysctl fs.protected_regular = 1`, passing `O_CREAT` on an existing regular file in world-writable sticky directory `/dev/shm` owned by a non-root user returns `EACCES`.
  - **Fix:** Updated [`open_faf_shm()`](src/calibration.c#L586-L618) in [`src/calibration.c`](src/calibration.c) and [`faf_plugin_init()`](src/faf_plugin.c#L588-L607) in [`src/faf_plugin.c`](src/faf_plugin.c) to call `shm_open(..., O_RDWR, 0666)` **without `O_CREAT`** first, falling back to `O_CREAT | O_RDWR` only when `errno == ENOENT`, and updated [`Makefile`](Makefile) to unlink (`rm -f`) before relinking shared libraries so live-mapped processes never receive `SIGBUS`.

---

### [2026-10-22] - Documentation Sync & First Commit of the Unified C Engine
- **Milestone:** Brought [`README.md`](README.md) in line with the current architecture: removed the obsolete UDP/Unix-socket weight channel and the Raylib/Bevy/Godot trainer from the Section 7 diagram and Phase 1 notes (weights now live in process memory as `_Atomic float octant_weights[8]`), and fixed the Section 12 table-of-contents entry to say `FpsAimForge`.
- **Repository:** Committed the unified C engine (`src/`), the `FpsAimForge` bundle (`faf/`), and the removal of the legacy Python/`octant_daemon.c` files. Files named in earlier log entries (`gui.py`, `mouse_protocol.py`, `send_weights.py`, `octant_daemon.c`) are kept in that history as the prototype stage and no longer exist in the tree.

### [2026-10-22] - Flick CSV Log, GUI Auto-Adapt Sync & Longer Rounds
- Added a persistent per-flick CSV log (`logs/flicks.csv`, see README) so calibration history survives the 256-slot ring and restarts.
- Fixed the GUI so the "Inter-Round Auto-Adapt" checkbox reflects `--auto-calibrate` at startup.
- Extended all three `AnyMouse` scenarios from 45 s to 60 s to reach the 5-trial per-octant gate more reliably.

### [2026-10-22] - Universal Mouse Auto-Detect, Polling-Rate Control & Steam Auto-Install
- **Universal plug & play ([`src/devices.c`](src/devices.c)):** Replaced the hard-coded Finalmouse/Attack Shark `by-id` paths with sysfs discovery of every physical pointing device.
  - **Ranking:** known vendor protocol +100, keyboard/macro-pad auxiliary mouse interfaces −40. Our own uinput clone is excluded by its `/sys/devices/virtual` location.
  - **Hotplug:** the RT thread now loops over select → clone → grab, releases on unplug, waits for the device to return, and moves back to a better mouse when one is plugged in (inotify on `/dev/input`).
  - **Safety:** after 50 consecutive uinput write failures the grab is released, so a dead virtual mouse can't strand the pointer.
- **GUI:** mouse dropdown with rescan, identify-by-motion and manual path fallback (persisted in `~/.config/anymouse/config`), a polling-rate dropdown, and an FpsAimForge install/repair row. Ctrl+C now closes the GUI cleanly (previously SIGINT left the GTK loop running).
- **Polling rate ([`src/hid_driver.c`](src/hid_driver.c)):** Finalmouse ULX 500–8000 Hz (cmd 17, u16le). Attack Shark R3 125–8000 Hz via the GearHub relay and an `OPTIONPARAM0` read-modify-write with read-back verification, per `OpenMouse-Project/mouse-protocol`. Hardware-verified on the R3 on 2026-10-22 (1k ↔ 2k switching confirmed in the GUI and on the mouse). Correction to the 2026-10-21 entry: Attack Shark R3 CPI *writes* were never implemented; only the Finalmouse writes CPI.
- **Steam auto-install ([`src/steam_install.c`](src/steam_install.c)):** `--install-faf` / `--uninstall-faf` / `--faf-status` locate FpsAimForge (app `5165380`) through `libraryfolders.vdf` and the appmanifest. The launcher now resolves `FpsAimForge.bin` and the plugin relative to itself, so no paths are compiled in, and it carries a marker so the installer can detect Steam updates and repair them. Pre-marker launchers are recognised, so the existing install migrated in place with `FpsAimForge.bin` byte-identical.
- **Fix (same day): polling dropdown froze the GUI on the Attack Shark R3.** The rate handler ran the multi-second relay write on the GTK thread and rebuilt the dropdown's items from inside its own popover selection. The popover re-fired selections, so the GUI looped write → re-read → rebuild while repeatedly rewriting the R3 settings block. The written rate itself was correct.
  - **Fix:** all vendor HID I/O now runs on one `hw-io` worker thread with results applied via `g_idle_add`. The rate list is only rebuilt when the supported rates change. Writes are skipped while a job is in flight or when the selection equals the rate the mouse reports. R3 relay reads retry once.
  - **Also fixed:** a 2-event stack overflow of `out_buf` in the RT thread when an evdev frame splits across reads.
- **Profile persistence:** weights are restored on launch and saved on exit to `~/.config/anymouse/profile.json` (atomic write, user-owned under sudo; the repo-local `profile.json` is read once as a fallback). `--preset` / explicit scale runs don't overwrite it. The Master Vector Scaling widgets now follow the loaded weights (they previously only changed the underlying values).
- **`--install-udev`:** optional `uaccess` rules + `uinput` autoload so the engine can run without sudo (`ANYMOUSE_NO_SUDO=1 ./start_daemon.sh`).

### [2026-10-25] - Phase 4 Re-targeted to ESP32-P4; Hardware Verified
- **Decision:** Phase 4 (hardware pass-through dongle) moves ahead of Phase 3 and the Windows spike. It uses a **Waveshare ESP32-P4-Module-DEV-KIT** instead of an RP2040. Configuration is over a vendor HID channel on the same USB cable (WebHID web page and/or `./anymouse`); Wi-Fi and USB networking were dropped. A hardware dongle also covers Windows vectoring without an interception driver.
- **Verified over the CH343 UART (`1A86:55D3`) with `esptool` 4.12.0:**
  - ESP32-P4 **rev v1.3** (pre-v3 silicon, 360 MHz), 16 MB GigaDevice flash, 32 MB PSRAM.
  - eFuses clean: no secure boot, no flash encryption, download mode enabled.
  - The previous Arduino "Haltech Multi-Gauge" image is disposable.
- **USB ports identified:** one USB-C is the CH343 UART bridge (`1A86:55D3`); the other is the P4's built-in USB Serial/JTAG (`303A:1001`), which cannot be a HID mouse. The USB-A is the High-Speed OTG (mouse side).
- **Schematic findings:** the two USB-A stacks (4 ports) sit behind one High-Speed controller through a mux (HOST jumper = port 1 direct, DEVICE = CH334 hub for ports 2-4); neither can face the PC. The "other" USB-C is wired to GPIO24/25.
- **PC-facing link solved in software (M0, device half):** `usb_wrap_ll_phy_select(&USB_WRAP, 0)` moves the Full-Speed OTG controller onto the "other" USB-C. Spike `firmware/esp32p4-dongle/spikes/m0_phy_swap/` (ESP-IDF v5.5.4, `esp_tinyusb` 3.0.0) enumerates on the Mac as `303A:4D56` with a HID mouse and a vendor HID interface (usage page `0xFF4D`); 20/20 vendor echoes, about 5 ms RTT. No eFuse burned, no header wiring needed.
- **Remaining M0 work:** the mouse-side USB host half on the HS port, USB-A VBUS, and whether a reset restores the default PHY mapping. Full-Speed caps output to the PC at 1000 Hz; input is still read at the mouse's native rate.
- **Plan:** [`firmware/esp32p4-dongle/PLAN.md`](firmware/esp32p4-dongle/PLAN.md), milestones M0 (two-port gate) through M6.


### [2026-10-25] - Dongle Control Plane Built and Tested Without a Mouse
- **Built** (`firmware/esp32p4-dongle/`, ESP-IDF v5.5.4, firmware 0.2.0): shared headers [`common/amv_octant.h`](common/amv_octant.h) (the scaling math) and [`common/amv_dongle_proto.h`](common/amv_dongle_proto.h) (protocol v1: 64-byte frames, 8-byte header, async `EVENT_STATE`); components `amv_store` (NVS slots, CRC32), `amv_ctrl` (state owner, generation counter, lock-free seqlock read for the hot path, 2 s debounced auto-save), `amv_proto` (command handler task) and `amv_usb_device` (PHY swap + composite HID). Partition table now has two OTA slots.
- **Tested** with [`tools/dongle_test.py`](firmware/esp32p4-dongle/tools/dongle_test.py) over the real USB path: **44/44 checks** passed. Covered: bad magic/length/command; factory reset; valid, invalid (0, 6.0, negative, NaN, Inf) and stale writes; device-originated change arriving as an event; bypass; slot save/load/empty/out-of-range; 282 scaling steps against an independent float64 reference (all exact, cumulative drift 0); persistence of weights and active slot across a reboot. Control round trip about 2 ms median (macOS host).
- **Host-side math tests:** [`tests/test_octant.c`](firmware/esp32p4-dongle/tests/test_octant.c) passes on the Mac (identity at 1.0, octant tags, no lost sub-pixel counts, continuous blend across octant boundaries).
- **Observation:** right after a hard reset the Serial/JTAG unit (`303A:1001`) briefly appeared on the "other" USB-C before the app swapped the PHY, which suggests a reset restores the default mapping. Not yet confirmed with a BOOT-held download-mode test.
- **Design change:** the hot-path weight read is a seqlock rather than the double buffer first planned (a double buffer can hand a reader a buffer that is being rewritten after two quick updates).
- **Caveats:** `TEST_SCALE` and `TEST_SELF_CHANGE` are debug-only commands to gate out of release builds; `src/evdev_rt.c` has not been switched to the shared header (cannot be built or tested off-Linux); scaled motion is not yet forwarded to the PC mouse report because there is no input source yet.

### [2026-10-25] - AnyMouse Studio (WebHID) Built; Firmware 0.3.0
- **Web app** ([`web/`](web/README.md)): a static, no-build WebHID page. Radar chart with draggable handles (its outline is the real cosine-blended envelope), weight sliders and number boxes, presets, master scale, left/right and up/down links, four profile slots with filled/active state, JSON export/import (also reads Linux-engine profiles), bypass, a developer panel, and a built-in `?demo` mode with a simulated dongle. Edits are coalesced to about 30 per second; a stale edit is dropped and the page refreshes; events from other clients update the page.
- **Protocol/firmware additions (0.3.0):** `GET_SLOTS` (which slots hold a saved profile) and `AMV_USB_VID/PID` in [`common/amv_dongle_proto.h`](common/amv_dongle_proto.h). [`web/proto.js`](web/proto.js) is generated from that header by [`gen_proto_js.py`](firmware/esp32p4-dongle/tools/gen_proto_js.py) and checked for staleness by the tests.
- **Bug found by recording real frames and fixed:** a refused `SAVE_PROFILE`/`LOAD_PROFILE` (bad slot) replied with uninitialized stack bytes instead of the current state. `amv_ctrl` now always returns the current state on refusals; `dongle_test.py` has a regression check (48/48 pass). The recording also exposed wrong byte offsets for the MAC and uptime in the web client and demo device, now fixed.
- **Tests:** 16 Node tests ([`web/tests/`](web/tests/protocol.test.mjs)). They replay 27 exchanges recorded from the real board ([`capture_fixtures.py`](firmware/esp32p4-dongle/tools/capture_fixtures.py)): requests must encode byte-identically, and the demo device must answer byte-identically (including the scaling output). The demo mode renders correctly in headless Chrome (screenshot checked).
- **Not yet verified:** the real WebHID permission prompt and dragging the chart in a real browser against the board; DFU and the live calibration mirror are not in the web app yet.
- **Unrelated:** an SSH key for pushing from this Mac was created at the user's request (`~/.ssh/id_ed25519_anymouse`); the remote is still HTTPS.

### [2026-10-25] - License Adopted (AGPL-3.0) and On-Device Space Budget Measured
- **License:** the repository is now AGPL-3.0 ([`LICENSE`](LICENSE)), matching the OpenMouse protocol library so its codecs and device catalog can be used directly. All three mouse-information layers (universal info, known-vendor drivers, catalog of names) ship together; see [PLAN.md section 9.1a](firmware/esp32p4-dongle/PLAN.md#91a-decisions-2026-10-25).
- **Space check on the board** (a throwaway build with the USB host stack and hubs, flashed and read back, then the real firmware restored and re-tested at 48/48): the image grows from 283 KB to 352 KB (+69 KB, 17% of a 2 MB slot) and free internal RAM drops from 559 KB to 553 KB. Porting every OpenMouse family to C would still fit (estimated 0.7 to 1.0 MB total). Details and estimates are in [PLAN.md section 9.4](firmware/esp32p4-dongle/PLAN.md#94-space-and-memory-budget-measured-2026-10-25).
- **Open question answered:** this ESP-IDF (v5.5.4) supports USB hubs through `CONFIG_USB_HOST_HUBS_SUPPORTED`; it still needs a real hub and mice to confirm (checklist test 10).
- **Recommended, not yet done:** enlarge the two OTA slots from 2 MB to 4 MB in `partitions.csv`, because the layout can only be changed by a USB reflash.
### [2026-10-26] - Desktop App Links to the Dongle by Protocol Handshake (Firmware 0.4.0)
- **Gap found:** `./anymouse` had no dongle support. It would have grabbed the dongle's HID mouse as an ordinary mouse and scaled its already-vectored motion a second time.
- **Identity by protocol, not USB IDs:** new `HELLO` command (nonce in, `AMVD` identity word + nonce + capabilities + MAC out). The Linux app treats any hidraw interface on usage page `0xFF4D` as a *candidate* and accepts it only after the handshake, so production VID:PIDs can change freely. Firmware older than HELLO is recognised by a well-formed `GET_INFO`.
- **Guard (`src/devices.c`, `src/evdev_rt.c`):** the mouse interface of a candidate is never auto-grabbed unless the candidate fails the handshake (then it is an ordinary device) or reports that its output is not vectored. Unopenable or silent candidates keep guarding (fail safe). An explicit choice of the dongle's mouse is refused with a clear message.
- **Link (`src/dongle.c`, new thread):** adopts the dongle's weights on connect, pushes local edits with `SET_WEIGHTS` (same 0.01..5.0 rule as the firmware, checked on the float bits because the engine builds with `-ffast-math`), applies `EVENT_STATE` from other clients (the web page) and resolves `STALE` in the dongle's favour. GUI shows "Vectoring: Dongle" with generation and bypass; CLI gains `--dongle-status` and `--dongle-bypass on|off`; `--install-udev` adds a rule for the AnyMouse manufacturer string.
- **Tests:** `tests/test_dongle_link.c` (`make test-dongle`, built and run on Linux through a container): 44 checks covering handshake classification, sync, stale handling, echo suppression, and the guard against a fake sysfs tree. The full app (GTK, RT engine) compiles on Linux. `dongle_test.py` gained 8 `HELLO` checks and passes **56/56 on the real board** flashed with 0.4.0. Web tests still pass (16).
- **Not yet verified:** the C client against the real board (the Mac has no hidraw, and the real-USB path needs the CachyOS box), and the udev manufacturer match. (The web page was moved to the same handshake later the same day, see the next entry.)

### [2026-10-26] - Web Studio on HELLO; Mouse Side Built (Parser, USB Host, Vector Output)
- **Web Studio uses the handshake:** `web/` now matches only the vendor usage page (`0xFF4D`) and calls `DongleClient.hello()` (random nonce, identity word, version check; legacy 0.3.x accepted through `GET_INFO`). The demo device answers `HELLO`; `capture_fixtures.py` records the real board's reply; 18 Node tests pass (2 new: replay of the real reply, and rejection of a wrong nonce, legacy and silent devices). Real-browser WebHID is still untried (the device chooser needs a human click).
- **`amv_hid_parse`** (`firmware/esp32p4-dongle/components/amv_hid_parse/`): pure-C report-descriptor parser (report IDs, 8/12/16-bit signed fields, usage ranges, Push/Pop, keyboard-in-same-descriptor flag, absolute pointers excluded). `tests/test_hid_parse.c`: 24 checks on hand-built descriptors in the shapes real mice use; runs on the Mac. Real descriptors captured by the dongle belong in `tests/descriptors/`.
- **`amv_usb_host`:** IDF `usb_host` on the High-Speed controller (`peripheral_map = 0`; esp_tinyusb keeps the Full-Speed one). Enumerates every device, reads each HID interface's report descriptor (and logs it as hex), claims only relative-motion mouse interfaces, sets idle 0 and report protocol, keeps 3 interrupt IN transfers in flight, handles unplug (buttons released, PC side untouched). Hubs enabled (`CONFIG_USB_HOST_HUBS_SUPPORTED`), control transfers up to 1024 bytes.
- **`amv_vector` + PC side:** each native report is scaled with its own sub-pixel state, summed, and sent as one frame per 1 ms slot (woken by feed, by the previous report finishing, and every tick); several mice merge (motion summed, buttons OR-ed); each change of the combined button state is queued and sent in its own frame; deltas beyond a frame are carried over. The PC mouse report is now 16 buttons, 16-bit X/Y, 8-bit wheel and pan (8 bytes), with a 3-byte clamped report when the host selects boot protocol.
- **Protocol/firmware:** `GET_INFO.has_mouse` is the number of attached mice; `HELLO` caps now include `AMV_CAP_MOUSE_HOST`. Firmware stays 0.4.0.
- **Verified on the board (no mouse attached):** it boots with the PC-side device attached and the High-Speed host up, no errors; `dongle_test.py` still passes 56/56; web tests 18/18.
- **Not verified (needs a mouse):** that the USB-A port powers a mouse (VBUS enable pin unknown; `AMV_USB_VBUS_EN_GPIO` is -1), that the host enumerates and reads at the native rate while the PC side stays up, and the whole of PLAN section 9.2.
- **First-run checklist for the real mouse** (UART and PC-facing USB-C connected, jumper on HOST, mouse or receiver in USB-A port 1):
  1. Read the console over the UART (`idf.py -p <UART port> monitor`, 115200). Expect `device VVVV:PPPP "name"`, one or more `if N: ... report descriptor` blocks with hex, `mouse: X .. bit ...`, and `vectoring started`. Every 5 s: `in=... frames=...` and `mouse 0: ... reports=...`.
  2. Save the printed hex descriptor to `firmware/esp32p4-dongle/tests/descriptors/<vendor>_<model>.hex` and add it to `tests/test_hid_parse.c`.
  3. Move the mouse: the cursor on the Mac should move; `raw` and `out` counts in the log should agree at weights 1.0 (bypass or neutral). Then change a weight in the Studio (or `dongle_test.py`) and see the effect.
  4. Note reports/s per mouse (compare with its polling rate), any `errors`, and whether clicks, wheel and side buttons work.
  5. If nothing enumerates: suspect VBUS. If it enumerates but there is no motion: compare the `mouse:` layout line with the descriptor. If the log says `not a relative mouse`, the parser rejected that interface: save its hex and add it as a test.

### [2026-10-26] - First Mouse Attempt (Razer): Nothing Enumerated; Schematic Answers VBUS
- **Result:** with a Razer mouse plugged into the dongle, the High-Speed host stayed at `0 device(s) enumerated` with no connect or port event, even with USB-host debug logging enabled (firmware now sets debug level for the `HUB`, `USBH`, `HCD DWC`, `ENUM` and `USB HOST` tags and prints the library's device count every 5 s). The PC-side device and the 56 control-plane checks are unaffected.
- **Schematic (rendered from Waveshare's PDF):** USB-A VBUS switch `DIO7003` has EN tied to 5 V, so VBUS is always on and no enable GPIO exists; BOOT is GPIO35. The mux `U15` sends the P4's HS pair either to the CH334F hub (jumper fitted, three sockets: J2 both, J8 first) or straight to J8's second socket (no jumper). See PLAN section 0. The earlier assumption "HOST jumper = direct port 1" is unconfirmed.
- **Resolved the same day, see the next entry:** the first attempt failed because of socket/jumper position, not the firmware.

### [2026-10-26] - First Real Mouse: Razer Basilisk Ultimate Enumerated, Read and Vectored
- **Bring-up path:** with nothing enumerating, a read-only peek at the High-Speed controller's port register (`HPRT`, now logged on every change by a small diagnostic task and in the 5 s status line) showed power on and both data lines low. A host-only build (PC-side device off, `-DAMV_HOST_ONLY`) ruled out interference between the two USB stacks. A sweep of the four sockets and the jumper states then produced real events: the Razer connecting at full speed on one socket, and a High-Speed device with no HID interfaces in the other jumper position (the CH334 hub).
- **Hardware facts settled:** USB-A VBUS is always on (switch EN tied to 5 V, no GPIO); BOOT is GPIO35; **jumper on HOST = the direct port, the lower socket next to the Ethernet jack**; jumper on DEVICE = the CH334 hub ports (the hub enumerates; a mouse behind it is untested). Both caps move as a pair. My first schematic reading of the polarity was wrong; PLAN section 0 now records the measured behaviour.
- **Result with the Razer Basilisk Ultimate (`1532:0086`, wired, full speed, interval 1 ms):** enumerates in under 0.3 s, interface 0 parsed as a mouse (X/Y 16-bit at bits 32/48, wheel at 24, 5 buttons, vendor fields skipped), interfaces 1 and 2 (macro-key keyboards) ignored as designed, 3 transfers in flight. About 1000 reports/s in and 1000 frames/s out, 0 errors, and `raw` equals `out` exactly at neutral weights. The Mac sees the dongle as a mouse: cursor, buttons and wheel work. The three real descriptors are in `firmware/esp32p4-dongle/tests/descriptors/`; `test_hid_parse.c` now has 30 checks including a decoded Basilisk report.
- **Studio in a real browser:** opened from `http://localhost:8765` in Chrome, connected to the board through the device chooser, showed the real state, and changing weights changed the cursor as expected ("working perfectly"). This closes the M4 "not yet tried in a browser" item.
- **Not yet done:** any mouse information in the UI (name, VID:PID, polling rate, CPI, battery). Done in the next entry. Also open: long-swipe count parity, 1000-click test, latency, the 30-minute run, 8 kHz receivers, a mouse behind the hub.

### [2026-10-26] - Mouse Information in Three Tiers (Firmware 0.5.0, Studio, Desktop App)
- **Why:** the Studio and `./anymouse` showed nothing about the attached mouse. Plan: PLAN section 9.
- **Protocol:** `GET_MOUSE`, `GET_MOUSE_CAPS`, `GET_MOUSE_DESC`, `SET_MOUSE_CPI`, `SET_MOUSE_POLLING`, async `EVENT_MOUSE`; new statuses `NO_MOUSE`, `UNSUPPORTED`, `DEVICE_ERR`. `web/proto.js` regenerated with the new groups (tiers, flags, drivers, ignore reasons).
- **Firmware (`amv_usb_host`):** per-interface registry (uid, descriptor copy, nominal rate from `bInterval`, 10 s measured-rate history), tier decision (known driver, generic, boot fallback, ignored with a reason), ignored devices kept open so their removal is seen, `EVENT_MOUSE` on attach, removal and change, per-VID:PID CPI labels in NVS for mice without a protocol. Control transfers are serialised so the setup task and the protocol task can share the bus.
- **OpenMouse drivers on the dongle** (AGPL-3.0, ported from `OpenMouse-Project/mouse-protocol`, cloned next to the repo for reading only): Razer (90-byte feature reports, per-product transaction id, XOR checksum, DPI/polling/battery/firmware), Finalmouse ULX and GearHub R3 (ported from `src/hid_driver.c`). Safety rule in code: probe and refresh are read-only; writes only for models in a `VERIFIED` list, which holds the Basilisk Ultimate wired (`1532:0086`) after it was verified through the dongle. Finalmouse and GearHub stay read-only until their hardware is at hand.
- **First real readings (Razer Basilisk Ultimate, wired):** firmware 1.7, 1600 CPI, 1000 Hz, battery 100%, CPI range 100 to 20000 step 50, rates 125/500/1000 Hz. Writing back the unchanged CPI took about 85 ms and raised `EVENT_MOUSE`.
- **Studio:** Mouse card (tier badge, polling nominal/set/measured, CPI with its source, battery, firmware, layout, controls only where writable, descriptor download, notes for ignored and fallback devices); the Weights card's effective-DPI follows the mouse's real CPI; the page refreshes every 2 s and on `EVENT_MOUSE`; demo mode shows a Razer, a generic mouse and a keyboard. 21 Node tests. Confirmed in Chrome by the user: card contents, measured rate, DPI-button changes, descriptor download, unplug and replug.
- **Desktop app:** `src/dongle.c` caches the list and runs mouse writes on the link thread; `hid_query_active_mouse`, `hid_set_mouse_cpi` and `hid_set_mouse_polling_rate` go through the dongle when one is linked and fall back to direct hidraw otherwise; the GUI refreshes when the list changes; `--dongle-status` prints the mice. The whole app compiles on Linux and `make test-dongle` passes 61 checks against a fake dongle. **Not run against the real board over hidraw yet (needs the CachyOS box).**
- **Board test:** `dongle_test.py` 68/68 with the Razer attached (new: list, count, descriptor fetched in pieces and checked, capabilities, refusals of bad rate and CPI).

### [2026-10-26] - Power-Cycle Check, Interrupted Stability Run, TODO Consolidated
- **Hand-verified by the user:** profiles saved in the Studio, the whole dongle unplugged and plugged back in: the Studio reconnected by itself and the last used profile was restored from flash. Everything built so far works together (Razer vectoring, mouse card, controls, profiles).
- **Stability run (not complete):** a 30-minute console log was started at 18:44 (fresh boot). It captured 15,224 reports from the Razer with 0 errors and no reboot of the firmware before the mouse was deliberately unplugged at about 3 minutes, a clean re-enumeration and re-read after replugging, and then ended when the serial port went away during the full power cycle. So the M0 exit test (30 minutes, no host errors) is **still open**; it needs an uninterrupted run with the mouse in use. IDF logs one `USB HOST: Enqueue URB error: ESP_ERR_INVALID_STATE` when a mouse disappears mid-read; it is harmless (the transfer is dropped and the unplug is handled).
- **Decisions:** the jumper stays on HOST (the hub path is not needed now). Deferred without harm: 4 MB OTA slots and gating the debug commands out of release builds (reasons in PLAN section 8). The user will run the PHY-mapping reset test. A wireless mouse through its receiver and the Linux/FpsAimForge items (calibration through the dongle, desktop app against the board, DFU) are TODO for later.
- **State of the docs:** PLAN section 8 is now the consolidated status and TODO list.

### [2026-10-26] - Plan: Every OpenMouse Device on the Dongle
- **Trigger:** on another computer a Logitech G Pro (Lightspeed receiver `046d:c547`) worked through the dongle and the self-hosted Studio, but showed as "Generic HID". Cause: only three OpenMouse families are ported (Razer, Finalmouse, GearHub), and Logitech HID++ lives on a vendor interface the dongle does not claim. The earlier summaries said "OpenMouse drivers" in a way that overstated this.
- **Plan written:** [PLAN.md section 10](firmware/esp32p4-dongle/PLAN.md#10-full-openmouse-coverage-on-the-dongle). Scope: every mouse family in OpenMouse (48 folders, about 100 driver classes at commit `326f292`, keyboards excluded), only DPI, polling rate, battery, LOD and Motion Sync. Approach: a catalog generated from OpenMouse so every model is named at once, a WebHID-shaped shim so C ports mirror the TypeScript, multi-interface claiming, generic settings in the protocol, receiver/asleep handling, and transcript tests generated by running OpenMouse's own clients against its test fakes. Phases: infrastructure, hardware on hand (Logitech, ULX, R3, Razer LOD), largest engines, long tail, upkeep.
- **Decisions pending:** the write policy for models not tested through the dongle, and whether to build a browser tunnel as a developer tool.

---

## Where We Left Off (2026-10-25 Handoff, ESP32-P4 dongle)

The active work is Phase 4 on the ESP32-P4. Everything about it is in [`firmware/esp32p4-dongle/`](firmware/esp32p4-dongle/README.md) (quickstart) and [`PLAN.md`](firmware/esp32p4-dongle/PLAN.md) (design); the project-level summary is README section 13.

**State:** M0 and M1 work with a real mouse (Razer Basilisk Ultimate), the control plane passes 56/56 checks, and the Studio has been used in a real browser. The board runs firmware 0.4.0 (adds the `HELLO` identity handshake; the Haltech sketch was overwritten, as agreed). The Linux app's dongle link (`src/dongle.c`, guard, CLI) is built and tested against a fake dongle on Linux, but not yet against the real board over hidraw (see the 2026-10-26 entry). The WebHID Studio (M4) is built in [`web/`](web/README.md) but not yet tried in a real browser against the board. ESP-IDF v5.5.4 is installed at `~/esp/esp-idf` on the Mac. See the dated entry above and [`firmware/esp32p4-dongle/README.md`](firmware/esp32p4-dongle/README.md) for how to rebuild and rerun the test.

**Detailed plan for the first session with real mice: [PLAN.md section 9](firmware/esp32p4-dongle/PLAN.md#9-mouse-discovery-device-info-and-hardware-verification)** (auto-discovery of all mice, immediate vectoring, OpenMouse info for the ULX and R3, generic fallback, 15-point acceptance checklist). The short version follows.

**Next session:** everything built so far works; the open and deferred items are listed in [PLAN.md section 8](firmware/esp32p4-dongle/PLAN.md#8-status-and-todo). In short: an uninterrupted 30-minute run and the hotplug and latency checks, the PHY-mapping reset test, then at home the Linux box, FpsAimForge calibration through the dongle, the Finalmouse and Attack Shark drivers, and DFU. Also: the rest of the section 9.2 checklist, `./anymouse --dongle-status` and the guard against the real board on the CachyOS box, calibration push through the dongle, and testing whether a reset restores the default PHY mapping.

The Linux-engine items below (the 2026-10-23 handoff) are unchanged and still valid.

## Earlier Handoff (2026-10-23, Linux engine)

### Current Working State
1. **Unified C Engine (`./anymouse`):**
   - Real-time `evdev` $\to$ `uinput` 8-octant Cosine $C^1$ vectoring with `EVIOCGID` cloning ($1:1$ KWin Wayland Flat profile parity).
   - Automatic mouse discovery with hotplug, a GUI mouse picker (auto / identify / manual path), and the chosen device persisted in `~/.config/anymouse/config`.
   - Native `/dev/hidraw` OpenMouse driver: Finalmouse ULX (`0x361D:0x0100`) CPI + polling rate; Attack Shark R3 (`0x3151:0x402D`) polling rate (CPI writes not implemented yet). All vendor HID I/O runs on a worker thread so the GUI never blocks.
   - Weights auto-restore on launch and auto-save on exit (`~/.config/anymouse/profile.json`).
   - Native GTK4 + Cairo Studio GUI with live Polar Radar Chart and $\mathbf{P}_1$ vs. $\mathbf{T}$ vector overlay.
2. **`FpsAimForge` (FAF) Phase 2 Integration:**
   - [`libanymouse_faf.so`](src/faf_plugin.c) + [`faf_launcher`](src/faf_launcher.c) are installed by `anymouse --install-faf` (or the GUI button), which finds FpsAimForge in any Steam library; the original binary is kept at `FpsAimForge.bin`, and a Steam update is detected as "needs repair".
   - Every flick is logged to `logs/flicks.csv`; scenarios run 60 s.
   - Custom calibration bundle [`faf/AnyMouse.bundle.json`](faf/AnyMouse.bundle.json) installed at `~/.local/share/FpsAimForge/bundles/AnyMouse.bundle.json` (`AnyMouse 8-Octant Center-Out`, `AnyMouse 8-Octant Center-Out Ghost`, `AnyMouse 8-Octant Wide Flick`, and `AnyMouse Calibration Suite` playlist).
   - KovaaK's UE4.26 code ([`src/kovaaks_offsets.h`](src/kovaaks_offsets.h)) preserved in standby mode behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing.

### Next Session Resume Steps
1. Start AnyMouse Studio: `sudo ./start_daemon.sh` (or `make run`). Your last weights are restored automatically.
2. Launch **`FpsAimForge`** from Steam and play **`AnyMouse 8-Octant Center-Out`** (or **`AnyMouse Calibration Suite`**).
3. Observe per-octant median landing ratios $\tilde{r}_k$ across rounds (they were within noise of 1.0 by round 8–12 on 2026-10-22) and check **Ghost** and **Wide Flick** with auto-adapt off. Weights save on exit; use `logs/flicks.csv` to review history.
4. Next engineering items: Attack Shark R3 CPI writes, then the Windows research spike.

---

## Upcoming Roadmap Checkpoints

### Checkpoint 1: Live In-Game `FpsAimForge` Multi-Round Calibration Tuning (Phase 2)
- [x] Consolidate daemon, HID driver, telemetry analyzer, and GUI into unified C binary (`./anymouse`).
- [x] Extract UE4.26 KovaaK's offsets (`src/kovaaks_offsets.h`) and preserve sunset implementation for future Windows testing.
- [x] Build and install native `FpsAimForge` in-process plugin (`libanymouse_faf.so`), Steam launcher wrapper (`faf_launcher`), and custom 8-octant scenario bundle (`AnyMouse.bundle.json`).
- [x] Verify live 3D ballistic submovement capture in Steam's `FpsAimForge` and resolve `/dev/shm` `fs.protected_regular` permissions.
- [ ] Run full multi-round convergence testing in `AnyMouse 8-Octant Center-Out` and fine-tune submovement thresholds / learning rate $\alpha$ if needed.

### Checkpoint 1.5: Universal Release (Linux → Windows)
- [x] Auto-detect any connected mouse; GUI dropdown + identify + manual fallback; hotplug.
- [x] Polling-rate control in the GUI (Finalmouse ULX and Attack Shark R3).
- [x] Steam library auto-detection and FpsAimForge plugin/bundle installer with Steam-update repair.
- [x] Hardware-test Attack Shark R3 polling-rate writes (1k ↔ 2k verified 2026-10-22).
- [x] Auto-save / auto-restore of the octant profile (`~/.config/anymouse/profile.json`).
- [ ] Implement R3 CPI writes (`SET 0x54` DPI table).
- [ ] Windows research spike: input interception method (Interception driver vs. virtual HID vs. vectoring-only), FpsAimForge on Windows, anti-cheat implications, `hidapi` port.

### Checkpoint 2: Attack Shark R3 On-Board Firmware (Phase 3)
- [ ] Test SWD pinout on Attack Shark R3 PCB.
- [ ] Evaluate ZMK/UFA (`aroum/ufa`) open firmware build.
- [ ] Explore binary trampoline injection on stock firmware via `morluto/rea` + Ghidra.

### Checkpoint 3: Universal USB Pass-Through Dongle (Phase 4, ESP32-P4, now the active priority)
- [x] Verify the target board (ESP32-P4 rev v1.3 on Waveshare ESP32-P4-Module-DEV-KIT) and write the plan ([`firmware/esp32p4-dongle/PLAN.md`](firmware/esp32p4-dongle/PLAN.md)).
- [x] M0: ESP-IDF v5.5.4 bring-up; PC-facing USB link; FS OTG device + HS OTG host enumerating simultaneously (done 2026-10-26 with a real mouse; the 30-minute stability run is still open).
- [x] M1 (working, exit tests partly open): 1 kHz passthrough with report-descriptor parsing; Razer Basilisk Ultimate: ~1000 reports/s in and out, 0 errors, raw = out at neutral. Still to measure: long-swipe parity, 1000-click test, latency, 8 kHz, hub.
- [x] Mouse discovery and device info in three tiers (OpenMouse drivers for known mice, generic fallback, boot fallback, ignored list) built 2026-10-26 and working in the Studio with a Razer; the desktop app side is built and tested against a fake dongle only. Still open: the rest of the 15-point hardware checklist: [PLAN.md section 9](firmware/esp32p4-dongle/PLAN.md#9-mouse-discovery-device-info-and-hardware-verification).
- [x] Section 10 phase A/B started (2026-10-26, firmware 0.6.0): OpenMouse-generated catalog of 810 ids, 4 MB OTA slots, settings/model protocol commands, vendor-interface claim with a WebHID-shaped transport, and a read-only Logitech HID++ driver verified on a G Pro (name, DPI, rate, battery, asleep state; shown correctly in the Studio). Still open: Studio/app UI for settings, Razer/Finalmouse LOD and Motion Sync, Logitech writes, transcript harness, remaining engines ([PLAN.md section 10.6](firmware/esp32p4-dongle/PLAN.md#106-status-2026-10-26-firmware-060)).
- [ ] M2: shared `common/amv_octant.h` vectoring + NVS profiles. (Header, seqlock weights, NVS slots and scaled deltas to the PC mouse report done and working; still to do: `src/evdev_rt.c` on the shared header.)
- [ ] M3: vendor HID protocol + `src/dongle.c` sync for live FpsAimForge calibration. (Firmware side done, 56/56 checks; Linux side built 2026-10-26: `src/dongle.c`, guard, CLI, GUI badge; real-board hidraw run and calibration push still to do.)
- [x] M4: WebHID web Studio built ([`web/`](web/README.md)) and tried in a real browser against the board (2026-10-26). Still to add: Mouse panel, DFU, live calibration mirror.
- [ ] M5: hardening: firmware update (DFU) over the vendor HID channel, app rollback, watchdog, 8-hour soak, docs.
- [ ] M6: stretch goals (vendor-command tunnel, extra downstream interfaces, Windows companion, identity cloning); see [PLAN.md M6](firmware/esp32p4-dongle/PLAN.md#m6--stretch-goals-after-m5-in-value-order).
