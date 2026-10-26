#ifndef ANYMOUSE_CORE_H
#define ANYMOUSE_CORE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../common/amv_dongle_proto.h"

#define PI 3.14159265358979323846f
#define TWO_PI (2.0f * PI)
#define INV_OCTANT_STEP (4.0f / PI)
#define UDP_PORT 42069

/* Maximum mice tracked by the universal device scanner (src/devices.c) */
#define DEVICES_MAX 32

/* Lock-free SPSC ring buffer capacity (power of 2: 65536 ticks) */
#define TICK_RING_SIZE 65536U
#define TICK_RING_MASK (TICK_RING_SIZE - 1U)

/* Maximum trials stored per octant per calibration round */
#define MAX_OCTANT_TRIALS 64
#define DEFAULT_TRIALS_PER_ROUND 5

/* Damped Calibration Hyperparameters (Phase 2 Spec) */
#define CALIB_LEARNING_RATE_ALPHA 0.35f
#define CALIB_MAX_STEP_CLAMP      0.05f   /* +/- 5% max change per round */
#define SUBMOVEMENT_VEL_RATIO     0.15f   /* v(t) < 0.15 * v_peak marks P_1 */

/* Explicit KovaaK's Session States for Strict Telemetry Gating */
typedef enum {
    KOVAAKS_STATE_OFFLINE = 0,       /* FPSAimTrainer-Win64-Shipping.exe not running */
    KOVAAKS_STATE_MENU_OR_LOBBY = 1, /* Game running, in Main Menu / Lobby (no active bots) */
    KOVAAKS_STATE_PAUSED = 2,        /* Scenario loaded, but pause menu is open */
    KOVAAKS_STATE_ACTIVE_ROUND = 3   /* Actively playing scenario/challenge with spawned bots */
} kovaaks_session_state_t;

/* Single mouse polling frame emitted by the real-time evdev thread */
typedef struct {
    uint64_t timestamp_ns;
    int16_t  raw_dx;
    int16_t  raw_dy;
    int16_t  scaled_dx;
    int16_t  scaled_dy;
    uint8_t  buttons;       /* bit 0: BTN_LEFT, bit 1: BTN_RIGHT */
    uint8_t  octant_idx;    /* Dominant octant [0..7] */
} tick_event_t;

/* A physical pointing device discovered under /sys/class/input */
typedef struct {
    char     event_path[64];    /* /dev/input/eventN */
    char     byid_path[256];    /* Stable /dev/input/by-id link, "" if none */
    char     name[128];
    char     usb_parent[128];   /* usbN/N-P, shared by every interface of one USB device */
    uint16_t vid;
    uint16_t pid;
    bool     hw_supported;      /* Known vendor protocol (CPI / polling control) */
    bool     is_dongle;         /* Output of an AnyMouse dongle (already vectored): never auto-grabbed */
    int      score;             /* Auto-selection rank, highest wins */
} mouse_device_t;

/* AnyMouse hardware dongle link state (src/dongle.c) */
typedef enum {
    DONGLE_NONE = 0,         /* no AnyMouse vendor interface present */
    DONGLE_NO_ACCESS,        /* interface present, but its hidraw node cannot be opened */
    DONGLE_NOT_RESPONDING,   /* opened, no handshake reply yet (retried) */
    DONGLE_INCOMPATIBLE,     /* AnyMouse framing at another protocol version: guarded, not synced */
    DONGLE_LINKED            /* handshake passed; weights kept in sync */
} dongle_link_t;

typedef struct {
    dongle_link_t link;
    bool     legacy;            /* firmware predates HELLO (recognised by GET_INFO) */
    bool     vectoring;         /* the dongle's mouse output is already vectored */
    char     node[64];          /* /dev/hidrawN */
    uint8_t  proto_ver;
    uint8_t  fw[3];
    uint8_t  mac[6];
    uint16_t caps;              /* AMV_CAP_* */
    uint32_t gen;
    bool     bypass;
    uint8_t  active_slot;
    float    weights[8];
    char     detail[160];
} dongle_status_t;

/* Hardware telemetry decoded directly over /dev/hidrawX (OpenMouse protocol) */
typedef struct {
    bool     connected;
    bool     accessible;
    bool     motion_sync;
    uint16_t vid;
    uint16_t pid;
    int      dpi;
    int      polling_rate_hz;
    int      battery_mv;
    int      battery_pct;
    int      rssi_dbm;
    int      lod_mm;
    char     brand[32];
    char     model[32];
    char     node[64];
    char     serial[32];
    char     mouse_fw[32];
    char     dongle_fw[32];
    int      supported_rates[8];  /* Polling rates the device accepts, 0-terminated */
    /* Set when the mouse sits behind the AnyMouse dongle and is reached through it */
    bool     via_dongle;
    int      tier;                /* AMV_TIER_* (dongle only) */
    bool     can_set_cpi;
    bool     can_set_rate;
} mouse_hw_status_t;

/* A mouse plugged into the dongle, as the dongle reports it (GET_MOUSE / GET_MOUSE_CAPS) */
typedef struct {
    amv_mouse_info_t info;
    amv_mouse_caps_t caps;
    bool             has_caps;
} dongle_mouse_t;

/* Per-octant ballistic submovement calibration statistics */
typedef struct {
    float    ratios[MAX_OCTANT_TRIALS];     /* Landing ratios r_i = (P_1 . T) / ||T||^2 */
    float    skews_deg[MAX_OCTANT_TRIALS];  /* Angular error theta(P_1) - theta(T) */
    int      trial_count;                   /* Trials collected in current round */
    int      total_flicks;                  /* Cumulative flicks across session */
    float    last_ratio;
    float    median_ratio;                  /* Outlier-resistant median r_tilde_k */
    float    mean_skew_deg;
    float    suggested_weight;
} octant_stats_entry_t;

/* Live Aim-Trainer (FpsAimForge Primary / KovaaK's Sunset) + Ballistic Flick Telemetry Snapshot */
typedef struct {
    /* FpsAimForge (FAF) Native Plugin Live Telemetry */
    bool     faf_connected;
    pid_t    faf_pid;
    int      faf_run_state;        /* faf_run_state_t (0..4, 10=MENU) */
    uint32_t faf_round_id;
    uint32_t faf_rounds_completed;
    uint32_t faf_rounds_aborted;
    char     faf_scenario[128];
    float    faf_cm_per_360;
    float    faf_elapsed_sec;
    float    faf_duration_sec;
    int      faf_shots;
    int      faf_hits;
    int      faf_kills;

    /* KovaaK's Process & Live UE4 Session State (Sunset on Linux / Retained for Windows) */
    kovaaks_session_state_t session_state;
    bool     kovaaks_running;
    bool     ue4_memory_attached;
    pid_t    kovaaks_pid;
    uint64_t uworld_ptr;
    float    cam_yaw;
    float    cam_pitch;
    float    cam_fov;
    int      active_bot_count;
    float    nearest_bot_yaw_delta;
    float    nearest_bot_pitch_delta;

    /* Live In-Round Stats read from AMetaCharacter in UE4 RAM */
    int      live_kill_count;
    int      live_overshots;
    float    live_damage_done;
    float    live_score;
    float    live_last_shot_time;
    int64_t  live_challenge_start_ticks;
    int      active_round_number;

    /* Latest Completed KovaaK's CSV Stats Summary */
    char     last_scenario[96];
    int      last_kills;
    float    last_accuracy;
    float    last_avg_ttk;
    int      last_overshots;
    float    csv_horiz_sens;
    char     csv_sens_scale[32];

    /* Last Validated In-Game Ballistic Flick (P_1 vs T) */
    uint64_t last_flick_time_ns;
    int      last_flick_octant;
    float    last_p1_x;
    float    last_p1_y;
    float    last_t_x;
    float    last_t_y;
    float    last_v_peak;       /* counts / ms */
    float    last_landing_ratio;
    float    last_skew_deg;
    int      total_valid_flicks;
    int      rounds_applied;

    octant_stats_entry_t octant[8];
} calib_telemetry_t;

/* Global Unified Process State */
typedef struct {
    /* Hot-path atomic weights (read lock-free by SCHED_FIFO evdev thread) */
    _Atomic float    octant_weights[8];
    _Atomic int      hardware_cpi;
    _Atomic bool     keep_running;
    _Atomic bool     evdev_grabbed;
    _Atomic bool     auto_calibrate;
    _Atomic int      trials_per_round;
    _Atomic uint64_t total_ticks_processed;

    /* Device configuration (strings guarded by device_mutex) */
    pthread_mutex_t  device_mutex;
    char             evdev_path[256];     /* Device the RT engine currently holds / last tried */
    char             evdev_error[160];
    char             active_name[128];
    char             requested_path[256]; /* "" = automatic selection */
    _Atomic uint32_t device_request_gen;  /* Bumped to make the RT engine re-select */
    _Atomic int      active_vid;          /* 0 when no device is grabbed */
    _Atomic int      active_pid;

    /* Bumped whenever the dongle link writes octant_weights (the GUI re-syncs its widgets) */
    _Atomic uint32_t dongle_weights_rev;
    /* Bumped whenever the list of mice behind the dongle (or their CPI / rate / battery) changes */
    _Atomic uint32_t dongle_mice_rev;

    /* Lock-free SPSC ring buffer: Thread 1 (evdev) -> Thread 2 (calibration) */
    tick_event_t     tick_ring[TICK_RING_SIZE];
    _Atomic uint32_t ring_head;
    _Atomic uint32_t ring_tail;

    /* Mutex-protected telemetry snapshots for UI thread */
    pthread_mutex_t  telemetry_mutex;
    mouse_hw_status_t hw_status;
    calib_telemetry_t calib;
} anymouse_state_t;

extern anymouse_state_t g_state;

/* Octant Metadata */
typedef struct {
    const char *name;
    const char *short_name;
    float       angle_deg;
    const char *desc;
} octant_info_t;

extern const octant_info_t OCTANT_INFO[8];

/* Thread & Subsystem Entry Points */
void  core_init(float initial_scale, const char *dev_path_override);
void  core_apply_preset(const char *preset_name);
bool  core_save_profile_json(const char *filepath);
bool  core_load_profile_json(const char *filepath);
bool  core_autoload_profile(char *used_path, size_t sz);  /* Restore last saved state at startup */
bool  core_autosave_profile(char *used_path, size_t sz);  /* Persist current state to the user's profile */

/* Thread 1: Real-time evdev + uinput vectoring engine (src/evdev_rt.c) */
void *evdev_rt_thread(void *arg);

/* Universal mouse discovery & selection (src/devices.c) */
int   devices_enumerate(mouse_device_t *out, int max);
bool  devices_pick_auto(mouse_device_t *out);
bool  devices_lookup(const char *path, mouse_device_t *out);
bool  devices_is_hw_supported(uint16_t vid, uint16_t pid);
const char *devices_preferred_path(const mouse_device_t *d);
int   devices_identify_by_motion(mouse_device_t *out, int timeout_ms);
void  devices_request_switch(const char *path);
void  devices_get_request(char *out, size_t out_sz);
void  devices_request_reselect(void);
void  devices_usb_parent_of(const char *sysfs_real, char *out, size_t out_sz);

/* AnyMouse hardware dongle: discovery by protocol handshake, guard, weight sync (src/dongle.c) */
void *dongle_thread(void *arg);
bool  dongle_guards_usb_device(const char *usb_parent);  /* mice on this USB device must not be grabbed */
bool  dongle_guard_active(void);                         /* any dongle (or unverified candidate) present */
bool  dongle_get_status(dongle_status_t *out);           /* true when linked */
int   dongle_cli(const char *action, const char *arg);   /* --dongle-status, --dongle-bypass on|off */
int   dongle_get_mice(dongle_mouse_t *out, int max);      /* -1: no link, or firmware without mouse info */
/* Write a mouse setting through the dongle (runs on the dongle thread). Returns the AMV_ST_* status,
 * or -1 when not linked / timed out. */
int   dongle_mouse_set(uint8_t index, bool cpi, uint16_t value);
bool  dongle_fill_hw_status(mouse_hw_status_t *out);      /* first mouse behind the dongle -> hardware panel */
int   dongle_first_mouse_set(bool cpi, uint16_t value);   /* dongle_mouse_set() on that mouse */

/* Persistent user config & invoking-user helpers (src/config.c) */
bool  config_user_home(char *out, size_t out_sz);
bool  config_exe_dir(char *out, size_t out_sz);
void  config_chown_to_user(const char *path);
bool  config_mkdirs_for_user(const char *dir);
void  config_load(void);
bool  config_save(void);
bool  config_profile_path(char *out, size_t out_sz, bool create_dir);

/* Hardware HIDraw driver (src/hid_driver.c) */
bool  hid_query_active_mouse(mouse_hw_status_t *out_status);
bool  hid_set_mouse_cpi(int cpi);
bool  hid_set_mouse_motion_sync(bool enabled);
bool  hid_set_mouse_polling_rate(int hz);
bool  hid_descriptor_has_collection(const char *hidraw_name, uint16_t want_page, uint16_t want_usage);

/* FpsAimForge Steam detection & plugin/bundle installer (src/steam_install.c) */
typedef enum {
    FAF_INSTALL_GAME_NOT_FOUND = 0,  /* No Steam library contains FpsAimForge */
    FAF_INSTALL_NOT_INSTALLED,       /* Game found, AnyMouse launcher not installed */
    FAF_INSTALL_NEEDS_REPAIR,        /* Steam updated the game and replaced our launcher */
    FAF_INSTALL_OUTDATED,            /* Installed, but plugin/bundle differ from this build */
    FAF_INSTALL_OK
} faf_install_state_t;

typedef struct {
    faf_install_state_t state;
    char game_dir[PATH_MAX];
    char bundle_dir[PATH_MAX];
    char detail[256];
} faf_install_info_t;

bool  faf_install_detect(faf_install_info_t *out);
bool  faf_install_apply(char *msg, size_t msg_sz);
bool  faf_install_remove(char *msg, size_t msg_sz);
bool  udev_install_rules(char *msg, size_t msg_sz);

/* Thread 2: Ballistic Submovement & KovaaK's Telemetry Engine (src/calibration.c) */
void *calibration_thread(void *arg);
void  calibration_reset_stats(void);
int   calibration_apply_damped_update(bool require_min_trials);

/* Thread 3 / Main: Native GTK4 + Cairo Studio GUI (src/gui_gtk4.c) */
int   gui_run(int argc, char **argv);

#endif /* ANYMOUSE_CORE_H */
