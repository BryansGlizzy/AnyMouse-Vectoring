#pragma once
/*
 * PC side of the bridge: the 8-byte mouse report (16 buttons, 16-bit X/Y, 8-bit wheel and pan) on the
 * Teensy's High-Speed device port, polled every 125 us.
 *
 * Everything runs in interrupts: a report from a mouse (host interrupt) is scaled and fed here, and a frame is
 * sent at once when the endpoint is idle; when the PC collects a frame (device interrupt) the next one is sent.
 * loop() never has to run for motion to reach the PC, so a slow driver call cannot stall the cursor.
 *
 * Same rules as the ESP32-P4's amv_vector: several mice are merged (motion summed, buttons OR-ed), every change
 * of the combined button state goes out in its own frame (a quick press and release is never merged), and
 * deltas beyond +-32767 are carried to the next frame, never clamped away.
 */
#include <stdint.h>
#include <stdbool.h>

#define PC_OUT_MAX_SOURCES 8

struct pc_out_stats_t {
    uint32_t frames;            // frames sent to the PC
    uint32_t reports;           // scaled reports fed in
    uint32_t button_overflow;   // button states dropped because the edge queue was full
    uint32_t carried;           // frames whose motion did not fit and was carried
    int32_t  raw_x, raw_y;      // the mice's own deltas
    int32_t  sent_x, sent_y;    // what was sent
    uint32_t age_sum, age_max, age_n;   // cycles from "motion fed" to "frame queued"
};

void pc_out_begin(void);
// Host interrupt: one native report, already scaled. `buttons` only counts when has_buttons.
void pc_out_feed(unsigned src, int32_t raw_dx, int32_t raw_dy, int32_t sx, int32_t sy, int32_t wheel, int32_t pan,
                 bool has_buttons, uint16_t buttons);
// The source went away: its buttons are released.
void pc_out_release(unsigned src);
// Self-test: 8 kHz +1/-1 X steps (net zero) until the given millis() value; 0 stops it.
void pc_out_selftest(uint32_t until_ms);
bool pc_out_selftest_active(void);
// Snapshot and clear.
void pc_out_take_stats(pc_out_stats_t *out);
// The PC is not listening (not configured): drop what is pending so it is not replayed later.
void pc_out_poll(void);
