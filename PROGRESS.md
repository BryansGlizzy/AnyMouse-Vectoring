# AnyMouse-Vectoring: Development Progress & Milestones

Sister document to [`README.md`](file:///home/future/Desktop/AnyMouse-Vectoring/README.md) tracking operational checkpoints, active implementations, architecture decisions, and roadmap progression with chronological timestamps.

---

## Project Status Overview

| Phase | Description | Status | Current Milestone |
| :---: | :--- | :---: | :--- |
| **Phase 1** | OS-Level Vectoring Daemon (Linux `evdev` + `uinput`) | **Complete** | Consolidated into unified C binary (`./anymouse`) with `EVIOCGID` 1:1 KWin parity |
| **Phase 1.5** | Native Mouse Hardware Protocol Integration (`OpenMouse`) | **Complete** | Direct C `/dev/hidraw` telemetry & CPI write in [`src/hid_driver.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/hid_driver.c) |
| **Phase 2** | `FpsAimForge` (FAF) Native Telemetry Plugin & Ballistic Calibration (KovaaK's Sunset/Retained) | **Verified / Active** | Live 3D submovement capture verified in Steam `FpsAimForge` (`libanymouse_faf.so` + `/dev/shm/anymouse_faf_telemetry`) |
| **Phase 3** | Mouse MCU On-Board Firmware (Attack Shark R3) | **Planned** | Testing ZMK/UFA (`aroum/ufa`) and Ghidra trampoline patching |
| **Phase 4** | Universal Hardware USB Pass-Through Dongle (RP2040) | **Concept / Design** | Dual-core USB-Host MitM pass-through board |

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
- **Artifact Created:** Complete foundational documentation in [`README.md`](file:///home/future/Desktop/AnyMouse-Vectoring/README.md).

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
- **Milestone:** Created zero-dependency, low-latency POSIX C daemon ([`octant_daemon.c`](file:///home/future/Desktop/AnyMouse-Vectoring/octant_daemon.c)):
  - Interface directly with `<linux/input.h>` and `<linux/uinput.h>` using exclusive grab (`ioctl(EVIOCGRAB)`).
  - `SCHED_FIFO` real-time scheduling priority ($80$) and memory page locking (`mlockall`) for microsecond-level wake latency on CachyOS.
  - Batched kernel event draining: Reads up to $64$ events per `read()` syscall, transforms vectors in CPU registers, and flushes frame via a single `write()` syscall.
  - Non-blocking UDP IPC socket on `127.0.0.1:42069`: Listens for 32-byte updates (`float weights[8]`) mid-swipe with zero mutex contention.
- **Compilation:** Compiled with `gcc -O3 -march=native -ffast-math -flto -Wall -Wextra -lm` into an $18\text{ KB}$ native binary ([`octant_daemon`](file:///home/future/Desktop/AnyMouse-Vectoring/octant_daemon)).

---

### [2026-10-20 22:58] - Kernel Module Hot-Fix & Launcher Script
- **Challenge:** Running `octant_daemon` produced `open /dev/uinput: No such device` (`ENODEV`).
- **Root Cause Identified:** CachyOS kernel packages were upgraded to `7.2.10-1-cachyos`, but the active running kernel was `7.2.9-2-cachyos`. The package upgrade deleted `/usr/lib/modules/7.2.9-2-cachyos/`, causing `modprobe uinput` to fail on the un-rebooted system.
- **Resolution:**
  - Extracted the matching binary `uinput.ko` directly from `/var/cache/pacman/pkg/linux-cachyos-7.2.9-2-x86_64_v4.pkg.tar.zst`.
  - Created [`start_daemon.sh`](file:///home/future/Desktop/AnyMouse-Vectoring/start_daemon.sh) with automatic kernel module loading (`insmod uinput.ko`) and device auto-detection.
  - Updated [`Makefile`](file:///home/future/Desktop/AnyMouse-Vectoring/Makefile) with build and run targets (`make run-ulx`, `make run-shark`).
- **Verification:** Successfully executed `sudo ./start_daemon.sh`, verified hardware grab, and confirmed live UDP updates with `send_weights.py` (`vertical_boost` and `neutral` presets).

---

### [2026-10-20 23:05] - Interactive PyQt6 Polar Studio GUI
- **Milestone:** Implemented complete graphical calibration interface ([`gui.py`](file:///home/future/Desktop/AnyMouse-Vectoring/gui.py)):
  - **Interactive Polar Radar Widget:** Real-time polygon rendering of the 8-octant sensitivity shape with interactive mouse drag handles on all 8 spokes.
  - **Granular Controls:** 8 individual sliders and DoubleSpinBoxes with dynamic Effective DPI readouts ($DPI = CPI \times weight$).
  - **Hardware Sensor Configurator:** Selectable hardware baseline CPI ($1600$, $3200$, $6400$, etc.).
  - **Master Scaling & Symmetry Linking:** Global multiplier slider + toggles for Left/Right and Up/Down mirror symmetry.
  - **Preset Management:** One-click application of `Neutral`, `Vertical Boost`, `Horizontal Boost`, `Diagonal ULX`, and `Low`.
  - **Profile Persistence:** JSON profile export and import.
  - **Seamless IPC Integration:** Added `--gui` flag to [`send_weights.py`](file:///home/future/Desktop/AnyMouse-Vectoring/send_weights.py) and added `make gui` target. Every UI adjustment emits instantaneous UDP packets to port $42069$.

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
  - Created [`mouse_protocol.py`](file:///home/future/Desktop/AnyMouse-Vectoring/mouse_protocol.py): Scans `/sys/class/hidraw/`, queries hardware over raw Linux file descriptors, and decodes telemetry in $< 50\text{ ms}$.
  - Verified live on connected hardware:
    - Detected: `Finalmouse UltralightX (Serial: MA234903747) on /dev/hidraw6`
    - Sensor DPI: `3200 CPI`
    - Polling Rate: `2000 Hz`
    - Battery: `59% (3.88V)`
    - Motion Sync: `ON`
    - RSSI: `-19 dBm`
- **GUI Integration:**
  - Updated [`gui.py`](file:///home/future/Desktop/AnyMouse-Vectoring/gui.py) with dynamic hardware badges (device identity, polling rate, battery, Motion Sync, RSSI).
  - Physical CPI auto-sync: Automatically updates the locked sensor CPI dropdown and calculates true Effective DPI for all 8 octants.
  - Added direct hardware control buttons (`Set 3200 CPI`, `Set 6400 CPI`) to write physical sensor registers via HID reports directly from the app.
  - Background `QTimer` refreshes telemetry every 8 seconds.

---

### [2026-10-21 17:08] - Unified Multi-Threaded C Consolidation (`./anymouse`)
- **Milestone:** Eliminated all split Python scripts (`gui.py`, `mouse_protocol.py`, `send_weights.py`) and inter-process UDP requirements by consolidating the entire stack into a single $86\text{ KB}$ multi-threaded POSIX C binary ([`./anymouse`](file:///home/future/Desktop/AnyMouse-Vectoring/anymouse)):
  - **Shared Lock-Free Core ([`src/core.h`](file:///home/future/Desktop/AnyMouse-Vectoring/src/core.h), [`src/main.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/main.c)):** Uses `_Atomic float octant_weights[8]`, `_Atomic int hardware_cpi`, and a $65,536$-slot lock-free Single-Producer Single-Consumer (`SPSC`) ring buffer (`tick_ring`) in shared process memory.
  - **Thread 1 — Real-Time Vectoring Hot-Path ([`src/evdev_rt.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/evdev_rt.c)):** Runs at `SCHED_FIFO` priority 80 with `mlockall()`, performs `EVIOCGRAB`, executes Cosine $C^1$ interpolation and sub-pixel error diffusion (`scale_vector()`), writes to `/dev/uinput`, and pushes every `SYN_REPORT` frame to `tick_ring` lock-free.
  - **Native C OpenMouse `/dev/hidraw` Driver ([`src/hid_driver.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/hid_driver.c)):** Direct C port of the Finalmouse ULX (`0x361D:0x0100`) and Attack Shark R3 (`0x3151:0x402D`) HIDraw telemetry reader and hardware CPI writer (both preset buttons and CPI dropdown write directly to the mouse sensor).
  - **Thread 3 — Native GTK4 + Cairo Studio GUI ([`src/gui_gtk4.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/gui_gtk4.c)):** Hardware-accelerated Cairo polar radar chart with draggable handles, live $\mathbf{P}_1$ vs. $\mathbf{T}$ vector overlay, uniform octant scalar controls (`1:1 (1.0x)`, `Half (0.50x)`, `Quarter (0.25x)`), symmetry linkers, and JSON profile persistence.
- **Resolved KDE Plasma / KWin Wayland $2\times$ Acceleration Quirk:**
  - **Root Cause:** In `~/.config/kcminputrc`, the physical Finalmouse ULX (`Vendor=13853`, `Product=256`, `Name="Finalmouse UltralightX dongle Mouse"`) was configured with `PointerAccelerationProfile=1` (Flat / unaccelerated) and `PointerAcceleration=-0.200`. Because the virtual `uinput` device previously used `Product=0x0101` (`257`), KWin Wayland treated it as an unconfigured mouse and applied `libinput`'s default Adaptive acceleration curve (`Profile=2`, $2.0\times$ incline).
  - **Fix:** Updated [`setup_uinput_device()`](file:///home/future/Desktop/AnyMouse-Vectoring/src/evdev_rt.c) to clone the exact `EVIOCGID` (`bustype`, `vendor`, `product`, `version`) and `EVIOCGNAME` from the grabbed physical mouse so KWin Wayland applies the exact same Flat (`Profile=1`) settings for true $1:1$ sensitivity parity.

---

### [2026-10-21 17:55] - Phase 2 KovaaK's UE4.26 Static Analysis & Session-Gated Telemetry
- **Milestone:** Installed [`morluto/rea`](https://github.com/morluto/rea) (`v6.7.0`) and **Ghidra 12.1.2**, extracted UE4.26 (`++UE4+Release-4.26`) reflection tables from [`FPSAimTrainer-Win64-Shipping.exe`](file:///mnt/iscsi-nvme/SteamLibrary/steamapps/common/FPSAimTrainer/FPSAimTrainer/Binaries/Win64/FPSAimTrainer-Win64-Shipping.exe) (`Build 3.8.7`), and implemented strict session-gated round-to-round telemetry in [`src/kovaaks_offsets.h`](file:///home/future/Desktop/AnyMouse-Vectoring/src/kovaaks_offsets.h) and [`src/calibration.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/calibration.c).
- **Verified UE4.26 & `/Script/GameSkillsTrainer` Offsets:**
  - **Global Pointers (`.data`):** `GWorld` at RVA `0x05533750` (`VA 0x145533750`), `FNamePool` at RVA `0x053C3340` (`VA 0x1453C3340`).
  - **Engine Hierarchy:** `UWorld::PersistentLevel` (`0x30`), `UWorld::GameState` (`0x120`), `UWorld::OwningGameInstance` (`0x180`), `UGameInstance::LocalPlayers` (`0x38`), `UPlayer::PlayerController` (`0x30`), `AController::ControlRotation` (`0x288`), `APlayerController::AcknowledgedPawn` (`0x2A0`), `APlayerController::PlayerCameraManager` (`0x2B8`), `APlayerCameraManager::CameraCachePrivate.POV` (`0x1AF0`).
  - **`AMetaGameState` (`GameState`):** `TArray<AMetaCharacter*> Characters` at `+0x02F0` (enumerates the player character plus all spawned bot characters).
  - **`AMetaPlayerController` (`PlayerController`):** `UserYawSensitivity` (`+0x0608`), `UserPitchSensitivity` (`+0x060C`), `MyCharacter` (`+0x0618`), `PauseMenuLocation` (`+0x0678`, `0` during active unpaused gameplay), `LastEnemyAttacked` (`+0x06A8`).
  - **`AMetaCharacter` (Player & Bots):** `RootComponent` (`+0x0130` $\to$ `RelativeLocation` at `+0x011C`), `KillCount` (`+0x09C8`), `KillStatistics` (`+0x09D0`), `TotalOverShots` (`+0x09E0`), `DamageDone` (`+0x0A20`), `LastScore` (`+0x0A28`), `ChallengeStartTime` (`+0x0A38`, `FDateTime` ticks), `LastShotFiredTime` (`+0x0A78`), `Health` (`+0x0E58`, $> 0$ when bot is alive).
- **Strict Session Gating & Inter-Round Lifecycle ([`src/calibration.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/calibration.c)):**
  - Tracks 4 explicit states: `KOVAAKS_STATE_OFFLINE`, `KOVAAKS_STATE_MENU_OR_LOBBY` (`bot_count == 0`), `KOVAAKS_STATE_PAUSED` (`PauseMenuLocation != 0`), and `KOVAAKS_STATE_ACTIVE_ROUND` (`MyCharacter != NULL && bot_count > 0 && PauseMenuLocation == 0`).
  - **Zero Desktop False-Positives:** When not in `KOVAAKS_STATE_ACTIVE_ROUND`, `tick_ring` is immediately flushed (`ring_tail = ring_head`) so desktop mouse movements are never tracked.
  - **Round-to-Round Aggregation & Adaptation:** Detects Challenge round start/restart via `ChallengeStartTime` (`+0x0A38`) and `KillCount` (`+0x09C8`) transitions, and detects Challenge completion via `inotify` on `FPSAimTrainer/stats/*.csv`, applying damped weight updates ($\alpha = 0.35$, $\pm 5\%$ clamp) strictly **between rounds**.

---

### [2026-10-21 18:50] - Phase 2 Transition to `FpsAimForge` (FAF) Native Telemetry Plugin & Custom 8-Octant Suite
- **Milestone:** Transitioned primary Linux Phase 2 calibration from KovaaK's (due to Linux anti-cheat incompatibility; KovaaK's code in [`src/kovaaks_offsets.h`](file:///home/future/Desktop/AnyMouse-Vectoring/src/kovaaks_offsets.h) and [`src/calibration.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/calibration.c) is preserved/sunset behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing) to **`FpsAimForge` (`v0.7.0`)**, a native Linux C++23/SDL3 open-source aim trainer installed on Steam.
- **Native In-Process Telemetry Plugin ([`src/faf_plugin.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_plugin.c) $\to$ `libanymouse_faf.so`):**
  - Hooks `FpsAimForge`'s 13 `Scenario` / `BaseScenario` vtables in `.data.rel.ro` (`OnTickStart`, `OnDetach`, `UpdateState`, `OnScenarioDone`) in-process with zero external memory sniffing.
  - Installed native Steam launcher wrapper ([`src/faf_launcher.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_launcher.c)) at `/mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge/FpsAimForge` so `libanymouse_faf.so` loads automatically whether `FpsAimForge` is launched from Steam or directly.
  - Streams live scenario state (`FAF_STATE_MENU`, `FAF_STATE_WAITING_FOR_CLICK_TO_START`, `FAF_STATE_START_COUNTDOWN`, `FAF_STATE_RUNNING`, `FAF_STATE_DONE`), round timers, hit/shot/kill counters, 3D camera `(yaw, pitch)`, and active 3D target coordinates over POSIX shared memory (`/dev/shm/anymouse_faf_telemetry`, defined in [`src/faf_telemetry.h`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_telemetry.h)).
  - **True 3D Target-Aware Submovement Extraction:** Records $1000\text{ Hz}$ camera `(yaw, pitch)` trajectory during active rounds, matches each click/kill to the exact 3D target center $\mathbf{T}$ from `TargetManager::last_removed_target_`, isolates the primary ballistic submovement $\mathbf{P}_1$ (velocity drop $< 16\%$ of $\omega_{\text{peak}}$ or corrective velocity valley $< 42\%$ of $\omega_{\text{peak}}$), and emits verified `faf_submovement_event_t` packets into a lock-free ring buffer.
- **Custom `AnyMouse` Calibration Bundle ([`faf/AnyMouse.bundle.json`](file:///home/future/Desktop/AnyMouse-Vectoring/faf/AnyMouse.bundle.json)):**
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
  - **Fix:** Updated [`open_faf_shm()`](file:///home/future/Desktop/AnyMouse-Vectoring/src/calibration.c#L586-L618) in [`src/calibration.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/calibration.c) and [`faf_plugin_init()`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_plugin.c#L588-L607) in [`src/faf_plugin.c`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_plugin.c) to call `shm_open(..., O_RDWR, 0666)` **without `O_CREAT`** first, falling back to `O_CREAT | O_RDWR` only when `errno == ENOENT`, and updated [`Makefile`](file:///home/future/Desktop/AnyMouse-Vectoring/Makefile) to unlink (`rm -f`) before relinking shared libraries so live-mapped processes never receive `SIGBUS`.

---

### [2026-10-22] - Documentation Sync & First Commit of the Unified C Engine
- **Milestone:** Brought [`README.md`](file:///home/future/Desktop/AnyMouse-Vectoring/README.md) in line with the current architecture: removed the obsolete UDP/Unix-socket weight channel and the Raylib/Bevy/Godot trainer from the Section 7 diagram and Phase 1 notes (weights now live in process memory as `_Atomic float octant_weights[8]`), and fixed the Section 12 table-of-contents entry to say `FpsAimForge`.
- **Repository:** Committed the unified C engine (`src/`), the `FpsAimForge` bundle (`faf/`), and the removal of the legacy Python/`octant_daemon.c` files. Files named in earlier log entries (`gui.py`, `mouse_protocol.py`, `send_weights.py`, `octant_daemon.c`) are kept in that history as the prototype stage and no longer exist in the tree.

### [2026-10-22] - Flick CSV Log, GUI Auto-Adapt Sync & Longer Rounds
- Added a persistent per-flick CSV log (`logs/flicks.csv`, see README) so calibration history survives the 256-slot ring and restarts.
- Fixed the GUI so the "Inter-Round Auto-Adapt" checkbox reflects `--auto-calibrate` at startup.
- Extended all three `AnyMouse` scenarios from 45 s to 60 s to reach the 5-trial per-octant gate more reliably.

---

## Where We Left Off (2026-10-21 Nightly Handoff)

### Current Working State
1. **Unified C Engine ([`./anymouse`](file:///home/future/Desktop/AnyMouse-Vectoring/anymouse)):**
   - Real-time `evdev` $\to$ `uinput` 8-octant Cosine $C^1$ vectoring with `EVIOCGID` cloning ($1:1$ KWin Wayland Flat profile parity).
   - Native `/dev/hidraw` OpenMouse driver for Finalmouse ULX (`0x361D:0x0100`) and Attack Shark R3 (`0x3151:0x402D`) with working CPI buttons and dropdown.
   - Native GTK4 + Cairo Studio GUI with live Polar Radar Chart and $\mathbf{P}_1$ vs. $\mathbf{T}$ vector overlay.
2. **`FpsAimForge` (FAF) Phase 2 Integration:**
   - [`libanymouse_faf.so`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_plugin.c) + [`faf_launcher`](file:///home/future/Desktop/AnyMouse-Vectoring/src/faf_launcher.c) installed into `/mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge/FpsAimForge` (original binary backed up at `FpsAimForge.bin`).
   - Custom calibration bundle [`faf/AnyMouse.bundle.json`](file:///home/future/Desktop/AnyMouse-Vectoring/faf/AnyMouse.bundle.json) installed at `~/.local/share/FpsAimForge/bundles/AnyMouse.bundle.json` (`AnyMouse 8-Octant Center-Out`, `AnyMouse 8-Octant Center-Out Ghost`, `AnyMouse 8-Octant Wide Flick`, and `AnyMouse Calibration Suite` playlist).
   - KovaaK's UE4.26 code ([`src/kovaaks_offsets.h`](file:///home/future/Desktop/AnyMouse-Vectoring/src/kovaaks_offsets.h)) preserved in standby mode behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing.

### Next Session Resume Steps
1. Start AnyMouse Studio: `sudo ./start_daemon.sh` (or `make run`).
2. Launch **`FpsAimForge`** from Steam and play **`AnyMouse 8-Octant Center-Out`** (or **`AnyMouse Calibration Suite`**).
3. Observe per-octant median landing ratios $\tilde{r}_k$ populate across all 8 octants during active rounds and test **Inter-Round Auto-Adapt** ($\alpha = 0.35$, $\pm 5\%$ clamp) across consecutive rounds, then save the converged weights to `profile.json`.

---

## Upcoming Roadmap Checkpoints

### Checkpoint 1: Live In-Game `FpsAimForge` Multi-Round Calibration Tuning (Phase 2)
- [x] Consolidate daemon, HID driver, telemetry analyzer, and GUI into unified C binary (`./anymouse`).
- [x] Extract UE4.26 KovaaK's offsets (`src/kovaaks_offsets.h`) and preserve sunset implementation for future Windows testing.
- [x] Build and install native `FpsAimForge` in-process plugin (`libanymouse_faf.so`), Steam launcher wrapper (`faf_launcher`), and custom 8-octant scenario bundle (`AnyMouse.bundle.json`).
- [x] Verify live 3D ballistic submovement capture in Steam's `FpsAimForge` and resolve `/dev/shm` `fs.protected_regular` permissions.
- [ ] Run full multi-round convergence testing in `AnyMouse 8-Octant Center-Out` and fine-tune submovement thresholds / learning rate $\alpha$ if needed.

### Checkpoint 2: Attack Shark R3 On-Board Firmware (Phase 3)
- [ ] Test SWD pinout on Attack Shark R3 PCB.
- [ ] Evaluate ZMK/UFA (`aroum/ufa`) open firmware build.
- [ ] Explore binary trampoline injection on stock firmware via `morluto/rea` + Ghidra.

### Checkpoint 3: Universal USB Pass-Through Dongle (Phase 4)
- [ ] Design RP2040 / RP2350 USB-Host pass-through firmware.
- [ ] Dual-core processing: Core 0 for 10-15 µs vector mutation, Core 1 for WebHID / RawHID tuning.
