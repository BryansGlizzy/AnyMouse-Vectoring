#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * The hot path between the mice and the PC.
 *
 * Every native report from a mouse is scaled on its own with the octant weights (per-source
 * sub-pixel remainder), the scaled deltas are summed, and one frame goes out per PC-side
 * 1 ms slot. Several mice are merged: motion is summed and buttons are OR-ed. Button changes
 * are never merged: each change of the combined button state is queued and sent in its own
 * frame, so a quick press+release always reaches the PC. Deltas beyond one frame's range
 * are carried to the next frame, never clamped away.
 */

#define AMV_VECTOR_MAX_SOURCES 8

typedef struct {
    uint32_t reports_in;        /* native reports fed */
    uint32_t frames_out;        /* PC-side frames sent */
    uint32_t button_overflow;   /* button states dropped because the queue was full */
    int64_t  raw_dx, raw_dy;    /* summed native counts */
    int64_t  out_dx, out_dy;    /* summed counts sent to the PC */
} amv_vector_stats_t;

void amv_vector_start(void);

/* src: 0..AMV_VECTOR_MAX_SOURCES-1, one per claimed mouse interface. Safe from any task. */
void amv_vector_feed(unsigned src, int32_t raw_dx, int32_t raw_dy, int32_t wheel, int32_t pan,
                     bool has_buttons, uint32_t buttons);
/* The source went away: its buttons are released and its sub-pixel remainder cleared. */
void amv_vector_release(unsigned src);
void amv_vector_get_stats(amv_vector_stats_t *out);
