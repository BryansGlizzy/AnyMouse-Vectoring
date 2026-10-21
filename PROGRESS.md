# AnyMouse-Vectoring: Development Progress & Milestones

Sister document to [`README.md`](file:///home/future/Desktop/AnyMouse-Vectoring/README.md) tracking operational checkpoints, active implementations, architecture decisions, and roadmap progression with chronological timestamps.

---

## Project Status Overview

| Phase | Description | Status | Current Milestone |
| :---: | :--- | :---: | :--- |
| **Phase 1** | OS-Level Vectoring Daemon (Linux `evdev` + `uinput`) | **Complete** | Core daemon, live UDP IPC, and PyQt6 Studio GUI operational |
| **Phase 1.5** | Native Mouse Hardware Protocol Integration (`OpenMouse`) | **Complete** | Direct `/dev/hidraw` telemetry & CPI sync for Finalmouse & Attack Shark |
| **Phase 2** | 2D Center-Out Ballistic Calibration Drill | **Next Milestone** | Telemetry capture for primary submovement inflection ($v(t) < 0.15 \cdot v_{\text{peak}}$) |
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

## Upcoming Roadmap Checkpoints

### Checkpoint 1: Phase 2 Center-Out Calibration Trainer
- [ ] Build standalone 2D target acquisition drill (Raylib or Godot/Bevy).
- [ ] Implement 40-flick randomized protocol with $\pm 8^\circ$ jitter around octant radials.
- [ ] High-frequency mouse position logging and velocity smoothing ($5\text{ ms}$ moving average).
- [ ] Detect primary submovement inflection coordinate $\mathbf{P}_1$ at $v(t) < 0.15 \cdot v_{\text{peak}}$.
- [ ] Calculate median radial landing ratio $\tilde{r}_k$ per octant and stream updated weights to UDP port $42069$ after each round.

### Checkpoint 2: Attack Shark R3 On-Board Firmware (Phase 3)
- [ ] Test SWD pinout on Attack Shark R3 PCB.
- [ ] Evaluate ZMK/UFA (`aroum/ufa`) open firmware build.
- [ ] Explore binary trampoline injection on stock firmware via `morluto/rea` + Ghidra.

### Checkpoint 3: Universal USB Pass-Through Dongle (Phase 4)
- [ ] Design RP2040 / RP2350 USB-Host pass-through firmware.
- [ ] Dual-core processing: Core 0 for 10-15 µs vector mutation, Core 1 for WebHID / RawHID tuning.
