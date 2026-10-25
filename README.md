# AnyMouse-Vectoring: Dynamic Mouse DPI Calibration

Anisotropic, 8-octant polar sensitivity calibration driven by real-time ballistic flick telemetry. This project bridges biomechanical wrist/forearm asymmetry with low-latency mouse vector scaling across OS-level interception, custom MCU firmware, and hardware man-in-the-middle pass-through adapters.

### Documentation Map

| Document | What it covers |
|:---|:---|
| **README.md** (this file) | Concept, math, calibration algorithm, the Linux engine (`./anymouse`), and the hardware-dongle overview |
| [PROGRESS.md](PROGRESS.md) | Dated log of milestones, current status per phase, handoff notes and the roadmap checklist |
| [web/README.md](web/README.md) | AnyMouse Studio, the no-install WebHID front-end for the dongle: run, use, files, tests |
| [firmware/esp32p4-dongle/README.md](firmware/esp32p4-dongle/README.md) | ESP32-P4 dongle quickstart: cabling, build, flash, recovery |
| [firmware/esp32p4-dongle/PLAN.md](firmware/esp32p4-dongle/PLAN.md) | ESP32-P4 dongle design: verified hardware, architecture, milestones M0 to M6, risks |
| [firmware/esp32p4-dongle/spikes/m0_phy_swap/README.md](firmware/esp32p4-dongle/spikes/m0_phy_swap/README.md) | The M0 proof-of-concept: PC-facing USB on the board's second USB-C |
| [faf/AnyMouse.bundle.json](faf/AnyMouse.bundle.json) | The `FpsAimForge` calibration scenarios |

---

## Table of Contents
1. [Project Overview & Core Goal](#1-project-overview--core-goal)
2. [Biomechanical Motivation](#2-biomechanical-motivation)
3. [Sensor Hardware Realities: Why Vector Scaling?](#3-sensor-hardware-realities-why-vector-scaling)
4. [Mathematical Foundations & Formulas](#4-mathematical-foundations--formulas)
   - [4.1 Coordinate Space & Octant Mapping](#41-coordinate-space--octant-mapping)
   - [4.2 Continuous Angular Interpolation (Cosine C¹ Continuity)](#42-continuous-angular-interpolation-cosine-c-continuity)
   - [4.3 Sub-Pixel Remainder Accumulation (Error Diffusion)](#43-sub-pixel-remainder-accumulation-error-diffusion)
   - [4.4 Reference Implementation (C / Embedded C)](#44-reference-implementation-c--embedded-c)
5. [Telemetry & Calibration Algorithm (Phase 2)](#5-telemetry--calibration-algorithm-phase-2)
   - [5.1 Primary Ballistic Submovement vs. Closed-Loop Correction](#51-primary-ballistic-submovement-vs-closed-loop-correction)
   - [5.2 Radial Landing Ratio ($r_i$) & Lateral Deviation](#52-radial-landing-ratio-r_i--lateral-deviation)
   - [5.3 Damped Learning Rate & Inter-Round Weight Update Rule](#53-damped-learning-rate--inter-round-weight-update-rule)
   - [5.4 Center-Out Drill Protocol](#54-center-out-drill-protocol)
6. [Why Motion Sync is Mandatory](#6-why-motion-sync-is-mandatory)
7. [System Architecture & 4-Phase Roadmap](#7-system-architecture--4-phase-roadmap)
   - [Phase 1: OS-Level Input Interception (CachyOS / Linux evdev + uinput)](#phase-1-os-level-input-interception-cachyos--linux-evdev--uinput)
   - [Phase 2: 2D Radial Calibration Engine](#phase-2-2d-radial-calibration-engine)
   - [Phase 3: Mouse MCU Firmware Integration (Attack Shark R3 / nRF52840)](#phase-3-mouse-mcu-firmware-integration-attack-shark-r3--nrf52840)
   - [Phase 4: Universal USB-HID Hardware Pass-Through (ESP32-P4 MitM Dongle)](#phase-4-universal-usb-hid-hardware-pass-through-esp32-p4-mitm-dongle)
8. [Reverse Engineering Proprietary Firmware (`morluto/rea`)](#8-reverse-engineering-proprietary-firmware-morlutorea)
   - [Reconstruction Pitfalls vs. Binary Trampoline Patching](#reconstruction-pitfalls-vs-binary-trampoline-patching)
   - [Workflow with Ghidra & Vendor DFU Updaters](#workflow-with-ghidra--vendor-dfu-updaters)
9. [Hardware Profiles & Linux Configuration Setup](#9-hardware-profiles--linux-configuration-setup)
   - [Device Telemetry & Verified USB Hardware IDs](#device-telemetry--verified-usb-hardware-ids)
   - [WebHID Browser Requirements & Sandboxing Warnings](#webhid-browser-requirements--sandboxing-warnings)
   - [CachyOS / Arch udev Configuration (`70-mouse-webhid.rules`)](#cachyos--arch-udev-configuration-70-mouse-webhidrules)
   - [Sniffing Protocol via WebHID](#sniffing-protocol-via-webhid)
10. [Developer & Agent Context Reference](#10-developer--agent-context-reference)
11. [Unified C Engine Architecture (`./anymouse`)](#11-unified-c-engine-architecture-anymouse)
12. [Native GTK4 + Cairo Studio & `FpsAimForge` Phase 2 Telemetry](#12-native-gtk4--cairo-studio--fpsaimforge-phase-2-telemetry)
13. [ESP32-P4 Hardware Dongle (Phase 4)](#13-esp32-p4-hardware-dongle-phase-4)

---

## 1. Project Overview & Core Goal

Traditional mouse sensitivity allows only a single uniform scalar or, at best, asymmetric X/Y sensitivity multipliers. However, human motor control is non-orthogonal. 

**The Goal:** Build an adaptive, 8-octant polar sensitivity calibration system that dynamically scales raw mouse displacement vectors based on trajectory angle. A dedicated calibration drill aggregates submovement telemetry from rounds of ballistic flicks, identifies directional overshoot and undershoot across 8 radial compass directions, and optimizes an 8-point weight profile.

The system is designed to scale across three tiers:
1. **Software Prototype (Linux/CachyOS):** Low-latency kernel input capture via `evdev` and `uinput`.
2. **On-Device Firmware:** Direct execution inside mouse MCUs (Nordic nRF52840).
3. **Universal Hardware Dongle:** A transparent USB pass-through device (ESP32-P4, see [Section 13](#13-esp32-p4-hardware-dongle-phase-4)) running hardware-level vector interpolation for any commercial mouse without host software or driver requirements.

---

## 2. Biomechanical Motivation

Standard mouse tracking assumes uniform physical resistance across all angles of movement. In human anatomy, this assumption is false:
- **Radial vs. Ulnar Deviation:** Moving the wrist laterally toward the thumb (radial) provides significantly less angular range of motion and different tendon resistance than moving toward the pinky (ulnar).
- **Flexion vs. Extension:** Wrist flexion (pulling toward the palm / downwards) engages stronger flexor muscles than wrist extension (pushing upwards).
- **Forearm Pronation / Supination & Elbow Pivoting:** Diagonal movements (e.g., flicking Up-Right vs. Up-Left for a right-handed player) combine wrist deviation with forearm pronation/supination and elbow extension in asymmetric ratios.

Standard 2-axis (X/Y) scaling cannot correct diagonal asymmetries. An **8-point polar sensitivity map** ($E, NE, N, NW, W, SW, S, SE$) provides the minimum basis set required to compensate for anatomical pivot asymmetries.

---

## 3. Sensor Hardware Realities: Why Vector Scaling?

### The Trap: Modifying Physical Sensor Registers On the Fly
Modern optical sensors (e.g., PixArt PAW3395, PAW3950, PMW3360) configure their physical Counts Per Inch (CPI) via internal registers over an SPI bus:
- Changing CPI mid-swipe requires writing to sensor SPI registers in discrete steps (typically 50 CPI increments).
- Doing this dynamically at 1,000 Hz – 8,000 Hz polling rates causes **SPI bus contention**, introducing sensor frame stalls, packet drops, and timing jitter.
- The optical sensor hardware itself only supports independent X and Y resolution registers—it has no native concept of polar directions or radial angles.

### The Solution: Fixed High CPI + Microcontroller Vector Scaling
1. Lock the physical optical sensor to a **high native resolution** (e.g., **3,200 CPI** or **6,400 CPI**).
2. The sensor outputs dense raw integer delta counts $(\Delta x_{\text{raw}}, \Delta y_{\text{raw}})$ to the MCU at full polling frequency.
3. The MCU (or interception daemon) executes continuous polar angle calculation, trigonometric interpolation, and sub-pixel remainder accumulation per tick before emitting the final USB HID report.
4. Baseline scaling factor converts high native CPI down to standard operational sensitivity (e.g., $3200 \times 0.25 = 800\text{ effective DPI}$).

---

## 4. Mathematical Foundations & Formulas

### 4.1 Coordinate Space & Octant Mapping
Screen coordinates invert the vertical axis (moving the mouse UP yields negative $\Delta y$). Standard Cartesian polar angle conversion requires negating $\Delta y$:

$$\theta = \text{atan2}(-\Delta y_{\text{raw}}, \Delta x_{\text{raw}})$$

Ensure $\theta$ is normalized to the range $[0, 2\pi)$:

$$\text{if } \theta < 0 \implies \theta \leftarrow \theta + 2\pi$$

The circle is partitioned into 8 octants spaced by $\Delta \theta = \frac{\pi}{4}$ (45° or 0.785398 rad):

| Index ($k$) | Direction | Angle (Degrees) | Angle (Radians) | Biomechanical Notes |
|:---:|:---:|:---:|:---:|:---|
| **0** | **E** (East) | $0^\circ$ | $0$ | Pure rightward lateral swipe |
| **1** | **NE** (North-East) | $45^\circ$ | $\pi/4$ | Extension + radial/ulnar deviation |
| **2** | **N** (North) | $90^\circ$ | $\pi/2$ | Pure upward push (wrist extension / finger push) |
| **3** | **NW** (North-West) | $135^\circ$ | $3\pi/4$ | Extension + counter deviation |
| **4** | **W** (West) | $180^\circ$ | $\pi$ | Pure leftward lateral swipe |
| **5** | **SW** (South-West) | $225^\circ$ | $5\pi/4$ | Flexion + inward pull |
| **6** | **S** (South) | $270^\circ$ | $3\pi/2$ | Pure downward pull (wrist flexion) |
| **7** | **SE** (South-East) | $315^\circ$ | $7\pi/4$ | Flexion + outward pull |

---

### 4.2 Continuous Angular Interpolation (Cosine $C^1$ Continuity)

#### Why Hard Wedges Fail
Partitioning the space into hard $45^\circ$ wedges causes discontinuous sensitivity jumps when crossing boundaries (e.g., transitioning from $22.4^\circ$ to $22.6^\circ$). This manifests as tactile stepping and perceived pixel skipping.

#### Interpolation Algorithm
1. Determine continuous octant position:
   $$\text{octant\_pos} = \frac{\theta}{\pi / 4}$$
2. Identify bounding anchor indices:
   $$k_0 = \lfloor \text{octant\_pos} \rfloor \pmod 8$$
   $$k_1 = (k_0 + 1) \pmod 8$$
3. Extract fractional parameter $t \in [0.0, 1.0)$:
   $$t = \text{octant\_pos} - \lfloor \text{octant\_pos} \rfloor$$
4. Compute smooth parameter using Cosine S-Curve interpolation ($C^1$ continuous derivative across boundaries):
   $$\tilde{t} = \frac{1 - \cos(t \cdot \pi)}{2}$$
5. Compute effective interpolated multiplier $S(\theta)$:
   $$S(\theta) = M_{k_0} \cdot (1 - \tilde{t}) + M_{k_1} \cdot \tilde{t}$$

*(Note: Linear interpolation $S(\theta) = M_{k_0}(1-t) + M_{k_1}t$ can serve as a lower-computation fallback, but cosine smoothing eliminates gradient kinks at anchor points).*

---

### 4.3 Sub-Pixel Remainder Accumulation (Error Diffusion)

USB HID mouse packets accept only signed integers (`int16_t`). Direct truncation of scaled values causes small micro-movements to vanish entirely (e.g., $1 \times 0.25 = 0.25 \xrightarrow{\text{trunc}} 0$), resulting in dead zones during slow micro-aiming.

To preserve sub-pixel fidelity, maintain floating-point (or fixed-point Q16.16) remainder accumulators ($\text{rem}_x, \text{rem}_y$) across ticks:

$$\text{exact}_x = (\Delta x_{\text{raw}} \cdot S(\theta)) + \text{rem}_x$$
$$\text{exact}_y = (\Delta y_{\text{raw}} \cdot S(\theta)) + \text{rem}_y$$

$$\Delta x_{\text{out}} = \text{trunc}(\text{exact}_x)$$
$$\Delta y_{\text{out}} = \text{trunc}(\text{exact}_y)$$

$$\text{rem}_x = \text{exact}_x - \Delta x_{\text{out}}$$
$$\text{rem}_y = \text{exact}_y - \Delta y_{\text{out}}$$

---

### 4.4 Reference Implementation (C / Embedded C)

```c
#include <math.h>
#include <stdint.h>

#define PI 3.14159265358979323846f
#define OCTANT_STEP (PI / 4.0f) // 45 degrees in radians

// Octant indices:
// 0: E  (0°)   1: NE (45°)   2: N  (90°)   3: NW (135°)
// 4: W  (180°) 5: SW (225°)  6: S  (270°)  7: SE (315°)
// Baseline: 0.25 converts 3200 hardware CPI -> 800 effective DPI
static float octant_weights[8] = {
    0.25f, 0.25f, 0.25f, 0.25f,
    0.25f, 0.25f, 0.25f, 0.25f
};

static float rem_x = 0.0f;
static float rem_y = 0.0f;

void process_mouse_delta(int16_t raw_dx, int16_t raw_dy, int16_t *out_dx, int16_t *out_dy) {
    if (raw_dx == 0 && raw_dy == 0) {
        *out_dx = 0;
        *out_dy = 0;
        return;
    }

    // 1. Compute polar angle in [0, 2*PI) with Cartesian Y-up (-raw_dy)
    float theta = atan2f((float)(-raw_dy), (float)raw_dx);
    if (theta < 0.0f) {
        theta += 2.0f * PI;
    }

    // 2. Find bounding octants k0 and k1 = (k0 + 1) % 8
    float octant_pos = theta / OCTANT_STEP;
    int k0 = (int)octant_pos % 8;
    int k1 = (k0 + 1) % 8;
    float t = octant_pos - floorf(octant_pos); // Fractional position [0.0, 1.0)

    // 3. Cosine interpolation for C1 smooth derivative across octant boundaries
    float smooth_t = (1.0f - cosf(t * PI)) * 0.5f;
    float scale = octant_weights[k0] * (1.0f - smooth_t) + octant_weights[k1] * smooth_t;

    // 4. Scale and accumulate sub-pixel remainders
    float exact_x = ((float)raw_dx * scale) + rem_x;
    float exact_y = ((float)raw_dy * scale) + rem_y;

    *out_dx = (int16_t)truncf(exact_x);
    *out_dy = (int16_t)truncf(exact_y);

    rem_x = exact_x - (float)(*out_dx);
    rem_y = exact_y - (float)(*out_dy);
}
```

*MCU Optimization Note:* On an ARM Cortex-M4F with hardware single-precision FPU (such as the Nordic nRF52840), floating point arithmetic executes in single cycles. For embedded loops, `atan2f` and `cosf` can be replaced by a precomputed 256-entry lookup table (LUT), reducing execution latency to $< 2\ \mu\text{s}$.

---

## 5. Telemetry & Calibration Algorithm (Phase 2)

### 5.1 Primary Ballistic Submovement vs. Closed-Loop Correction
Final target hit accuracy (whether the user clicked on target) is a flawed calibration signal:
- Humans execute motor movements in two phases:
  1. **Primary Ballistic Submovement (Open-Loop):** Fast, reflexive muscle memory burst based on initial visual target acquisition.
  2. **Secondary Corrective Submovement (Closed-Loop):** Visual feedback arrives $\sim 150 - 200\text{ ms}$ later, prompting corrective micro-adjustments before clicking.
- If a sensitivity multiplier is miscalibrated, the user will stop short (undershoot) or fly past (overshoot) on their primary flick, and then compensate visually.
- **The Secret Sauce:** Calibrate sensitivity to the termination point of the **primary submovement**, eliminating the need for corrective secondary adjustments.

```
Velocity
   ^
   |        /\  <- Peak Velocity (v_peak)
   |       /  \
   |      /    \
   |     /      \       __---__ <- Secondary visual micro-correction
   |    /        \     /       \
   |   /          \___/         \___
   +----------------------------------> Time
                  ^
         Primary submovement boundary:
         v(t) < 0.15 * v_peak
         (Capture P_1 coordinate here)
```

#### Velocity Computation & Submovement Boundary Detection:
1. Log mouse position $(x, y)$ and timestamp $t$ at full polling frequency.
2. Compute instantaneous velocity:
   $$v(t) = \frac{\sqrt{\Delta x^2 + \Delta y^2}}{\Delta t}$$
3. Smooth velocity with a lightweight $5\text{ ms}$ moving average.
4. Locate $t_{\text{peak}}$ where velocity reaches maximum ($v_{\text{peak}}$).
5. Scan forward from $t_{\text{peak}}$ to identify the first local velocity minimum where:
   $$v(t) < 0.15 \cdot v_{\text{peak}}$$
   *(or the first sign reversal of the radial velocity component).*
6. Record the spatial coordinate $(x_1, y_1)$ at this instant as primary landing vector $\mathbf{P}_1$ relative to start center.

---

### 5.2 Radial Landing Ratio ($r_i$) & Lateral Deviation

Let:
- $\mathbf{T}$ be the vector from center to the target center.
- $\mathbf{P}_1$ be the primary landing vector from center.

#### Radial Landing Ratio ($r_i$):
Project $\mathbf{P}_1$ onto $\mathbf{T}$:

$$r_i = \frac{\mathbf{P}_1 \cdot \mathbf{T}}{\|\mathbf{T}\|^2}$$

- $r_i = 1.0$: Perfect primary calibration (dead center on flick landing).
- $r_i < 1.0$: Undershoot (e.g., $r_i = 0.90 \implies 10\%$ undershoot; multiplier $M_k$ must increase).
- $r_i > 1.0$: Overshoot (e.g., $r_i = 1.12 \implies 12\%$ overshoot; multiplier $M_k$ must decrease).

#### Lateral Deviation (Axis Skew):
Evaluate angular error $\Delta \theta_{\text{err}} = \theta_{\mathbf{P}_1} - \theta_{\mathbf{T}}$. Consistent skew (e.g., flicking at $45^\circ$ consistently pulling to $39^\circ$) indicates adjacent octant cross-talk or physical wrist pivot bias.

---

### 5.3 Damped Learning Rate & Inter-Round Weight Update Rule

To maintain muscle memory adaptation and prevent hunting/oscillation:
1. Conduct $N$ trials per octant per round (recommended: 5 trials per octant).
2. Compute median landing ratio $\tilde{r}_k$ for octant $k$ (median rejects outlier flubs/slips).
3. Update multiplier $M_k$ with learning rate $\alpha = 0.35$ and clamp maximum change to $\pm 5\%$ per round:

$$M_k^{(\text{new})} = M_k^{(\text{old})} \times \left(1 + \alpha \left(\frac{1}{\tilde{r}_k} - 1\right)\right)$$

$$\Delta_{\text{clamped}} = \text{clamp}\left(M_k^{(\text{new})} - M_k^{(\text{old})},\ -0.05 \cdot M_k^{(\text{old})},\ +0.05 \cdot M_k^{(\text{old})}\right)$$

$$M_k^{(\text{final})} = M_k^{(\text{old})} + \Delta_{\text{clamped}}$$

---

### 5.4 Center-Out Drill Protocol
- **Home Position:** Hold crosshair on center home dot for $150\text{ ms}$.
- **Target Spawning:** Target appears at fixed radial distance $R$ (e.g., $350\text{ px}$ to $500\text{ px}$) along one of 8 octant radials.
- **Angle Jitter:** Add pseudo-random angular jitter of $\pm 8^\circ$ around the octant ray to prevent mechanical muscle-locking or straight-line rail memorization.
- **Round Size:** 40 trials per round (5 randomized trials $\times$ 8 directions) to prevent anticipatory wrist pre-tensing.

---

## 6. Why Motion Sync is Mandatory

Modern optical sensors (PAW3395/3950) run internal optical imaging framing at up to $\sim 20,000\text{ FPS}$, while the USB/RF controller polls at fixed intervals (e.g., $250\ \mu\text{s}$ at 4,000 Hz or $1,000\ \mu\text{s}$ at 1,000 Hz).

### 1. Eliminating "Beat Frequency" Angle Jitter
- **Motion Sync OFF:** Sensor frame capture and USB poll cycles run asynchronously. One USB packet might aggregate $180\ \mu\text{s}$ of displacement while the next aggregates $310\ \mu\text{s}$. On a steady diagonal swipe, this introduces integer quantization noise where instantaneous $\theta = \text{atan2}(-\Delta y, \Delta x)$ oscillates erratically (e.g., $26^\circ \to 34^\circ \to 27^\circ$), causing micro-fluctuations in the interpolated multiplier $S(\theta)$.
- **Motion Sync ON:** The sensor locks optical frame accumulation directly to the USB poll tick. Every packet covers an identical $\Delta t$, generating a mathematically stable angle $\theta$ on every tick.

### 2. Clean Velocity Derivatives for Telemetry
Submovement detection requires computing $v(t) = \frac{\sqrt{\Delta x^2 + \Delta y^2}}{\Delta t}$. Asynchronous polling causes artificial sawtooth spikes in $\Delta x^2 + \Delta y^2$, requiring heavy low-pass filtering that smears the true deceleration boundary. Motion Sync delivers clean, deterministic velocity curves directly from raw inputs.

### 3. Negligible Latency Cost on High-End Hardware
- On the **Finalmouse UltralightX**, Motion Sync is implemented via hardware interrupt synchronization between the nRF52840 MCU and PAW3395 (not software MCU buffering).
- At 2,000 Hz – 8,000 Hz polling rates, the maximum synchronization window is $< 125\ \mu\text{s} - 250\ \mu\text{s}$ (real-world synchronization offset $\sim 0.05\text{ ms} - 0.12\text{ ms}$), which is imperceptible to humans.

---

## 7. System Architecture & 4-Phase Roadmap

```
+-----------------------------------------------------------------------------+
|                               PHASE 1 (Prototype)                           |
|  [Mouse 3200 CPI] -> /dev/input/eventX -> [./anymouse RT thread]            |
|                                                  | (scale_vector)           |
|  [Calibration thread] -- atomic octant_weights[8] --> /dev/uinput (Virtual) |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                               PHASE 2 (Calibration)                         |
|  FpsAimForge in-process plugin (libanymouse_faf.so) + 8-octant drills       |
|  Telemetry over /dev/shm; computes P_1, r_i; updates weights between rounds |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                               PHASE 3 (Firmware MCU)                        |
|  Directly on Attack Shark R3 (nRF52840 + PAW3395)                           |
|  Option B1: Open ZMK/UFA firmware via SWD pads                              |
|  Option B2: Binary trampoline patch in Flash via morluto/rea + Ghidra       |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                               PHASE 4 (Universal Hardware)                  |
|  ESP32-P4 USB-Host MitM Pass-Through (firmware/esp32p4-dongle/)             |
|  - Mouse side (HS host): reads any commercial mouse at its native rate      |
|  - Core 0: scales every report with the octant weights                      |
|  - Core 1: vendor-HID control channel (WebHID page and/or ./anymouse)       |
|  - PC side (FS device): standard USB HID mouse, 1000 Hz, no driver          |
+-----------------------------------------------------------------------------+
```

### Phase 1: OS-Level Input Interception (CachyOS / Linux evdev + uinput)
- Intercept raw events using `ioctl(fd, EVIOCGRAB, 1)` on `/dev/input/by-id/usb-...-event-mouse` to detach the physical mouse from Wayland / X11 / Gamescope.
- Create a virtual `/dev/uinput` mouse with matching input capabilities (`EV_REL`, `REL_X`, `REL_Y`, `REL_WHEEL`, `BTN_LEFT`, `BTN_RIGHT`, etc.).
- Run vector math loop on `EV_SYN` (`SYN_REPORT`).
- Hot-reload `_Atomic float octant_weights[8]` directly in process memory (GUI and calibration threads write, the real-time thread reads with relaxed atomics), so no socket is needed and no ticks are dropped. The earlier UDP/Unix-socket design was removed during the C consolidation.

### Phase 2: `FpsAimForge` (FAF) Native Telemetry Plugin & 8-Octant Calibration Engine
- **In-Process Native Plugin (`libanymouse_faf.so`):** Hooks `FpsAimForge`'s 13 `Scenario` / `BaseScenario` vtables in-process (`src/faf_plugin.c`) via a lightweight Steam launcher wrapper (`src/faf_launcher.c`) with zero external memory sniffing.
- **Custom 8-Octant Calibration Bundle (`faf/AnyMouse.bundle.json`):** Installed into `~/.local/share/FpsAimForge/bundles/AnyMouse.bundle.json`, providing `AnyMouse 8-Octant Center-Out`, `AnyMouse 8-Octant Center-Out Ghost`, `AnyMouse 8-Octant Wide Flick`, and the `AnyMouse Calibration Suite` playlist.
- **True 3D Submovement Telemetry (`/dev/shm/anymouse_faf_telemetry`):** Tracks $1000\text{ Hz}$ 3D camera `(yaw, pitch)` trajectories against exact 3D target centers $\mathbf{T}$ from `TargetManager`, isolates the primary ballistic submovement $\mathbf{P}_1$, evaluates per-octant median landing ratios $\tilde{r}_k$, and applies damped inter-round weight updates ($\alpha = 0.35$, $\pm 5\%$ clamp) at round completion. (KovaaK's UE4.26 offsets in `src/kovaaks_offsets.h` are retained in sunset/standby mode behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing.)

### Phase 3: Mouse MCU Firmware Integration (Attack Shark R3 / nRF52840)
Deploy vectoring directly to the internal microcontroller:
- **Path B1 (Open Firmware Port via Project UFA / ZMK):** [aroum/ufa](https://github.com/aroum/ufa) provides open ZMK + Enhanced ShockBurst (ESB) firmware for Chinese nRF52840 + PAW3395 mice. Flash via SWD solder pads (`VDD`, `GND`, `DIO`, `CLK`). Full C source access.
- **Path B2 (Binary Trampoline Patching via `morluto/rea`):** Binary patch stock firmware image to maintain stock 2.4GHz receiver compatibility (see Section 8).

### Phase 4: Universal USB-HID Hardware Pass-Through (ESP32-P4 MitM Dongle)
The earlier RP2040 concept was replaced by an **ESP32-P4** (Waveshare ESP32-P4-Module-DEV-KIT) on 2026-10-25. The mouse or its 2.4 GHz receiver plugs into the board's USB-A port (High-Speed host); the board connects to the PC over USB-C and appears as an ordinary USB mouse. See [Section 13](#13-esp32-p4-hardware-dongle-phase-4) for the summary and [firmware/esp32p4-dongle/PLAN.md](firmware/esp32p4-dongle/PLAN.md) for the full design.
- **USB Host:** reads raw HID reports at the mouse's native rate (including 8 kHz receivers).
- **Core 0:** extracts the deltas, runs the same octant math as `scale_vector()`, and emits the transformed report to the PC.
- **Core 1:** vendor-HID control channel for weights, profiles and firmware updates.
- **Limit:** the PC-facing controller is Full-Speed, so output to the PC is capped at 1000 Hz.

---

## 8. Reverse Engineering Proprietary Firmware (`morluto/rea`)

[morluto/rea](https://github.com/morluto/rea) (*Reverse Engineer Anything*) is an MCP-based agentic framework connecting coding agents to disassemblers (**Ghidra, IDA Pro, Hopper, Rizin**) and binary unpackers (**Binwalk, Unblob**).

### Reconstruction Pitfalls vs. Binary Trampoline Patching
- **The Pitfall of Full C Reconstruction:** Proprietary wireless gaming mice rely on closed-source radio stacks (e.g., Nordic SoftDevice, proprietary Gazell 4K/8K protocols) and cryptographic bootloader validation. Attempting to decompile and rebuild the complete C project will break hard real-time radio timings and fail bootloader integrity checks.
- **The Trampoline Patch Approach:**
  1. **Unpack Companion App / Updater:** Point `rea` at the vendor's Windows firmware updater executable. Trace firmware payload extraction, decompression, and CRC/checksum algorithms.
  2. **Locate Motion Burst Read in Ghidra:** Inspect the extracted ARM Cortex-M4F binary. Locate SPI reads targeting PixArt Motion Burst register `0x16` (PAW3395/PMW3360). This isolates the exact routine receiving raw `int16_t dx, dy`.
  3. **Inject Trampoline Hook:**
     - Identify unused Flash memory at the end of the firmware image.
     - Allocate 32 bytes of SRAM for the 8 float weights and 8 bytes for remainder accumulators.
     - Replace the instruction following the SPI burst read with a branch (`BL`) instruction redirecting execution to the custom ARM Thumb-2 hook.
  4. **Expose Vendor HID Control Channel:** Hook the USB HID vendor report dispatcher to allow the aim trainer to write weights directly into SRAM.
  5. **Checksum & Flash:** Recalculate checksums using the unpacked algorithm and flash via stock DFU updater.

---

## 9. Hardware Profiles & Linux Configuration Setup

### Device Telemetry & Verified USB Hardware IDs

Both the Finalmouse UltralightX and Attack Shark R3 share the same underlying silicon architecture: **Nordic nRF52840 MCU + PixArt PAW3395 Sensor**.

Verified system identifiers from `/sys/class/hidraw/hidraw*/device/uevent`:

| Device | Subsystem | Vendor ID (Hex) | Product ID (Hex) | Nodes / Endpoints | Notes |
|:---|:---|:---:|:---:|:---|:---|
| **Finalmouse UltralightX Dongle** | `hid-generic` | `361D` / `361d` | `0100` | `/dev/hidraw6` (`input0`) | Single combined HID interface for movement and vendor commands |
| **Attack Shark R3 2.4G Receiver** | `hid-generic` | `3151` | `402D` / `402d` | `/dev/hidraw11` (`input0`), `/dev/hidraw12` (`input1`), `/dev/hidraw13` (`input2`) | OEM Yichip / Royuan / Fantech stack. `hidraw11` is mouse motion; `hidraw12`/`hidraw13` are config endpoints |
| **Wooting 80HE** (Reference) | `hid-generic` | `31E3` | `1402` | `/dev/hidraw1` – `/dev/hidraw5` | Multi-endpoint analog keyboard |
| **Epomaker EK21** (Reference) | `hid-generic` | `36B0` | `3066` | `/dev/hidraw7` – `/dev/hidraw9` | Multi-endpoint numpad |

---

### WebHID Browser Requirements & Sandboxing Warnings
Both mice support browser configuration:
- **Finalmouse ULX:** [xpanel.finalmouse.com](https://xpanel.finalmouse.com)
- **Attack Shark R3:** Attack Shark Web Beta portal
- **Universal Open Driver:** [umdrivers.com/en/mice/openmouse](https://umdrivers.com/en/mice/openmouse) (supports both `361d:0100` and `3151:402d` natively).

#### Browser Caveats:
1. **Chromium Required:** Firefox does not support the WebHID standard.
2. **Flatpak Sandbox Issue:** Flatpak browsers (Brave, Chromium, Chrome) block access to `/dev/hidraw*` regardless of system udev rules. You must use a **native pacman/paru package** or apply flatpak device overrides:
   ```bash
   flatpak override --user --device=all <app-id>
   ```

---

### CachyOS / Arch udev Configuration (`70-mouse-webhid.rules`)

By default, `/dev/hidraw*` nodes are created with `0600` permissions (root-only), causing `EACCES (Permission Denied)` when the browser calls `navigator.hid.requestDevice()`.

#### udev Matching Quirks:
- `udevadm trigger` defaults to `--action=change`, but hidraw attributes require `--action=add`.
- The USB subsystem uses lowercase hexadecimal (`361d`, `3151`), while the HID parent subsystem exposes uppercase strings (`HID_ID=0003:0000361D:00000100`). Rules must account for both levels.

#### Definitive udev Rule (`/etc/udev/rules.d/70-mouse-webhid.rules`):
```udev
# Finalmouse ULX Dongle (361D:0100) + Wired
SUBSYSTEM=="hidraw", KERNELS=="*361D:*", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="361d", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="361d", MODE="0666", TAG+="uaccess"

# Attack Shark R3 2.4G Receiver (3151:402D) + Wired
SUBSYSTEM=="hidraw", KERNELS=="*3151:*", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="3151", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="3151", MODE="0666", TAG+="uaccess"

# Allow user-space virtual mouse creation (/dev/uinput) for Phase 1 daemon
KERNEL=="uinput", SUBSYSTEM=="misc", MODE="0660", GROUP="input", TAG+="uaccess"
```

#### Reload & Activation Command:
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger --action=add --subsystem-match=hidraw --subsystem-match=usb
```

Verify permissions:
```bash
ls -l /dev/hidraw6 /dev/hidraw11 /dev/hidraw12 /dev/hidraw13 /dev/uinput
```
*(Nodes should show `crw-rw-rw-+` or `crw-rw----+` with user ACL tags).*

---

### Sniffing Protocol via WebHID
Because vendor configurators operate via WebHID:
1. Open Chromium DevTools (`F12`) on XPANEL or OpenMouse.
2. In the **Sources** tab, place breakpoints on `sendReport` or `sendFeatureReport`.
3. Capture the exact 16-to-64 byte HID payloads used to adjust CPI, polling rate, LOD, and Motion Sync.
4. Alternatively, capture USB packets on CachyOS using kernel monitors:
   ```bash
   sudo modprobe usbmon
   sudo wireshark # Capture on interface usbmonX
   ```

---

## 10. Developer & Agent Context Reference

### Hardware Baseline Setup Checklist
Before running vector calibration, ensure the physical mouse profile is locked:
- [x] **CPI Setting:** Fixed at **3,200 CPI** (or **6,400 CPI**).
- [x] **Polling Rate:** **1,000 Hz – 4,000 Hz**.
- [x] **Motion Sync:** **ON** (deterministic $\Delta t$ framing).
- [x] **Angle Snapping:** **OFF** (prevents sensor from overriding diagonal vectors).
- [x] **Ripple Control / Smoothing:** **OFF** (preserves raw velocity profiles).

### Mouse Auto-Detection (Universal Plug & Play)
The engine finds mice by itself; no device path needs to be configured ([`src/devices.c`](src/devices.c)):
- **Candidates:** every physical `/dev/input/event*` node with `REL_X`, `REL_Y` and `BTN_LEFT`. Nodes under `/sys/devices/virtual/input/` are skipped. That is essential, because the engine's own uinput output clones the real mouse's name and VID:PID.
- **Ranking:** a known vendor protocol (Finalmouse ULX, Attack Shark R3) gets +100, "mouse" in the name +10, a scroll wheel +5, a right button +5, and a stable `/dev/input/by-id` link +2. Keyboards and macro pads that expose an auxiliary mouse interface (another interface on the same USB device has `KEY_A` + `EV_REP`) get −40.
- **Hotplug:** an unplugged mouse is released and the engine waits for it to return. In auto mode, plugging in a higher-ranked mouse (e.g. re-plugging a dongle) moves the grab back to it. The engine watches `/dev/input` with inotify, so sysfs is only scanned on hotplug, never on a timer in the real-time thread.
- **Selection:** in the GUI, use the **Mouse** dropdown (Auto, or any detected mouse), **🎯 Identify** (move the mouse you want for 4 s), or type a path into the manual fallback. The choice is saved to `~/.config/anymouse/config`. On the CLI, pass a `/dev/input/...` path to override auto-detection.
- `./anymouse --list-devices` prints the ranking, with `*` marking the auto-selected mouse.

Any mouse works for vectoring. CPI and polling-rate control need a supported vendor protocol; for other mice, set the CPI dropdown to your mouse's real CPI so the effective-DPI readouts are correct.

---

## 11. Unified C Engine Architecture (`./anymouse`)

The repository compiles into a single, self-contained, multi-threaded POSIX C binary (`./anymouse`) with zero Python runtime or multi-process IPC dependencies:

- [`src/core.h`](src/core.h) & [`src/main.c`](src/main.c): Shared process state (`_Atomic float octant_weights[8]`, `_Atomic int hardware_cpi`, lock-free `SPSC` ring buffer `tick_ring[65536]`, presets, and JSON profile persistence).
- [`src/evdev_rt.c`](src/evdev_rt.c): **Thread 1 (`SCHED_FIFO` priority 80 + `mlockall`)** — Real-time kernel `evdev` interceptor (`EVIOCGRAB`) and virtual `/dev/uinput` mouse emitter. Executes `scale_vector()` with Cosine $C^1$ smooth interpolation and sub-pixel error diffusion (`rem_x`, `rem_y`) using lock-free `memory_order_relaxed` atomic weight reads, and pushes every `SYN_REPORT` frame into the lock-free `SPSC` ring buffer.
- [`src/hid_driver.c`](src/hid_driver.c): Native C `/dev/hidraw*` OpenMouse protocol driver (the GUI calls it from a dedicated `hw-io` worker thread, because the Attack Shark relay takes seconds) for **Finalmouse UltralightX** (`0x361D:0x0100`) and **Attack Shark R3 / GearHub** (`0x3151:0x402D`). It controls whichever mouse the engine holds and picks the vendor configuration interface from the HID report descriptor (usage page `0xFF00`/`0x01` for Finalmouse, `0xFFFF`/`0x02` for GearHub).
  - **Finalmouse:** reads CPI, polling rate, battery, Motion Sync and RSSI; writes CPI and polling rate (500 to 8000 Hz).
  - **Attack Shark R3:** reads and writes polling rate (125 to 8000 Hz) through the 2.4G receiver relay (`F6 05` → `F7` → command → `FC`, feature report `0x00`). The write is a read-modify-write of the `OPTIONPARAM0` settings block that is verified by reading it back. CPI writing is not implemented for the R3 yet.
- [`src/devices.c`](src/devices.c) & [`src/config.c`](src/config.c): Universal mouse discovery, ranking, identify-by-motion, the persisted device choice (`~/.config/anymouse/config`) and the auto-saved octant profile (`~/.config/anymouse/profile.json`). Both files are owned by the invoking user even under `sudo`.
- [`src/steam_install.c`](src/steam_install.c): Finds FpsAimForge (Steam app `5165380`) in every Steam library and installs, repairs or removes the integration (see below).
- [`src/faf_telemetry.h`](src/faf_telemetry.h), [`src/faf_plugin.c`](src/faf_plugin.c) (`libanymouse_faf.so`), & [`src/faf_launcher.c`](src/faf_launcher.c): **Phase 2 `FpsAimForge` (FAF) Native In-Process Telemetry Plugin** — Hooks `FpsAimForge`'s 13 `Scenario` / `BaseScenario` vtables in-process to capture $1000\text{ Hz}$ 3D camera `(yaw, pitch)` trajectories, exact 3D target coordinates from `TargetManager`, and round lifecycle events (`MENU`, `WAITING_FOR_CLICK`, `START_COUNTDOWN`, `RUNNING`, `DONE`) over POSIX shared memory (`/dev/shm/anymouse_faf_telemetry`). Includes custom calibration scenarios in [`faf/AnyMouse.bundle.json`](faf/AnyMouse.bundle.json) (`AnyMouse 8-Octant Center-Out`, `AnyMouse 8-Octant Center-Out Ghost`, `AnyMouse 8-Octant Wide Flick`).
- [`src/calibration.c`](src/calibration.c) & [`src/kovaaks_offsets.h`](src/kovaaks_offsets.h): **Thread 2 (Ballistic Submovement & Aim-Trainer Telemetry Engine)** — Consumes verified 3D ballistic submovement events ($\mathbf{P}_1$ vs. true target center $\mathbf{T}$) from `FpsAimForge` over `/dev/shm/anymouse_faf_telemetry` with strict active-round session gating, computes per-octant median landing ratios $\tilde{r}_k$, and applies damped inter-round weight updates ($\alpha = 0.35$, $\pm 5\%$ clamp) at round completion. (Retains KovaaK's UE4.26 offsets in sunset/standby mode behind `ANYMOUSE_ENABLE_KOVAAKS=1` for future Windows testing.)
- [`src/gui_gtk4.c`](src/gui_gtk4.c): **Thread 3 (Native GTK4 + Cairo Studio GUI)** — Interactive polar radar chart, hardware sensor controls, and real-time `FpsAimForge` ballistic telemetry dashboard.

### Flick Log
Every plugin event is appended to `logs/flicks.csv` next to the binary (override with `ANYMOUSE_LOG_DIR`). Columns: `unix_time, scenario, round_id, octant, accepted, is_hit, target_angle_deg, target_dist_deg, p1_dist_deg, radial_ratio, angular_error_deg, peak_vel_deg_s, duration_ms, cm_per_360, hardware_cpi, auto_calibrate, w0..w7` (the weights in effect when the flick was consumed). `accepted=0` marks flicks the calibrator filtered out. The file is owned by the invoking user when launched via `sudo`, and `logs/` is git-ignored.

### Building & Running

#### 1. Compile & Install `FpsAimForge` Plugin + Scenario Bundle
```bash
make          # builds everything, then runs `./anymouse --install-faf`
```

#### 2. Run the Unified Application (GUI + RT Engine + `FpsAimForge` Telemetry)
```bash
# Launch with elevated evdev/uinput permissions while preserving Wayland/X11 GUI session:
./start_daemon.sh
# Or via Makefile:
make run
# Force a specific mouse instead of auto-detection:
./start_daemon.sh /dev/input/by-id/usb-...-event-mouse
```

#### 3. CLI & Hardware Probe Modes
```bash
# Query live hardware mouse telemetry over /dev/hidraw and exit:
./anymouse --probe-hw
# Or:
make probe-hw

# List detected mice in auto-selection order:
./anymouse --list-devices

# Run in headless CLI daemon mode with automatic damped calibration enabled:
./start_daemon.sh --headless --auto-calibrate
```

#### 4. FpsAimForge Integration (Steam Auto-Detect)
`./anymouse --install-faf` (also run by `make`, or the **Install / Repair** button in the GUI):
- **Finds the game:** reads every library in `libraryfolders.vdf` for native, Flatpak and Snap Steam, then finds FpsAimForge through `appmanifest_5165380.acf`. Nothing is hard-coded.
- **Installs:** moves the real binary to `FpsAimForge.bin`, installs the launcher as `FpsAimForge` and the plugin as `libanymouse_faf.so` in the game folder, and copies `AnyMouse.bundle.json` into the FpsAimForge `bundles` folder. Files are written atomically and owned by your user.
- **Steam updates:** when Steam updates the game it restores the original `FpsAimForge`. Detection reports **needs repair**, and installing again puts the launcher back.
- **Safety:** the installer refuses to move a suspiciously small `FpsAimForge` over `FpsAimForge.bin`.
- `./anymouse --faf-status` shows the state (not installed / needs repair / update available / up to date). `./anymouse --uninstall-faf` restores the original binary.

#### 5. Running Without sudo (optional)
`sudo ./anymouse --install-udev` (or `make install-udev`) installs `/etc/udev/rules.d/70-anymouse.rules` and loads `uinput` at boot. The rules give the logged-in seat user (`uaccess`) access to mice, `/dev/uinput` and the two vendor HID interfaces. After that, `ANYMOUSE_NO_SUDO=1 ./start_daemon.sh` runs as your user.
- **Trade-offs:** any program you run can then read mouse input and create virtual input devices. Without root, the real-time thread uses `SCHED_FIFO` only if your rtprio limits allow it.

---

## 12. Native GTK4 + Cairo Studio & `FpsAimForge` Phase 2 Telemetry

The built-in GTK4 + Cairo studio ([`src/gui_gtk4.c`](src/gui_gtk4.c)) provides a zero-latency interface connected directly to the real-time vectoring and calibration threads in shared memory:

### Key Features
- **Interactive Polar Radar Chart (Cairo):** Displays a glowing polygon of the current 8-octant sensitivity envelope with 8 draggable spoke handles AND a live **Ballistic Telemetry Vector Overlay** showing the last flick's primary submovement landing point $\mathbf{P}_1$ (amber dot) vs. final target vector $\mathbf{T}$ (green marker).
- **Direct In-Memory Atomic Tuning:** Dragging radar handles or sliders updates `_Atomic float octant_weights[8]` directly in process memory with zero socket serialization overhead.
- **Mouse Selection:** **Mouse** dropdown (Auto or any detected mouse, ★ = hardware controls), **🔄 Rescan**, **🎯 Identify** (move the mouse you want), and a manual path fallback. The list refreshes itself on hotplug.
- **Native OpenMouse Hardware Sync:** for the selected mouse, shows live polling rate, battery percentage & voltage, Motion Sync state and RSSI dBm. Includes `Set 3200 CPI` / `Set 6400 CPI` hardware buttons, a CPI dropdown, and a **Polling** dropdown limited to the rates the mouse accepts. After a write, it shows the rate the mouse reports back.
- **FpsAimForge Install / Repair:** shows the integration status and can install, repair after a Steam update, update, or uninstall.
- **Phase 2 `FpsAimForge` (FAF) Telemetry & Auto-Calibration Panel:**
  - Connects to `libanymouse_faf.so` inside `FpsAimForge` over `/dev/shm/anymouse_faf_telemetry` with strict active-round session gating (`FAF_STATE_RUNNING`).
  - Displays live scenario name, round timer, hits/shots/accuracy, per-octant median landing ratios $\tilde{r}_k$, trial progress ($n/5$), peak angular velocity $\omega_{\text{peak}}$, and angular axis skew $\Delta\theta_{\text{err}}$.
  - Supports automatic inter-round or one-click damped weight updates ($\alpha = 0.35$, $\pm 5\%$ max step clamp per round).
- **Auto-Saved Profile:** the 8 octant weights (and the sensor CPI used for the effective-DPI readouts) are saved to `~/.config/anymouse/profile.json` when the app exits and restored on the next launch. The file is written atomically and owned by your user even under `sudo`. A `profile.json` next to the binary (the older location) is read as a fallback until a config-dir profile exists. Starting with `--preset` or an explicit base scale is session-only and is not written back. **Save Profile** writes immediately and **Reload Saved Profile** reverts to the saved state. If the app is killed rather than closed, the previous save is kept. The Master Vector Scaling widgets show the mean of the 8 weights, so they follow loaded and calibrated profiles.
- **Master Scaling, Symmetry Linking & JSON Profiles:** Proportionally scale all octants, link horizontal ($E \leftrightarrow W, NE \leftrightarrow NW, SE \leftrightarrow SW$) or vertical ($N \leftrightarrow S$) symmetry, switch presets, and export/import `profile.json`.

---

## 13. ESP32-P4 Hardware Dongle (Phase 4)

**Status: in development (milestone M0).** Design and details live in [firmware/esp32p4-dongle/PLAN.md](firmware/esp32p4-dongle/PLAN.md); this section is the summary.

### What it is
A small USB man-in-the-middle board. The user plugs their mouse (or its 2.4 GHz receiver) into the board, and the board plugs into the PC. The board scales every movement with the same 8-octant, cosine-interpolated weights as `./anymouse` and appears to the PC as a normal USB mouse, so it works on any OS (including Windows) with no driver and no input interception. The weights are stored on the board.

### How it is configured
Over a vendor HID channel on the same USB cable. No Wi-Fi, no USB networking.
- **Web page (WebHID):** nothing to install; open it in Chrome, Edge or Brave and click Connect. Handles weights, presets, profiles and firmware updates. Chromium only.
- **`./anymouse`:** pairs with the dongle automatically. It is the only front-end that can do live `FpsAimForge` auto-calibration, because the game telemetry lives on the PC.
- Once a profile is saved, the dongle keeps vectoring with no software running.

### Hardware
Waveshare **ESP32-P4-Module-DEV-KIT** (ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM), flashed with ESP-IDF v5.5.4.

| Connector | Role |
|:---|:---|
| USB-A, port 1 (jumper on **HOST**) | Mouse or receiver in (P4 High-Speed host) |
| USB-C, **non-UART** | To the PC. The P4's Full-Speed OTG controller is moved onto this connector in software; it normally carries USB Serial/JTAG |
| USB-C, **UART** (CH343) | Flashing and serial console; always works for recovery |

Limit: the PC-facing controller is Full-Speed, so output to the PC is capped at 1000 Hz. The mouse is still read at its native rate (up to 8 kHz), so no motion is lost.

### Verified so far (2026-10-25)
- Board identified and probed with `esptool`; no secure boot or flash encryption, so it cannot be bricked by a bad image.
- Schematic read: the two USB-A stacks share one High-Speed controller through a mux, so neither can face the PC.
- Proof of concept [`spikes/m0_phy_swap`](firmware/esp32p4-dongle/spikes/m0_phy_swap/README.md): the second USB-C enumerates as a HID mouse plus a vendor HID channel (`0xFF4D`), and 20 of 20 vendor round trips echoed correctly (about 5 ms RTT). No eFuse was burned.
- Control plane built and tested with no mouse: octant weights are stored in flash (4 profile slots, auto-save, CRC), updated from the host with stale-write detection, and pushed back as events when the device changes them. [`firmware/esp32p4-dongle/tools/dongle_test.py`](firmware/esp32p4-dongle/tools/dongle_test.py) runs 48 checks over the vendor-HID channel, including the real scaling code against an independent reference and persistence across a reboot. The wire protocol is [`common/amv_dongle_proto.h`](common/amv_dongle_proto.h) and the shared scaling math is [`common/amv_octant.h`](common/amv_octant.h).
- **Web front-end built:** [`web/`](web/README.md) is AnyMouse Studio, a static WebHID page (radar chart, sliders, presets, profile slots, JSON import/export) with a built-in demo mode. 16 Node tests pass, including checks against frames recorded from the real board. It has not yet been tried in a real browser against the board.
- Not yet done: the mouse-side USB host with a real mouse, forwarding scaled motion to the PC, and the `./anymouse` side of the sync (`src/dongle.c`). `src/evdev_rt.c` still has its own copy of the scaling math.

### Where to go next
| I want to... | Read |
|:---|:---|
| Build, flash and cable the board | [firmware/esp32p4-dongle/README.md](firmware/esp32p4-dongle/README.md) |
| Understand the design, milestones and risks | [firmware/esp32p4-dongle/PLAN.md](firmware/esp32p4-dongle/PLAN.md) |
| See what was tested and when | [PROGRESS.md](PROGRESS.md) |
| Know what comes next with real mice (auto-discovery, CPI/polling for known mice, generic fallback, acceptance checklist) | [PLAN.md section 9](firmware/esp32p4-dongle/PLAN.md#9-mouse-discovery-device-info-and-hardware-verification) |
