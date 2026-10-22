/*
 * AnyMouse-Vectoring — FpsAimForge (FAF) Native Telemetry Plugin
 * ==============================================================
 * Compiled into libanymouse_faf.so and loaded into FpsAimForge.
 * Hooks FpsAimForge's Scenario / BaseScenario vtables in-process to stream
 * 100% exact 3D camera, target, round lifecycle, and primary ballistic
 * submovement telemetry to /dev/shm/anymouse_faf_telemetry.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <link.h>

#include "faf_telemetry.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RAD2DEG (180.0f / (float)M_PI)

/* ============================================================================
 * FpsAimForge v0.7.0 Binary Offsets (Verified via ELF Symbol Table & Disasm)
 * ============================================================================ */

/* Stopwatch::GetElapsedMicros() const */
#define FAF_FN_STOPWATCH_GET_ELAPSED_MICROS 0x118f46ULL

/* Expected original vtable slot targets (for safety verification) */
#define FAF_FN_ON_TICK_START    0x1ad6aeULL
#define FAF_FN_ON_DETACH        0x1a9c3eULL
#define FAF_FN_UPDATE_STATE     0x1a78e6ULL
#define FAF_FN_ON_SCENARIO_DONE 0x1a60aaULL

/* Vtable addresses in .data.rel.ro for BaseScenario and all 12 Scenario types */
static const uint64_t kScenarioVtables[] = {
    0xaa5728ULL, /* aim::BaseScenario */
    0xaa1600ULL, /* aim::AngleStrafeScenario */
    0xaa1760ULL, /* aim::BarrelScenario */
    0xaa1850ULL, /* aim::BounceScenario */
    0xaa1928ULL, /* aim::CenteringScenario */
    0xaa19d8ULL, /* aim::CircleScenario */
    0xaa1ae8ULL, /* aim::LinearScenario */
    0xaa1b98ULL, /* aim::SineScenario */
    0xaa1c48ULL, /* aim::StaticScenario */
    0xaa1d28ULL, /* aim::StrafeScenario */
    0xaa1dd8ULL, /* aim::WallArcScenario */
    0xaa1ee8ULL, /* aim::WallWanderScenario */
    0xaa1fb0ULL, /* aim::WaypointScenario */
};

/* Vtable slot indices (from start of vtable symbol, where +0x10 is vptr[0]) */
#define VSLOT_ON_TICK_START    5  /* +0x28 */
#define VSLOT_ON_DETACH        8  /* +0x40 */
#define VSLOT_UPDATE_STATE     11 /* +0x58 */
#define VSLOT_ON_SCENARIO_DONE 12 /* +0x60 */

/* Field offsets inside aim::BaseScenario / aim::Scenario */
#define OFF_SCENARIO_NAME_PTR    0x030
#define OFF_SCENARIO_NAME_LEN    0x038
#define OFF_DEF_DURATION_SEC     0x0a0
#define OFF_RUN_STOPWATCH        0x0d8
#define OFF_CAMERA_POS           0x138 /* vec3 (12 bytes) */
#define OFF_CAMERA_UP            0x144 /* vec3 (12 bytes) */
#define OFF_CAMERA_FRONT         0x150 /* vec3 (12 bytes) */
#define OFF_CAMERA_RIGHT         0x15c /* vec3 (12 bytes) */
#define OFF_CAMERA_PITCH         0x168 /* float */
#define OFF_CAMERA_YAW           0x16c /* float */
#define OFF_TARGETS_BEGIN        0x180 /* Target* */
#define OFF_TARGETS_END          0x188 /* Target* */
#define OFF_LAST_REMOVED_TARGET  0x198 /* Target (0xd0 bytes) */
#define OFF_HAS_LAST_REMOVED     0x268 /* bool */
#define OFF_RUN_STATE            0x43c /* int32_t ScenarioRunState */
#define OFF_EFFECTIVE_CM_PER_360 0x4e0 /* float */
#define OFF_RADIANS_PER_DOT      0x4f4 /* float */
#define OFF_STATS_NUM_HITS       0xa60 /* double */
#define OFF_STATS_NUM_SHOTS      0xa68 /* double */
#define OFF_STATS_NUM_KILLS      0xa70 /* double */

/* Field offsets inside aim::Target (sizeof == 0xd0) */
#define TARGET_STRIDE            0x0d0
#define OFF_TARGET_ID            0x000 /* uint16_t */
#define OFF_TARGET_POS           0x004 /* vec3 (12 bytes) */
#define OFF_TARGET_WALL_POS      0x010 /* vec2 (8 bytes) */
#define OFF_TARGET_HAS_WALL_POS  0x018 /* bool */
#define OFF_TARGET_RADIUS        0x020 /* float */
#define OFF_TARGET_HIDDEN        0x04a /* bool */
#define OFF_TARGET_IS_GHOST      0x04b /* bool */

/* ============================================================================
 * Plugin State & Trajectory Ring Buffer
 * ============================================================================ */

typedef void (*fn_void_self_t)(void *self);
typedef void (*fn_update_state_t)(void *self, void *update_data);
typedef int64_t (*fn_stopwatch_micros_t)(const void *stopwatch);

static uintptr_t             g_pie_base = 0;
static fn_void_self_t        orig_OnTickStart    = NULL;
static fn_void_self_t        orig_OnDetach       = NULL;
static fn_update_state_t     orig_UpdateState    = NULL;
static fn_void_self_t        orig_OnScenarioDone = NULL;
static fn_stopwatch_micros_t orig_GetElapsedMicros = NULL;

static anymouse_faf_shm_t   *g_shm = NULL;
static int32_t               g_prev_state = FAF_STATE_MENU;
static int64_t               g_last_shm_sync_us = -10000;

/* 1000 Hz Camera Trajectory Buffer for Submovement P1 Extraction */
#define TRAJ_MAX_SAMPLES 2048

typedef struct {
    int64_t time_us;
    float   yaw_rad;
    float   pitch_rad;
    float   vel_deg_s;
} cam_traj_sample_t;

static cam_traj_sample_t g_traj[TRAJ_MAX_SAMPLES];
static int               g_traj_len = 0;
static int64_t           g_last_traj_us = -10000;
static float             g_smooth_vel_deg_s = 0.0f;

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int angle_to_octant_deg(float deg) {
    int idx = (int)floorf((deg + 22.5f) / 45.0f);
    return ((idx % 8) + 8) % 8;
}

/* Convert 3D world target position into camera-space (yaw_rad, pitch_rad, radius_rad) */
static void target_world_to_angles(const float cam_pos[3],
                                   const float cam_right[3],
                                   const float cam_front[3],
                                   const float cam_up[3],
                                   const float target_pos[3],
                                   float target_radius,
                                   float *out_yaw_rad,
                                   float *out_pitch_rad,
                                   float *out_radius_rad) {
    float dx = target_pos[0] - cam_pos[0];
    float dy = target_pos[1] - cam_pos[1];
    float dz = target_pos[2] - cam_pos[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-5f) {
        *out_yaw_rad = 0.0f;
        *out_pitch_rad = 0.0f;
        *out_radius_rad = 0.0f;
        return;
    }
    float inv_d = 1.0f / dist;
    float vx = dx * inv_d;
    float vy = dy * inv_d;
    float vz = dz * inv_d;

    /* Project onto camera basis (matches MakeCoordinateSystemTransform in FpsAimForge) */
    float tx = vx * cam_right[0] + vy * cam_right[1] + vz * cam_right[2];
    float ty = vx * cam_front[0] + vy * cam_front[1] + vz * cam_front[2];
    float tz = vx * cam_up[0]    + vy * cam_up[1]    + vz * cam_up[2];

    if (tz > 1.0f) tz = 1.0f;
    if (tz < -1.0f) tz = -1.0f;

    *out_pitch_rad = asinf(tz);
    *out_yaw_rad   = atan2f(tx, ty);
    float sin_r    = target_radius * inv_d;
    if (sin_r > 1.0f) sin_r = 1.0f;
    *out_radius_rad = asinf(sin_r);
}

static void record_traj_sample(int64_t now_us, float yaw_rad, float pitch_rad, bool force) {
    if (!force && g_traj_len > 0 && (now_us - g_last_traj_us) < 1000) {
        return;
    }
    float inst_vel = 0.0f;
    if (g_traj_len > 0) {
        int prev_idx = g_traj_len - 1;
        int64_t dt_us = now_us - g_traj[prev_idx].time_us;
        if (dt_us > 0) {
            float dyaw_deg   = (yaw_rad - g_traj[prev_idx].yaw_rad) * RAD2DEG;
            float dpitch_deg = (pitch_rad - g_traj[prev_idx].pitch_rad) * RAD2DEG;
            float dist_deg   = hypotf(dyaw_deg, dpitch_deg);
            inst_vel         = (dist_deg * 1e6f) / (float)dt_us;
            g_smooth_vel_deg_s = 0.35f * inst_vel + 0.65f * g_smooth_vel_deg_s;
        } else {
            return;
        }
    } else {
        g_smooth_vel_deg_s = 0.0f;
    }

    if (g_traj_len >= TRAJ_MAX_SAMPLES) {
        /* Shift window by 512 samples if full */
        memmove(&g_traj[0], &g_traj[512], (TRAJ_MAX_SAMPLES - 512) * sizeof(cam_traj_sample_t));
        g_traj_len -= 512;
    }

    g_traj[g_traj_len].time_us   = now_us;
    g_traj[g_traj_len].yaw_rad   = yaw_rad;
    g_traj[g_traj_len].pitch_rad = pitch_rad;
    g_traj[g_traj_len].vel_deg_s = g_smooth_vel_deg_s;
    g_traj_len++;
    g_last_traj_us = now_us;
}

static void analyze_and_emit_submovement(const faf_target_info_t *target,
                                         bool is_hit,
                                         const char *scenario_name) {
    if (!g_shm || !target || g_traj_len < 6) {
        return;
    }

    /* Check if this target is the Center Home (0,0) reset target in a Center-Out drill */
    bool is_center_drill = (strstr(scenario_name, "Center-Out") != NULL ||
                            strstr(scenario_name, "Octant") != NULL);
    bool is_center_target = (is_center_drill && target->has_wall_pos &&
                             fabsf(target->wall_x) < 1.0f && fabsf(target->wall_y) < 1.0f);
    if (is_center_target) {
        /* Center return is only a posture reset to (0,0); do not emit a calibration sample */
        return;
    }

    /* 1. Find Peak Angular Velocity (omega_max) during this target acquisition */
    float peak_vel = 0.0f;
    int   idx_peak = -1;
    for (int i = 1; i < g_traj_len; i++) {
        if (g_traj[i].vel_deg_s > peak_vel) {
            peak_vel = g_traj[i].vel_deg_s;
            idx_peak = i;
        }
    }
    if (idx_peak < 1 || peak_vel < 18.0f) {
        return;
    }

    /* 2. Walk backward from idx_peak to find Movement Onset C_0 (idx_start) */
    float onset_thresh = fmaxf(8.0f, 0.06f * peak_vel);
    int   idx_start = 0;
    for (int i = idx_peak; i >= 0; i--) {
        if (g_traj[i].vel_deg_s <= onset_thresh) {
            idx_start = i;
            break;
        }
    }

    float yaw_0   = g_traj[idx_start].yaw_rad;
    float pitch_0 = g_traj[idx_start].pitch_rad;

    /* 3. True Target Vector T from movement origin C_0 to true 3D target center */
    float tx_deg = (target->yaw_rad   - yaw_0)   * RAD2DEG;
    float ty_deg = (target->pitch_rad - pitch_0) * RAD2DEG;
    float target_dist_deg = hypotf(tx_deg, ty_deg);

    /* Require a genuine ballistic movement (>= 1.5 degrees) */
    if (target_dist_deg < 1.5f) {
        return;
    }

    /* If missed click, verify click was aimed near this target (not a desk bump) */
    float shot_yaw_deg   = (g_traj[g_traj_len - 1].yaw_rad   - target->yaw_rad)   * RAD2DEG;
    float shot_pitch_deg = (g_traj[g_traj_len - 1].pitch_rad - target->pitch_rad) * RAD2DEG;
    float miss_err_deg   = hypotf(shot_yaw_deg, shot_pitch_deg);
    if (!is_hit && miss_err_deg > fmaxf(5.0f, 0.45f * target_dist_deg)) {
        return;
    }

    /* 4. Identify Primary Ballistic Submovement End P1 (idx_p1) after idx_peak */
    int   idx_p1 = g_traj_len - 1;
    float stop_thresh   = fmaxf(10.0f, 0.16f * peak_vel);
    float valley_thresh = 0.42f * peak_vel;

    for (int i = idx_peak + 1; i < g_traj_len; i++) {
        float cur_dx = (g_traj[i].yaw_rad   - yaw_0)   * RAD2DEG;
        float cur_dy = (g_traj[i].pitch_rad - pitch_0) * RAD2DEG;
        float progressed = hypotf(cur_dx, cur_dy);

        /* Must have covered at least 35% of the target distance past peak velocity */
        if (progressed < 0.35f * target_dist_deg) {
            continue;
        }

        /* Condition A: Velocity dropped below 16% of peak ballistic velocity */
        if (g_traj[i].vel_deg_s <= stop_thresh) {
            idx_p1 = i;
            break;
        }

        /* Condition B: Velocity hit a local minimum < 42% of peak (corrective submovement onset) */
        if (i + 1 < g_traj_len &&
            g_traj[i].vel_deg_s < valley_thresh &&
            g_traj[i].vel_deg_s <= g_traj[i - 1].vel_deg_s &&
            g_traj[i].vel_deg_s <  g_traj[i + 1].vel_deg_s) {
            idx_p1 = i;
            break;
        }
    }

    float p1_x_deg = (g_traj[idx_p1].yaw_rad   - yaw_0)   * RAD2DEG;
    float p1_y_deg = (g_traj[idx_p1].pitch_rad - pitch_0) * RAD2DEG;

    /* 5. Project P1 onto True Target Vector T */
    float ux = tx_deg / target_dist_deg;
    float uy = ty_deg / target_dist_deg;

    float d_along = p1_x_deg * ux + p1_y_deg * uy;
    float d_perp  = ux * p1_y_deg - uy * p1_x_deg;

    float radial_ratio = d_along / target_dist_deg;
    if (radial_ratio < 0.35f || radial_ratio > 2.20f) {
        return;
    }

    float angular_error_deg = atan2f(d_perp, d_along) * RAD2DEG;
    float target_angle_deg  = atan2f(ty_deg, tx_deg) * RAD2DEG;
    int   octant            = angle_to_octant_deg(target_angle_deg);
    float duration_ms       = (float)(g_traj[g_traj_len - 1].time_us - g_traj[idx_start].time_us) / 1000.0f;

    /* 6. Push into Shared Memory Event Ring */
    uint32_t w = atomic_load_explicit(&g_shm->write_idx, memory_order_relaxed);
    faf_submovement_event_t *ev = &g_shm->events[w & FAF_EVENT_RING_MASK];
    ev->round_id          = atomic_load_explicit(&g_shm->round_id, memory_order_relaxed);
    ev->timestamp_ns      = monotonic_ns();
    ev->octant            = octant;
    ev->target_angle_deg  = target_angle_deg;
    ev->target_dist_deg   = target_dist_deg;
    ev->p1_dist_deg       = d_along;
    ev->radial_ratio      = radial_ratio;
    ev->angular_error_deg = angular_error_deg;
    ev->peak_vel_deg_s    = peak_vel;
    ev->duration_ms       = duration_ms;
    ev->is_hit            = is_hit ? 1 : 0;
    ev->is_center_return  = 0;
    ev->target_id         = target->id;

    atomic_store_explicit(&g_shm->write_idx, w + 1, memory_order_release);
}

/* ============================================================================
 * Vtable Hook Callbacks Inside FpsAimForge
 * ============================================================================ */

static void sync_scenario_metadata(void *self, int32_t state) {
    if (!g_shm || !self) return;

    const char *name_ptr = *(const char **)((const char *)self + OFF_SCENARIO_NAME_PTR);
    size_t      name_len = *(const size_t *)((const char *)self + OFF_SCENARIO_NAME_LEN);
    if (name_ptr && name_len > 0 && name_len < 1024) {
        size_t copy_len = name_len < (sizeof(g_shm->scenario_name) - 1)
                              ? name_len
                              : (sizeof(g_shm->scenario_name) - 1);
        memcpy(g_shm->scenario_name, name_ptr, copy_len);
        g_shm->scenario_name[copy_len] = '\0';
    }

    g_shm->duration_seconds = (float)(*(const int32_t *)((const char *)self + OFF_DEF_DURATION_SEC));
    g_shm->cm_per_360       = *(const float *)((const char *)self + OFF_EFFECTIVE_CM_PER_360);
    g_shm->radians_per_dot  = *(const float *)((const char *)self + OFF_RADIANS_PER_DOT);

    if (orig_GetElapsedMicros) {
        int64_t us = orig_GetElapsedMicros((const char *)self + OFF_RUN_STOPWATCH);
        g_shm->elapsed_seconds = (float)us / 1e6f;
    }

    g_shm->num_hits  = *(const double *)((const char *)self + OFF_STATS_NUM_HITS);
    g_shm->num_shots = *(const double *)((const char *)self + OFF_STATS_NUM_SHOTS);
    g_shm->num_kills = *(const double *)((const char *)self + OFF_STATS_NUM_KILLS);

    atomic_store_explicit(&g_shm->run_state, state, memory_order_release);
    atomic_store_explicit(&g_shm->heartbeat_ns, monotonic_ns(), memory_order_release);
}

static void hook_OnTickStart(void *self) {
    orig_OnTickStart(self);
    if (!g_shm || !self) return;

    int32_t state = *(const int32_t *)((const char *)self + OFF_RUN_STATE);
    if (state == FAF_STATE_RUNNING && g_prev_state != FAF_STATE_RUNNING) {
        /* Round just transitioned into active RUNNING state! */
        atomic_fetch_add_explicit(&g_shm->round_id, 1, memory_order_relaxed);
        g_traj_len = 0;
        g_last_traj_us = -10000;
        g_last_shm_sync_us = -10000;
        g_smooth_vel_deg_s = 0.0f;
    }
    g_prev_state = state;

    if (state != FAF_STATE_RUNNING) {
        sync_scenario_metadata(self, state);
    }
}

static void hook_UpdateState(void *self, void *update_data) {
    if (!g_shm || !self) {
        orig_UpdateState(self, update_data);
        return;
    }

    int64_t now_us = orig_GetElapsedMicros
                         ? orig_GetElapsedMicros((const char *)self + OFF_RUN_STOPWATCH)
                         : 0;

    const float *cam_pos   = (const float *)((const char *)self + OFF_CAMERA_POS);
    const float *cam_up    = (const float *)((const char *)self + OFF_CAMERA_UP);
    const float *cam_front = (const float *)((const char *)self + OFF_CAMERA_FRONT);
    const float *cam_right = (const float *)((const char *)self + OFF_CAMERA_RIGHT);
    float cam_pitch = *(const float *)((const char *)self + OFF_CAMERA_PITCH);
    float cam_yaw   = *(const float *)((const char *)self + OFF_CAMERA_YAW);

    /* Record 1000 Hz camera trajectory */
    record_traj_sample(now_us, cam_yaw, cam_pitch, false);

    double prev_shots = *(const double *)((const char *)self + OFF_STATS_NUM_SHOTS);
    double prev_hits  = *(const double *)((const char *)self + OFF_STATS_NUM_HITS);
    double prev_kills = *(const double *)((const char *)self + OFF_STATS_NUM_KILLS);

    /* Snapshot visible targets before UpdateState processes clicks/kills */
    faf_target_info_t pre_targets[FAF_MAX_ACTIVE_TARGETS];
    uint32_t pre_count = 0;
    const char *t_begin = *(const char * const *)((const char *)self + OFF_TARGETS_BEGIN);
    const char *t_end   = *(const char * const *)((const char *)self + OFF_TARGETS_END);
    if (t_begin && t_end && t_end >= t_begin) {
        for (const char *p = t_begin; p < t_end && pre_count < FAF_MAX_ACTIVE_TARGETS; p += TARGET_STRIDE) {
            uint8_t hidden = *(const uint8_t *)(p + OFF_TARGET_HIDDEN);
            uint16_t tid   = *(const uint16_t *)(p + OFF_TARGET_ID);
            if (hidden || tid == 0) continue;

            faf_target_info_t *ti = &pre_targets[pre_count++];
            ti->id           = tid;
            ti->is_ghost     = *(const uint8_t *)(p + OFF_TARGET_IS_GHOST);
            ti->has_wall_pos = *(const uint8_t *)(p + OFF_TARGET_HAS_WALL_POS);
            const float *wpos = (const float *)(p + OFF_TARGET_WALL_POS);
            ti->wall_x       = ti->has_wall_pos ? wpos[0] : 0.0f;
            ti->wall_y       = ti->has_wall_pos ? wpos[1] : 0.0f;

            const float *tpos = (const float *)(p + OFF_TARGET_POS);
            float tradius     = *(const float *)(p + OFF_TARGET_RADIUS);
            target_world_to_angles(cam_pos, cam_right, cam_front, cam_up,
                                   tpos, tradius,
                                   &ti->yaw_rad, &ti->pitch_rad, &ti->radius_rad);
        }
    }

    /* Execute FpsAimForge's actual UpdateState */
    orig_UpdateState(self, update_data);

    double new_shots = *(const double *)((const char *)self + OFF_STATS_NUM_SHOTS);
    double new_hits  = *(const double *)((const char *)self + OFF_STATS_NUM_HITS);
    double new_kills = *(const double *)((const char *)self + OFF_STATS_NUM_KILLS);

    bool killed = (new_kills > prev_kills);
    bool shot   = (new_shots > prev_shots);

    if (killed || shot) {
        /* Force-record exact camera angle at shot/kill instant */
        record_traj_sample(now_us, cam_yaw, cam_pitch, true);

        faf_target_info_t engaged_target = {0};
        bool found_target = false;

        /* Prefer last_removed_target_ on kill (100% exact killed target) */
        uint8_t has_removed = *(const uint8_t *)((const char *)self + OFF_HAS_LAST_REMOVED);
        if (killed && has_removed) {
            const char *rem = (const char *)self + OFF_LAST_REMOVED_TARGET;
            engaged_target.id           = *(const uint16_t *)(rem + OFF_TARGET_ID);
            engaged_target.is_ghost     = 0;
            engaged_target.has_wall_pos = *(const uint8_t *)(rem + OFF_TARGET_HAS_WALL_POS);
            const float *wpos           = (const float *)(rem + OFF_TARGET_WALL_POS);
            engaged_target.wall_x       = engaged_target.has_wall_pos ? wpos[0] : 0.0f;
            engaged_target.wall_y       = engaged_target.has_wall_pos ? wpos[1] : 0.0f;

            const float *tpos = (const float *)(rem + OFF_TARGET_POS);
            float tradius     = *(const float *)(rem + OFF_TARGET_RADIUS);
            target_world_to_angles(cam_pos, cam_right, cam_front, cam_up,
                                   tpos, tradius,
                                   &engaged_target.yaw_rad,
                                   &engaged_target.pitch_rad,
                                   &engaged_target.radius_rad);
            found_target = true;
        } else if (pre_count > 0) {
            /* Find closest non-ghost target to crosshair at click instant */
            float best_dist = 1e9f;
            for (uint32_t i = 0; i < pre_count; i++) {
                if (pre_targets[i].is_ghost) continue;
                float d = hypotf(pre_targets[i].yaw_rad - cam_yaw,
                                 pre_targets[i].pitch_rad - cam_pitch);
                if (d < best_dist) {
                    best_dist = d;
                    engaged_target = pre_targets[i];
                    found_target = true;
                }
            }
        }

        if (found_target) {
            bool is_hit = (new_hits > prev_hits) || killed;
            analyze_and_emit_submovement(&engaged_target, is_hit, g_shm->scenario_name);
        }

        /* Reset trajectory to start fresh from this click/kill position */
        g_traj_len = 0;
        g_smooth_vel_deg_s = 0.0f;
        record_traj_sample(now_us, cam_yaw, cam_pitch, true);
    }

    /* Throttle shared-memory metadata updates to 1000 Hz or on shot/kill */
    if (killed || shot || (now_us - g_last_shm_sync_us) >= 1000) {
        g_shm->cam_yaw_rad   = cam_yaw;
        g_shm->cam_pitch_rad = cam_pitch;
        g_shm->active_target_count = pre_count;
        if (pre_count > 0) {
            memcpy(g_shm->active_targets, pre_targets, pre_count * sizeof(faf_target_info_t));
        }
        sync_scenario_metadata(self, FAF_STATE_RUNNING);
        g_last_shm_sync_us = now_us;
    }
}

static void hook_OnScenarioDone(void *self) {
    orig_OnScenarioDone(self);
    if (!g_shm) return;
    atomic_fetch_add_explicit(&g_shm->rounds_completed, 1, memory_order_relaxed);
    sync_scenario_metadata(self, FAF_STATE_DONE);
    g_prev_state = FAF_STATE_DONE;
}

static void hook_OnDetach(void *self) {
    if (g_shm && self) {
        int32_t state = *(const int32_t *)((const char *)self + OFF_RUN_STATE);
        if (g_prev_state == FAF_STATE_RUNNING && state != FAF_STATE_DONE) {
            atomic_fetch_add_explicit(&g_shm->rounds_aborted, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&g_shm->run_state, FAF_STATE_MENU, memory_order_release);
        atomic_store_explicit(&g_shm->heartbeat_ns, monotonic_ns(), memory_order_release);
        g_prev_state = FAF_STATE_MENU;
    }
    orig_OnDetach(self);
}

/* ============================================================================
 * Plugin Constructor & Destructor
 * ============================================================================ */

static int find_pie_base_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    (void)data;
    if (info->dlpi_name && info->dlpi_name[0] == '\0') {
        g_pie_base = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

__attribute__((constructor))
static void faf_plugin_init(void) {
    char exe_path[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0 || strstr(exe_path, "FpsAimForge") == NULL) {
        return; /* Only activate inside FpsAimForge */
    }

    dl_iterate_phdr(find_pie_base_cb, NULL);
    if (!g_pie_base) {
        return;
    }

    /* Verify binary vtable signature before patching */
    uintptr_t *base_vtable = (uintptr_t *)(g_pie_base + kScenarioVtables[0]);
    if (base_vtable[VSLOT_ON_TICK_START] != (g_pie_base + FAF_FN_ON_TICK_START) ||
        base_vtable[VSLOT_UPDATE_STATE]  != (g_pie_base + FAF_FN_UPDATE_STATE)) {
        fprintf(stderr, "[AnyMouse-FAF] Warning: FpsAimForge binary vtable mismatch, plugin disabled.\n");
        return;
    }

    /* Open & map POSIX shared memory (/dev/shm/anymouse_faf_telemetry).
     * Try without O_CREAT first so fs.protected_regular=1 never rejects opening
     * a /dev/shm file created by root or another user. */
    int fd = shm_open(ANYMOUSE_FAF_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        fd = shm_open(ANYMOUSE_FAF_SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            perror("[AnyMouse-FAF] shm_open");
            return;
        }
        (void)fchmod(fd, 0666);
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(anymouse_faf_shm_t)) {
        if (ftruncate(fd, sizeof(anymouse_faf_shm_t)) < 0) {
            perror("[AnyMouse-FAF] ftruncate");
            close(fd);
            return;
        }
    }

    g_shm = (anymouse_faf_shm_t *)mmap(NULL, sizeof(anymouse_faf_shm_t),
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        return;
    }

    if (g_shm->magic != ANYMOUSE_FAF_MAGIC || g_shm->version != ANYMOUSE_FAF_VERSION) {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->magic   = ANYMOUSE_FAF_MAGIC;
        g_shm->version = ANYMOUSE_FAF_VERSION;
    }

    atomic_store_explicit(&g_shm->faf_pid, (int32_t)getpid(), memory_order_release);
    atomic_store_explicit(&g_shm->run_state, FAF_STATE_MENU, memory_order_release);
    atomic_store_explicit(&g_shm->heartbeat_ns, monotonic_ns(), memory_order_release);

    orig_OnTickStart      = (fn_void_self_t)(g_pie_base + FAF_FN_ON_TICK_START);
    orig_OnDetach         = (fn_void_self_t)(g_pie_base + FAF_FN_ON_DETACH);
    orig_UpdateState      = (fn_update_state_t)(g_pie_base + FAF_FN_UPDATE_STATE);
    orig_OnScenarioDone   = (fn_void_self_t)(g_pie_base + FAF_FN_ON_SCENARIO_DONE);
    orig_GetElapsedMicros = (fn_stopwatch_micros_t)(g_pie_base + FAF_FN_STOPWATCH_GET_ELAPSED_MICROS);

    /* Patch all 13 Scenario vtables in .data.rel.ro */
    long page_size = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < sizeof(kScenarioVtables) / sizeof(kScenarioVtables[0]); i++) {
        uintptr_t *vt = (uintptr_t *)(g_pie_base + kScenarioVtables[i]);
        uintptr_t page_start = ((uintptr_t)vt) & ~((uintptr_t)page_size - 1);
        if (mprotect((void *)page_start, (size_t)page_size * 2, PROT_READ | PROT_WRITE) == 0) {
            vt[VSLOT_ON_TICK_START]    = (uintptr_t)hook_OnTickStart;
            vt[VSLOT_ON_DETACH]        = (uintptr_t)hook_OnDetach;
            vt[VSLOT_UPDATE_STATE]     = (uintptr_t)hook_UpdateState;
            vt[VSLOT_ON_SCENARIO_DONE] = (uintptr_t)hook_OnScenarioDone;
            mprotect((void *)page_start, (size_t)page_size * 2, PROT_READ);
        }
    }

    fprintf(stderr, "[AnyMouse-FAF] Native Telemetry Plugin active (PID %d -> %s)\n",
            (int)getpid(), ANYMOUSE_FAF_SHM_NAME);
}

__attribute__((destructor))
static void faf_plugin_fini(void) {
    if (g_shm) {
        atomic_store_explicit(&g_shm->run_state, FAF_STATE_NOT_STARTED, memory_order_release);
        atomic_store_explicit(&g_shm->faf_pid, 0, memory_order_release);
        munmap(g_shm, sizeof(*g_shm));
        g_shm = NULL;
    }
}
