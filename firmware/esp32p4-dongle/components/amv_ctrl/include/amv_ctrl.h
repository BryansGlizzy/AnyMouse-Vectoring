#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "amv_dongle_proto.h"

/*
 * Single owner of the dongle's tuning state: octant weights, bypass, active profile slot
 * and a generation counter that increases on every change.
 *
 * The hot path (USB host -> scaling) reads weights with amv_ctrl_snapshot_effective(),
 * which is lock-free (a seqlock) and never blocks on the control plane.
 */

typedef struct {
    uint32_t gen;
    float    w[8];      /* raw weights; bypass is separate */
    bool     bypass;
    uint8_t  active_slot;
} amv_state_t;

typedef enum {
    AMV_R_OK = 0,
    AMV_R_BAD_VALUE,
    AMV_R_STALE,
    AMV_R_BAD_SLOT,
    AMV_R_EMPTY,
    AMV_R_STORE,
} amv_result_t;

typedef struct {
    uint32_t sets_ok, stale_rejects, bad_value_rejects, saves, loads;
} amv_ctrl_stats_t;

/* Called after every state change, outside the internal lock, from the thread that made the change. */
typedef void (*amv_event_cb_t)(const amv_state_t *state, uint8_t source);

esp_err_t    amv_ctrl_init(void);
void         amv_ctrl_subscribe(amv_event_cb_t cb);
void         amv_ctrl_get_state(amv_state_t *out);
void         amv_ctrl_get_stats(amv_ctrl_stats_t *out);

/* Lock-free; weights with bypass applied (all 1.0 when bypassed). */
void         amv_ctrl_snapshot_effective(float w[8]);

/* base_gen == AMV_GEN_ANY applies unconditionally. `out` (optional) gets the resulting or, on STALE, current state. */
amv_result_t amv_ctrl_set_weights(uint32_t base_gen, const float w[8], uint8_t source, amv_state_t *out);
amv_result_t amv_ctrl_set_bypass(bool on, uint8_t source, amv_state_t *out);
amv_result_t amv_ctrl_save_slot(uint8_t slot, amv_state_t *out);
amv_result_t amv_ctrl_load_slot(uint8_t slot, uint8_t source, amv_state_t *out);
amv_result_t amv_ctrl_factory_reset(uint8_t source, amv_state_t *out);

/* Bit n set if slot n holds a valid saved profile. */
uint8_t      amv_ctrl_slots_mask(void);
