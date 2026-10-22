/*
 * AnyMouse-Vectoring — FpsAimForge (FAF) Native Telemetry Plugin Protocol
 * =======================================================================
 * Shared memory IPC contract between:
 *   1. libanymouse_faf.so (loaded inside FpsAimForge)
 *   2. ./anymouse (Phase 2 Calibration & GTK4 Studio GUI)
 *
 * Mapped at POSIX shared memory path: /dev/shm/anymouse_faf_telemetry
 */

#ifndef ANYMOUSE_FAF_TELEMETRY_H
#define ANYMOUSE_FAF_TELEMETRY_H

#include <stdint.h>
#include <stdatomic.h>

#define ANYMOUSE_FAF_SHM_NAME "/anymouse_faf_telemetry"
#define ANYMOUSE_FAF_MAGIC    0x414D4641U /* "AMFA" */
#define ANYMOUSE_FAF_VERSION  1U

#define FAF_MAX_ACTIVE_TARGETS 16
#define FAF_EVENT_RING_SIZE    256
#define FAF_EVENT_RING_MASK    (FAF_EVENT_RING_SIZE - 1)

/* Matches aim::ScenarioRunState in FpsAimForge src/aim/scenario/scenario.h */
typedef enum {
    FAF_STATE_NOT_STARTED               = 0,
    FAF_STATE_WAITING_FOR_CLICK_TO_START = 1,
    FAF_STATE_START_COUNTDOWN           = 2,
    FAF_STATE_RUNNING                   = 3,
    FAF_STATE_DONE                      = 4,
    FAF_STATE_MENU                      = 10
} faf_run_state_t;

/* Snapshot of an active visible target on the wall in 3D angular camera space */
typedef struct {
    uint16_t id;
    uint8_t  is_ghost;
    uint8_t  has_wall_pos;
    float    yaw_rad;
    float    pitch_rad;
    float    radius_rad;
    float    wall_x;
    float    wall_y;
} faf_target_info_t;

/*
 * Verified Primary Ballistic Submovement Event emitted by libanymouse_faf.so
 * upon target engagement (click/kill) during an active FpsAimForge round.
 */
typedef struct {
    uint32_t round_id;
    uint64_t timestamp_ns;
    int32_t  octant;            /* 0..7 (E, NE, N, NW, W, SW, S, SE) */
    float    target_angle_deg;  /* Polar angle of true target vector T (-180..+180) */
    float    target_dist_deg;   /* ||T|| true angular distance from flick origin to target center (deg) */
    float    p1_dist_deg;       /* P1 . T_hat primary ballistic projection along target axis (deg) */
    float    radial_ratio;      /* R = (P1 . T_hat) / ||T|| (<1.0 undershoot, >1.0 overshoot) */
    float    angular_error_deg; /* Signed angular deflection of P1 relative to T (deg) */
    float    peak_vel_deg_s;    /* Peak angular velocity during primary ballistic phase (deg/s) */
    float    duration_ms;       /* Total acquisition time from movement onset to click/kill (ms) */
    uint8_t  is_hit;            /* 1 = target hit/killed, 0 = missed click */
    uint8_t  is_center_return;  /* 1 = return to center (0,0) home target in Center-Out drill */
    uint16_t target_id;         /* FpsAimForge target ID */
} faf_submovement_event_t;

/* Shared Memory Region (/dev/shm/anymouse_faf_telemetry) */
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    _Atomic int32_t   faf_pid;
    _Atomic uint64_t  heartbeat_ns;

    /* Scenario & Round Lifecycle State */
    _Atomic int32_t   run_state;        /* faf_run_state_t */
    _Atomic uint32_t  round_id;         /* Increments on each RUNNING start */
    _Atomic uint32_t  rounds_completed; /* Increments on OnScenarioDone */
    _Atomic uint32_t  rounds_aborted;   /* Increments on early exit/restart */

    char              scenario_name[128];
    float             cm_per_360;
    float             radians_per_dot;
    float             elapsed_seconds;
    float             duration_seconds;

    /* Live Round Score / Hit Stats */
    double            num_shots;
    double            num_hits;
    double            num_kills;

    /* Live Camera Orientation (radians) */
    float             cam_yaw_rad;
    float             cam_pitch_rad;

    /* Active Visible Targets */
    uint32_t          active_target_count;
    faf_target_info_t active_targets[FAF_MAX_ACTIVE_TARGETS];

    /* Lock-free SPSC/MPSC Submovement Event Ring Buffer */
    _Atomic uint32_t        write_idx;
    _Atomic uint32_t        read_idx;
    faf_submovement_event_t events[FAF_EVENT_RING_SIZE];
} anymouse_faf_shm_t;

#endif /* ANYMOUSE_FAF_TELEMETRY_H */
