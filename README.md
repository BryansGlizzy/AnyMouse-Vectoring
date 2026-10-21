# AnyMouse-Vectoring: Dynamic Mouse DPI Calibration

Anisotropic, 8-octant polar sensitivity calibration driven by real-time ballistic flick telemetry. This project bridges biomechanical wrist/forearm asymmetry with low-latency mouse vector scaling across OS-level interception, custom MCU firmware, and hardware man-in-the-middle pass-through adapters.

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
   - [Phase 4: Universal USB-HID Hardware Pass-Through (RP2040 MitM Dongle)](#phase-4-universal-usb-hid-hardware-pass-through-rp2040-mitm-dongle)
8. [Reverse Engineering Proprietary Firmware (`morluto/rea`)](#8-reverse-engineering-proprietary-firmware-morlutorea)
   - [Reconstruction Pitfalls vs. Binary Trampoline Patching](#reconstruction-pitfalls-vs-binary-trampoline-patching)
   - [Workflow with Ghidra & Vendor DFU Updaters](#workflow-with-ghidra--vendor-dfu-updaters)
9. [Hardware Profiles & Linux Configuration Setup](#9-hardware-profiles--linux-configuration-setup)
   - [Device Telemetry & Verified USB Hardware IDs](#device-telemetry--verified-usb-hardware-ids)
   - [WebHID Browser Requirements & Sandboxing Warnings](#webhid-browser-requirements--sandboxing-warnings)
   - [CachyOS / Arch udev Configuration (`70-mouse-webhid.rules`)](#cachyos--arch-udev-configuration-70-mouse-webhidrules)
   - [Sniffing Protocol via WebHID](#sniffing-protocol-via-webhid)
10. [Developer & Agent Context Reference](#10-developer--agent-context-reference)
11. [C Daemon Implementation & Live IPC (`octant_daemon`)](#11-c-daemon-implementation--live-ipc-octant_daemon)
12. [Interactive GUI Studio (`gui.py`)](#12-interactive-gui-studio-guipy)

---

## 1. Project Overview & Core Goal

Traditional mouse sensitivity allows only a single uniform scalar or, at best, asymmetric X/Y sensitivity multipliers. However, human motor control is non-orthogonal. 

**The Goal:** Build an adaptive, 8-octant polar sensitivity calibration system that dynamically scales raw mouse displacement vectors based on trajectory angle. A dedicated calibration drill aggregates submovement telemetry from rounds of ballistic flicks, identifies directional overshoot and undershoot across 8 radial compass directions, and optimizes an 8-point weight profile.

The system is designed to scale across three tiers:
1. **Software Prototype (Linux/CachyOS):** Low-latency kernel input capture via `evdev` and `uinput`.
2. **On-Device Firmware:** Direct execution inside mouse MCUs (Nordic nRF52840).
3. **Universal Hardware Dongle:** A transparent USB pass-through device (RP2040) running hardware-level vector interpolation for any commercial mouse without host software or driver requirements.

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
|  [Mouse 3200 CPI] -> /dev/input/eventX -> [libevdev daemon]                 |
|                                                  | (process_mouse_delta)    |
|  [Aim Trainer] --- Unix Domain Socket --------> /dev/uinput (Virtual Mouse) |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                               PHASE 2 (Calibration)                         |
|  Dedicated 2D Center-Out Flick Drill (Raylib/Bevy/Godot)                    |
|  Computes P_1, Landing Ratios r_i, updates weights via IPC socket          |
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
|  Adafruit Feather RP2040 / RP2350 USB-Host MitM Pass-Through                |
|  - Host Port: Reads any commercial mouse @ 1000Hz-8000Hz                   |
|  - Core 0: Intercepts & scales vectors (10-15 µs latency)                   |
|  - Core 1: WebHID endpoint for live weight adjustment & EEPROM storage      |
|  - Device Port: Emits standard USB HID to any PC / console                  |
+-----------------------------------------------------------------------------+
```

### Phase 1: OS-Level Input Interception (CachyOS / Linux evdev + uinput)
- Intercept raw events using `ioctl(fd, EVIOCGRAB, 1)` on `/dev/input/by-id/usb-...-event-mouse` to detach the physical mouse from Wayland / X11 / Gamescope.
- Create a virtual `/dev/uinput` mouse with matching input capabilities (`EV_REL`, `REL_X`, `REL_Y`, `REL_WHEEL`, `BTN_LEFT`, `BTN_RIGHT`, etc.).
- Run vector math loop on `EV_SYN` (`SYN_REPORT`).
- Bind a local Unix Domain Socket (`/tmp/octant_weights.sock`) or POSIX shared memory (`shm_open`) to hot-reload `float octant_weights[8]` from the calibration client without dropping ticks.

### Phase 2: 2D Radial Calibration Engine
- Standalone calibration client built in Raylib, Bevy (Rust), or Godot.
- Implements 40-flick center-out protocol with randomized octants and $\pm 8^\circ$ jitter.
- Tracks ballistic inflection points, evaluates median landing ratio $\tilde{r}_k$, and streams updated weights over IPC socket.

### Phase 3: Mouse MCU Firmware Integration (Attack Shark R3 / nRF52840)
Deploy vectoring directly to the internal microcontroller:
- **Path B1 (Open Firmware Port via Project UFA / ZMK):** [aroum/ufa](https://github.com/aroum/ufa) provides open ZMK + Enhanced ShockBurst (ESB) firmware for Chinese nRF52840 + PAW3395 mice. Flash via SWD solder pads (`VDD`, `GND`, `DIO`, `CLK`). Full C source access.
- **Path B2 (Binary Trampoline Patching via `morluto/rea`):** Binary patch stock firmware image to maintain stock 2.4GHz receiver compatibility (see Section 8).

### Phase 4: Universal USB-HID Hardware Pass-Through (RP2040 MitM Dongle)
- **Hardware:** Adafruit Feather RP2040 with USB Type-A Host (or RP2350 / STM32H7 with high-speed USB PHY).
- **USB Host:** Connects to mouse wireless dongle, polling raw HID reports at native rates.
- **Dual-Core Split:**
  - **Core 0:** Extracts 16-bit deltas, runs `process_mouse_delta()`, outputs transformed report via USB Device port. Added latency: **$\sim 10 - 15\ \mu\text{s}$** (0.015 ms).
  - **Core 1:** Exposes secondary WebHID / RawHID interface to PC for persistent profile configuration in on-chip flash memory.

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

### Identifying Persistent Device Paths
For the Phase 1 interceptor daemon:
```bash
ls -l /dev/input/by-id/ | grep -E "mouse|Finalmouse|2.4G"
```
Use the resulting `/dev/input/by-id/usb-...-event-mouse` symlink in daemon configurations to ensure deterministic node binding across reboots.

---

## 11. C Daemon Implementation & Live IPC (`octant_daemon`)

The repository includes a production-grade, zero-dependency C implementation ([`octant_daemon.c`](file:///home/future/Desktop/AnyMouse-Vectoring/octant_daemon.c)) designed for microsecond-level latency on CachyOS / Arch Linux.

### Why POSIX C for the Low-Level Hot-Path?
- **Zero Kernel Overhead:** Direct interface with `<linux/input.h>` and `<linux/uinput.h>` through raw kernel syscalls without crate abstraction overhead or runtime garbage collection.
- **Zero Heap Allocations:** All event buffers and remainder state structs reside on the thread stack and data segment.
- **Fast SIMD Compilation:** Compiles in $< 0.3\text{ seconds}$ with `-O3 -march=native -ffast-math -flto` to an $18\text{ KB}$ binary.
- **Cross-Platform Portable Math:** The `scale_vector()` function and remainder state logic can be directly copied into the Attack Shark R3's ARM Cortex-M4F firmware or an RP2040 dongle in Phases 3 and 4.

### Latency Optimization Mechanisms
1. **`SCHED_FIFO` Real-Time Kernel Priority:** Elevates the process thread to real-time priority (80/99) and locks all process memory pages into RAM using `mlockall(MCL_CURRENT | MCL_FUTURE)`. This guarantees the CachyOS kernel scheduler wakes the daemon within microseconds of a hardware USB packet interrupt.
2. **Batch Event Draining (Up to 64 Events per Syscall):** Rather than performing individual `read()` and `write()` syscalls for each axis event, the daemon drains the entire kernel evdev buffer in a single syscall, mutates $(\Delta x, \Delta y)$ in CPU registers, and flushes the entire frame (`EV_REL`, `EV_KEY`, `EV_SYN`) to `/dev/uinput` in a single batched `write()`.
3. **Zero-Lock Non-Blocking UDP IPC (Port `42069`):** Uses non-blocking `poll()` across the evdev file descriptor and a local UDP loopback socket (`127.0.0.1:42069`). An external aim trainer or CLI tool can blast 32-byte updates (`float weights[8]`) mid-swipe without any mutex contention or dropped poll ticks.

### Building & Running

#### 1. Compile
```bash
make
# Or manually:
gcc -O3 -march=native -ffast-math -flto -Wall -Wextra octant_daemon.c -o octant_daemon -lm
```

#### 2. Run the Daemon
Launch with `sudo` (required for `EVIOCGRAB`, `SCHED_FIFO`, and `/dev/input/event*` access):

```bash
# Using the launcher script (auto-detects Finalmouse ULX or Attack Shark R3):
./start_daemon.sh

# Or run target mouse via Makefile:
make run-ulx       # Finalmouse ULX @ 0.25x scale (3200 CPI -> 800 effective DPI)
make run-shark     # Attack Shark R3 @ 0.25x scale

# Or direct invocation:
sudo ./octant_daemon /dev/input/by-id/usb-Finalmouse_UltralightX_dongle_BE1E28B86F3859BD-event-mouse 0.25
```

### Live On-The-Fly Octant Tuning

While `octant_daemon` is running, update the 8-octant multipliers in real time without stopping or restarting:

#### Using `send_weights.py`
```bash
# Apply predefined test presets:
./send_weights.py --preset vertical_boost    # Boosts North / NE / NW flick sensitivity
./send_weights.py --preset neutral           # Resets all 8 octants to 0.25
./send_weights.py --preset horizontal_boost  # Boosts lateral East/West swipes

# Set uniform sensitivity:
./send_weights.py 0.30

# Specify custom 8-octant array (E, NE, N, NW, W, SW, S, SE):
./send_weights.py 0.25 0.35 0.45 0.35 0.25 0.25 0.25 0.25
```

#### Raw Python One-Liner
```bash
python3 -c 'import socket, struct; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(struct.pack("8f", 0.25, 0.35, 0.45, 0.35, 0.25, 0.25, 0.25, 0.25), ("127.0.0.1", 42069))'
```

---

## 12. Interactive GUI Studio (`gui.py`)

A desktop GUI ([`gui.py`](file:///home/future/Desktop/AnyMouse-Vectoring/gui.py)) provides a dynamic visual interface for tuning 8-octant sensitivity in real time.

```bash
# Launch via Python:
./gui.py
# Or via send_weights.py:
./send_weights.py --gui
# Or via Makefile:
make gui
```

### Key Features
- **Interactive Polar Radar Chart:** Displays a glowing polygon of the current 8-octant sensitivity envelope. Drag any of the 8 spoke handles directly on the radar to reshape your sensitivity curve on the fly.
- **Real-Time Slider & SpinBox Tuning:** 8 granular sliders and DoubleSpinBoxes ($0.01$ step increments) with instantaneous effective DPI readouts (e.g., $3200\text{ CPI} \times 0.25 = 800\text{ DPI}$).
- **Hardware Sensor CPI Sync:** Select your mouse's locked native CPI ($1600$, $3200$, $6400$, etc.) to calculate true effective DPI across all directions.
- **Master Scaling & Symmetry Linking:** Proportionally scale the entire profile or lock horizontal ($E \leftrightarrow W, NE \leftrightarrow NW$) and vertical ($N \leftrightarrow S$) symmetry with one click.
- **Preset Quick-Bar:** Instant switching between `Neutral (0.25x)`, `Vertical Boost`, `Horizontal Boost`, `Diagonal ULX`, and `Low (0.125x)`.
- **Profile Save/Load:** Export and import custom sensitivity profiles as JSON files.
- **Zero-Latency UDP Streaming:** Emits 32-byte UDP updates to `127.0.0.1:42069` on every drag event with zero polling overhead.
