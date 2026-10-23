#define _GNU_SOURCE
#include "core.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

anymouse_state_t g_state;

const octant_info_t OCTANT_INFO[8] = {
    { "East (0°)",        "E",  0.0f,   "Right lateral swipe" },
    { "North-East (45°)", "NE", 45.0f,  "Up-Right flick" },
    { "North (90°)",      "N",  90.0f,  "Upward push" },
    { "North-West (135°)","NW", 135.0f, "Up-Left flick" },
    { "West (180°)",      "W",  180.0f, "Left lateral swipe" },
    { "South-West (225°)","SW", 225.0f, "Down-Left pull" },
    { "South (270°)",     "S",  270.0f, "Downward pull" },
    { "South-East (315°)","SE", 315.0f, "Down-Right pull" }
};

static void sig_handler(int sig) {
    (void)sig;
    atomic_store(&g_state.keep_running, false);
}

void core_init(float initial_scale, const char *dev_path_override) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.telemetry_mutex, NULL);
    pthread_mutex_init(&g_state.device_mutex, NULL);

    for (int i = 0; i < 8; i++) {
        atomic_store(&g_state.octant_weights[i], initial_scale);
    }
    atomic_store(&g_state.hardware_cpi, 3200);
    atomic_store(&g_state.keep_running, true);
    atomic_store(&g_state.evdev_grabbed, false);
    atomic_store(&g_state.auto_calibrate, false);
    atomic_store(&g_state.trials_per_round, DEFAULT_TRIALS_PER_ROUND);
    atomic_store(&g_state.total_ticks_processed, 0ULL);
    atomic_store(&g_state.ring_head, 0U);
    atomic_store(&g_state.ring_tail, 0U);

    /* "" = let the RT engine auto-select the best-ranked mouse (src/devices.c) */
    if (dev_path_override && dev_path_override[0] != '\0') {
        snprintf(g_state.requested_path, sizeof(g_state.requested_path), "%s", dev_path_override);
    }
}

void core_apply_preset(const char *preset_name) {
    static const struct {
        const char *name;
        float w[8];
    } table[] = {
        { "neutral",          { 0.250f, 0.250f, 0.250f, 0.250f, 0.250f, 0.250f, 0.250f, 0.250f } },
        { "passthrough_1x",   { 1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 1.000f } },
        { "half_05x",         { 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f } },
        { "high",             { 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f, 0.500f } },
        { "low",              { 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f, 0.125f } },
        { "vertical_boost",   { 0.250f, 0.350f, 0.450f, 0.350f, 0.250f, 0.250f, 0.250f, 0.250f } },
        { "horizontal_boost", { 0.400f, 0.250f, 0.250f, 0.250f, 0.400f, 0.250f, 0.250f, 0.250f } },
        { "diagonal_ulx",     { 0.250f, 0.300f, 0.250f, 0.280f, 0.250f, 0.220f, 0.250f, 0.260f } },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(preset_name, table[i].name) == 0) {
            for (int k = 0; k < 8; k++) {
                atomic_store_explicit(&g_state.octant_weights[k], table[i].w[k], memory_order_relaxed);
            }
            return;
        }
    }
}

bool core_save_profile_json(const char *filepath) {
    /* Write beside the target and rename, so a crash can't leave a truncated profile */
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", filepath);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    int cpi = atomic_load(&g_state.hardware_cpi);
    float w[8];
    for (int i = 0; i < 8; i++) w[i] = atomic_load(&g_state.octant_weights[i]);

    fprintf(f, "{\n");
    fprintf(f, "  \"hardware_cpi\": %d,\n", cpi);
    fprintf(f, "  \"octant_weights\": [%.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f],\n",
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    fprintf(f, "  \"effective_dpi\": [%d, %d, %d, %d, %d, %d, %d, %d]\n",
            (int)lroundf(w[0] * cpi), (int)lroundf(w[1] * cpi),
            (int)lroundf(w[2] * cpi), (int)lroundf(w[3] * cpi),
            (int)lroundf(w[4] * cpi), (int)lroundf(w[5] * cpi),
            (int)lroundf(w[6] * cpi), (int)lroundf(w[7] * cpi));
    fprintf(f, "}\n");
    bool ok = (fclose(f) == 0);
    if (ok && rename(tmp, filepath) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    config_chown_to_user(filepath);
    return true;
}

/* The profile lives in the user's config dir; a profile.json beside the binary
 * (the pre-autosave location) is still read once so existing work carries over. */
bool core_autoload_profile(char *used_path, size_t sz) {
    char path[PATH_MAX + 32];
    if (config_profile_path(path, sizeof(path), false) && core_load_profile_json(path)) {
        snprintf(used_path, sz, "%s", path);
        return true;
    }
    char dir[PATH_MAX];
    if (config_exe_dir(dir, sizeof(dir))) {
        snprintf(path, sizeof(path), "%s/profile.json", dir);
        if (core_load_profile_json(path)) {
            snprintf(used_path, sz, "%s", path);
            return true;
        }
    }
    return false;
}

bool core_autosave_profile(char *used_path, size_t sz) {
    char path[PATH_MAX + 32];
    if (!config_profile_path(path, sizeof(path), true)) return false;
    snprintf(used_path, sz, "%s", path);
    return core_save_profile_json(path);
}

bool core_load_profile_json(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *cpi_pos = strstr(buf, "\"hardware_cpi\"");
    if (cpi_pos) {
        const char *colon = strchr(cpi_pos, ':');
        if (colon) {
            int cpi = atoi(colon + 1);
            if (cpi >= 50 && cpi <= 26000) {
                atomic_store(&g_state.hardware_cpi, cpi);
            }
        }
    }

    const char *w_pos = strstr(buf, "\"octant_weights\"");
    if (!w_pos) return false;
    const char *br = strchr(w_pos, '[');
    if (!br) return false;

    float w[8];
    int parsed = sscanf(br + 1, "%f , %f , %f , %f , %f , %f , %f , %f",
                        &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7]);
    if (parsed == 8) {
        for (int i = 0; i < 8; i++) {
            if (w[i] >= 0.01f && w[i] <= 5.0f) {
                atomic_store(&g_state.octant_weights[i], w[i]);
            }
        }
        return true;
    }
    return false;
}

static void print_usage(const char *prog) {
    printf("AnyMouse-Vectoring — Unified C 8-Octant Polar Vectoring & KovaaK's Telemetry Engine\n\n");
    printf("Usage: %s [OPTIONS] [DEVICE_PATH] [BASE_SCALE]\n\n", prog);
    printf("Options:\n");
    printf("  --headless           Run without GTK4 GUI (daemon + KovaaK's telemetry in CLI mode)\n");
    printf("  --auto-calibrate     Automatically apply damped weight updates (alpha=0.35, +/-5%% clamp)\n");
    printf("  --preset <name>      Start with preset (neutral, vertical_boost, horizontal_boost, diagonal_ulx, low)\n");
    printf("  --cpi <value>        Set physical mouse sensor CPI on startup (e.g. 3200 or 6400)\n");
    printf("  --probe-hw           Query connected mouse hardware telemetry over /dev/hidraw and exit\n");
    printf("  --list-devices       List detected mice (auto-selection order) and exit\n");
    printf("  --faf-status         Show FpsAimForge (Steam) plugin & bundle install status and exit\n");
    printf("  --install-faf        Install/repair the FpsAimForge launcher, plugin and AnyMouse bundle\n");
    printf("  --uninstall-faf      Restore the original FpsAimForge binary and remove the AnyMouse files\n");
    printf("  --install-udev       (root) Install udev rules so AnyMouse runs without sudo\n");
    printf("\nDEVICE_PATH overrides auto-detection (e.g. /dev/input/by-id/usb-...-event-mouse).\n");
    printf("  -h, --help           Show this help message\n");
}

static int run_cli_action(const char *action) {
    char msg[512];
    if (strcmp(action, "--list-devices") == 0) {
        mouse_device_t list[DEVICES_MAX];
        int n = devices_enumerate(list, DEVICES_MAX);
        if (n == 0) {
            printf("No mice detected under /sys/class/input.\n");
            return 1;
        }
        printf("Detected mice (first = auto-selected):\n");
        for (int i = 0; i < n; i++) {
            printf("  %s %-40s %04x:%04x score=%-4d %s%s\n", i == 0 ? "*" : " ",
                   list[i].name, list[i].vid, list[i].pid, list[i].score,
                   devices_preferred_path(&list[i]),
                   list[i].hw_supported ? "  [hardware controls]" : "");
        }
        return 0;
    }
    if (strcmp(action, "--faf-status") == 0) {
        faf_install_info_t info;
        faf_install_detect(&info);
        printf("FpsAimForge: %s\n", info.detail);
        if (info.game_dir[0])   printf("  Game dir  : %s\n", info.game_dir);
        if (info.bundle_dir[0]) printf("  Bundle dir: %s\n", info.bundle_dir);
        return info.state == FAF_INSTALL_OK ? 0 : 1;
    }
    bool ok;
    if (strcmp(action, "--install-faf") == 0)        ok = faf_install_apply(msg, sizeof(msg));
    else if (strcmp(action, "--uninstall-faf") == 0) ok = faf_install_remove(msg, sizeof(msg));
    else                                             ok = udev_install_rules(msg, sizeof(msg));
    printf("%s %s\n", ok ? "[OK]" : "[FAIL]", msg);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    bool headless = false;
    bool auto_cal = false;
    bool probe_only = false;
    const char *action = NULL;
    int  startup_cpi = 0;
    float base_scale = 0.25f;
    const char *dev_path = NULL;
    const char *preset = NULL;
    bool scale_given = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--auto-calibrate") == 0) {
            auto_cal = true;
        } else if (strcmp(argv[i], "--probe-hw") == 0) {
            probe_only = true;
        } else if (strcmp(argv[i], "--list-devices") == 0 || strcmp(argv[i], "--faf-status") == 0 ||
                   strcmp(argv[i], "--install-faf") == 0 || strcmp(argv[i], "--uninstall-faf") == 0 ||
                   strcmp(argv[i], "--install-udev") == 0) {
            action = argv[i];
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset = argv[++i];
        } else if (strcmp(argv[i], "--cpi") == 0 && i + 1 < argc) {
            startup_cpi = atoi(argv[++i]);
        } else if (argv[i][0] == '/') {
            dev_path = argv[i];
        } else {
            float v = strtof(argv[i], NULL);
            if (v >= 0.01f && v <= 4.0f) {
                base_scale = v;
                scale_given = true;
            }
        }
    }

    core_init(base_scale, dev_path);
    if (!dev_path) {
        config_load();
    }

    if (action) {
        return run_cli_action(action);
    }

    /* Restore the last session's weights unless the command line asked for specific ones.
     * Those explicit overrides are session-only and are not written back on exit. */
    bool persist_profile = !preset && !scale_given;
    if (persist_profile && !probe_only) {
        char used[PATH_MAX + 32];
        if (core_autoload_profile(used, sizeof(used))) {
            printf("[PROFILE] Restored last state from %s\n", used);
        }
    }
    if (preset) {
        core_apply_preset(preset);
    }
    if (auto_cal) {
        atomic_store(&g_state.auto_calibrate, true);
    }

    if (probe_only) {
        mouse_hw_status_t st;
        if (hid_query_active_mouse(&st)) {
            printf("=== OpenMouse Hardware Telemetry ===\n");
            printf("  Device      : %s %s (%s)\n", st.brand, st.model, st.node);
            printf("  Serial      : %s\n", st.serial[0] ? st.serial : "N/A");
            printf("  Sensor CPI  : %d\n", st.dpi);
            printf("  Polling Rate: %d Hz\n", st.polling_rate_hz);
            printf("  Battery     : %d%% (%d mV)\n", st.battery_pct, st.battery_mv);
            printf("  Motion Sync : %s\n", st.motion_sync ? "ON" : "OFF");
            printf("  Signal RSSI : %d dBm\n", st.rssi_dbm);
            return 0;
        } else {
            printf("No responsive OpenMouse device found.\n");
            return 1;
        }
    }

    if (startup_cpi > 0) {
        hid_set_mouse_cpi(startup_cpi);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Launch Thread 1: Real-time evdev/uinput vectoring engine */
    pthread_t rt_tid;
    pthread_create(&rt_tid, NULL, evdev_rt_thread, NULL);

    /* Launch Thread 2: Ballistic Submovement & KovaaK's Telemetry Engine */
    pthread_t cal_tid;
    pthread_create(&cal_tid, NULL, calibration_thread, NULL);

    /* If DISPLAY and WAYLAND_DISPLAY are unset (or running under sudo without display env), check headless */
    if (!headless && getenv("DISPLAY") == NULL && getenv("WAYLAND_DISPLAY") == NULL) {
        printf("[INFO] No DISPLAY/WAYLAND_DISPLAY in environment; running in --headless CLI mode.\n");
        headless = true;
    }

    if (headless) {
        printf("[CLI] Running in headless mode. Press Ctrl+C to stop.\n");
        while (atomic_load(&g_state.keep_running)) {
            usleep(250000);
        }
    } else {
        gui_run(argc, argv);
        atomic_store(&g_state.keep_running, false);
    }

    pthread_join(rt_tid, NULL);
    pthread_join(cal_tid, NULL);

    if (persist_profile) {
        char used[PATH_MAX + 32];
        if (core_autosave_profile(used, sizeof(used))) {
            printf("[PROFILE] Saved current state to %s\n", used);
        } else {
            fprintf(stderr, "[WARN] Could not save profile on exit\n");
        }
    }
    return 0;
}
