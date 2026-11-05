#pragma once
/*
 * The list of attached mice the vendor channel reports (GET_MOUSE and friends), and the OpenMouse drivers behind
 * it. Teensy counterpart of the registry and handlers in the ESP32-P4's amv_usb_host.c, same semantics:
 *
 *   - one entry per mouse interface (tiers GENERIC, BOOT, KNOWN), then one per device that is not a mouse
 *     (tier IGNORED with a reason);
 *   - a device whose VID:PID is in the OpenMouse catalog with an engine gets a driver session in the QuickJS
 *     engine over all its HID interfaces; a successful readStatus makes the mouse KNOWN (real CPI, polling rate,
 *     battery, LOD, Motion Sync, link), refreshed every 5 s;
 *   - writes go through the driver for KNOWN mice and only for models verified through a dongle (write policy);
 *     a generic mouse's CPI is a label stored per VID:PID.
 *
 * Everything here runs in loop(). Driver calls take milliseconds to seconds, so a write is started here and its
 * reply sent later (amv_proto's deferred reply); loop() keeps serving the vendor channel meanwhile.
 */
#include "amv_proto_teensy.h"

void mice_begin(bool engine);
void mice_poll(void);
const amv_mouse_ops_t *mice_ops(void);
uint8_t mice_count(void);
// Console: one line per entry.
void mice_print(void);
