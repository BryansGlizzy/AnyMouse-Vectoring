#define _GNU_SOURCE
#include "core.h"
#include "kovaaks_offsets.h"
#include "faf_telemetry.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Read remote process memory via Linux process_vm_readv (zero-copy kernel transfer) */
static bool vm_read(pid_t pid, uint64_t remote_addr, void *local_buf, size_t len) {
    if (pid <= 0 || remote_addr < 0x10000ULL || remote_addr > 0x00007FFFFFFFFFFFULL) {
        return false;
    }
    struct iovec local_iov = { .iov_base = local_buf, .iov_len = len };
    struct iovec remote_iov = { .iov_base = (void *)(uintptr_t)remote_addr, .iov_len = len };
    ssize_t n = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
    return (n == (ssize_t)len);
}

static bool vm_read_u64(pid_t pid, uint64_t remote_addr, uint64_t *out_val) {
    return vm_read(pid, remote_addr, out_val, sizeof(uint64_t));
}

static bool vm_read_i32(pid_t pid, uint64_t remote_addr, int32_t *out_val) {
    return vm_read(pid, remote_addr, out_val, sizeof(int32_t));
}

static bool vm_read_f32(pid_t pid, uint64_t remote_addr, float *out_val) {
    return vm_read(pid, remote_addr, out_val, sizeof(float));
}

/* Scan /proc to locate running FPSAimTrainer-Win64-Shipping.exe under Wine/Proton */
static pid_t find_kovaaks_pid(uint64_t *out_image_base) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    pid_t found_pid = -1;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 1) continue;

        char cmdline_path[128];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
        int fd = open(cmdline_path, O_RDONLY);
        if (fd < 0) continue;

        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\0') buf[i] = ' ';
        }
        buf[n] = '\0';

        if (strstr(buf, "FPSAimTrainer-Win64-Shipping.exe") != NULL &&
            strstr(buf, "reaper") == NULL &&
            strstr(buf, "pressure-vessel") == NULL) {
            found_pid = pid;
            break;
        }
    }
    closedir(dir);

    if (found_pid > 0 && out_image_base) {
        *out_image_base = UE4_DEFAULT_IMAGE_BASE;
        char maps_path[128];
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", found_pid);
        FILE *mf = fopen(maps_path, "r");
        if (mf) {
            char line[512];
            while (fgets(line, sizeof(line), mf)) {
                if (strstr(line, "FPSAimTrainer-Win64-Shipping.exe")) {
                    unsigned long long start_addr = 0;
                    if (sscanf(line, "%llx-", &start_addr) == 1 && start_addr >= 0x10000ULL) {
                        *out_image_base = (uint64_t)start_addr;
                        break;
                    }
                }
            }
            fclose(mf);
        }
    }
    return found_pid;
}

static inline float wrap_deg_180(float deg) {
    while (deg > 180.0f)  deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float compute_median(const float *arr, int n) {
    if (n <= 0) return 1.0f;
    float tmp[MAX_OCTANT_TRIALS];
    int count = n < MAX_OCTANT_TRIALS ? n : MAX_OCTANT_TRIALS;
    memcpy(tmp, arr, (size_t)count * sizeof(float));
    qsort(tmp, (size_t)count, sizeof(float), cmp_float);
    if (count & 1) {
        return tmp[count / 2];
    }
    return 0.5f * (tmp[(count / 2) - 1] + tmp[count / 2]);
}

void calibration_reset_stats(void) {
    pthread_mutex_lock(&g_state.telemetry_mutex);
    for (int k = 0; k < 8; k++) {
        float w = atomic_load_explicit(&g_state.octant_weights[k], memory_order_relaxed);
        memset(&g_state.calib.octant[k], 0, sizeof(octant_stats_entry_t));
        g_state.calib.octant[k].median_ratio = 1.0f;
        g_state.calib.octant[k].last_ratio = 1.0f;
        g_state.calib.octant[k].suggested_weight = w;
    }
    g_state.calib.total_valid_flicks = 0;
    g_state.calib.last_landing_ratio = 1.0f;
    g_state.calib.last_skew_deg = 0.0f;
    g_state.calib.last_p1_x = 0.0f;
    g_state.calib.last_p1_y = 0.0f;
    g_state.calib.last_t_x = 0.0f;
    g_state.calib.last_t_y = 0.0f;
    pthread_mutex_unlock(&g_state.telemetry_mutex);
}

/* Inter-Round Damped Weight Adaptation (README Section 5.3) */
int calibration_apply_damped_update(bool require_min_trials) {
    int updated_count = 0;
    int req = require_min_trials ? atomic_load_explicit(&g_state.trials_per_round, memory_order_relaxed) : 1;
    if (req < 1) req = 1;

    pthread_mutex_lock(&g_state.telemetry_mutex);
    for (int k = 0; k < 8; k++) {
        octant_stats_entry_t *st = &g_state.calib.octant[k];
        if (st->trial_count >= req && st->suggested_weight >= 0.02f && st->suggested_weight <= 3.0f) {
            atomic_store_explicit(&g_state.octant_weights[k], st->suggested_weight, memory_order_relaxed);
            st->trial_count = 0; /* Clear current round trial buffer for next round */
            updated_count++;
        }
    }
    if (updated_count > 0) {
        g_state.calib.rounds_applied++;
    }
    pthread_mutex_unlock(&g_state.telemetry_mutex);
    return updated_count;
}

/*
 * Poll UE4.26 GWorld, AMetaGameState::Characters, AMetaPlayerController, and AMetaCharacter.
 * Determines exact session state:
 *   - KOVAAKS_STATE_OFFLINE
 *   - KOVAAKS_STATE_MENU_OR_LOBBY (no active bots / no player character)
 *   - KOVAAKS_STATE_PAUSED        (PauseMenuLocation != 0)
 *   - KOVAAKS_STATE_ACTIVE_ROUND  (Player character active + spawned bots + unpaused)
 */
static kovaaks_session_state_t poll_kovaaks_ue4_memory(pid_t pid, uint64_t image_base) {
    uint64_t uworld = 0;
    if (!vm_read_u64(pid, image_base + UE4_GWORLD_RVA, &uworld) || uworld < 0x10000ULL) {
        pthread_mutex_lock(&g_state.telemetry_mutex);
        g_state.calib.session_state = KOVAAKS_STATE_MENU_OR_LOBBY;
        g_state.calib.kovaaks_running = true;
        g_state.calib.kovaaks_pid = pid;
        g_state.calib.ue4_memory_attached = false;
        g_state.calib.uworld_ptr = 0;
        g_state.calib.active_bot_count = 0;
        pthread_mutex_unlock(&g_state.telemetry_mutex);
        return KOVAAKS_STATE_MENU_OR_LOBBY;
    }

    uint64_t game_inst = 0, local_players_data = 0, local_player = 0, player_ctrl = 0;
    uint64_t ack_pawn = 0, my_char = 0, cam_mgr = 0;
    int32_t  pause_loc = 0;
    ue_frotator_t ctrl_rot = {0};
    ue_minimal_view_info_t pov = {0};
    bool have_cam = false;

    if (vm_read_u64(pid, uworld + UE4_UWORLD_GAME_INSTANCE, &game_inst) && game_inst > 0x10000ULL) {
        if (vm_read_u64(pid, game_inst + UE4_UGAMEINSTANCE_LOCALPLAYERS, &local_players_data) && local_players_data > 0x10000ULL) {
            if (vm_read_u64(pid, local_players_data, &local_player) && local_player > 0x10000ULL) {
                if (vm_read_u64(pid, local_player + UE4_UPLAYER_PLAYERCONTROLLER, &player_ctrl) && player_ctrl > 0x10000ULL) {
                    vm_read(pid, player_ctrl + UE4_ACONTROLLER_CONTROL_ROT, &ctrl_rot, sizeof(ctrl_rot));
                    vm_read_u64(pid, player_ctrl + UE4_APC_ACKNOWLEDGED_PAWN, &ack_pawn);
                    vm_read_u64(pid, player_ctrl + KOV_PC_MY_CHARACTER, &my_char);
                    vm_read_i32(pid, player_ctrl + KOV_PC_PAUSE_MENU_LOCATION, &pause_loc);
                    if (vm_read_u64(pid, player_ctrl + UE4_APC_CAMERA_MANAGER, &cam_mgr) && cam_mgr > 0x10000ULL) {
                        if (vm_read(pid, cam_mgr + UE4_ACAMERA_CAMERACACHE + UE4_FCAMERACACHE_POV, &pov, sizeof(pov))) {
                            have_cam = true;
                        }
                    }
                }
            }
        }
    }

    uint64_t player_char = (my_char > 0x10000ULL) ? my_char : ack_pawn;

    /* Read live player round stats from AMetaCharacter */
    int32_t kill_count = 0, overshots = 0;
    float   damage_done = 0.0f, last_score = 0.0f, last_shot_time = 0.0f;
    int64_t challenge_start_ticks = 0;

    if (player_char > 0x10000ULL) {
        vm_read_i32(pid, player_char + KOV_CHAR_KILL_COUNT, &kill_count);
        vm_read_i32(pid, player_char + KOV_CHAR_TOTAL_OVERSHOTS, &overshots);
        vm_read_f32(pid, player_char + KOV_CHAR_DAMAGE_DONE, &damage_done);
        vm_read_f32(pid, player_char + KOV_CHAR_LAST_SCORE, &last_score);
        vm_read_f32(pid, player_char + KOV_CHAR_LAST_SHOT_FIRED_TIME, &last_shot_time);
        vm_read(pid, player_char + KOV_CHAR_CHALLENGE_START_TIME, &challenge_start_ticks, sizeof(int64_t));
    }

    /* Read AMetaGameState::Characters (UWorld + 0x120 -> + 0x2F0) to enumerate spawned Bots */
    uint64_t game_state = 0;
    int bot_count = 0;
    float best_ang_dist_sq = 1e9f;
    float best_yaw_delta = 0.0f;
    float best_pitch_delta = 0.0f;

    if (vm_read_u64(pid, uworld + UE4_UWORLD_GAMESTATE, &game_state) && game_state > 0x10000ULL) {
        ue_tarray_t chars_arr = {0};
        if (vm_read(pid, game_state + KOV_GS_CHARACTERS_TARRAY, &chars_arr, sizeof(chars_arr))) {
            int count = chars_arr.count;
            if (count > 0 && count <= 128 && chars_arr.data_ptr > 0x10000ULL) {
                uint64_t char_ptrs[128];
                if (vm_read(pid, chars_arr.data_ptr, char_ptrs, (size_t)count * sizeof(uint64_t))) {
                    for (int i = 0; i < count; i++) {
                        uint64_t cptr = char_ptrs[i];
                        if (cptr < 0x10000ULL || cptr == player_char) continue;

                        float bot_health = 0.0f;
                        vm_read_f32(pid, cptr + KOV_CHAR_HEALTH, &bot_health);
                        if (bot_health <= 0.0f) continue; /* Skip dead/respawning bots */

                        uint64_t root_comp = 0;
                        if (!vm_read_u64(pid, cptr + UE4_AACTOR_ROOT_COMPONENT, &root_comp) || root_comp < 0x10000ULL) {
                            continue;
                        }
                        ue_fvector_t loc = {0};
                        if (!vm_read(pid, root_comp + UE4_USCENE_RELATIVE_LOCATION, &loc, sizeof(loc))) {
                            continue;
                        }
                        bot_count++;

                        if (have_cam) {
                            float dx = loc.x - pov.location.x;
                            float dy = loc.y - pov.location.y;
                            float dz = loc.z - pov.location.z;
                            float target_yaw = atan2f(dy, dx) * (180.0f / PI);
                            float target_pitch = atan2f(dz, sqrtf(dx * dx + dy * dy)) * (180.0f / PI);
                            float d_yaw = wrap_deg_180(target_yaw - pov.rotation.yaw);
                            float d_pitch = wrap_deg_180(target_pitch - pov.rotation.pitch);
                            float d2 = d_yaw * d_yaw + d_pitch * d_pitch;
                            if (d2 < best_ang_dist_sq) {
                                best_ang_dist_sq = d2;
                                best_yaw_delta = d_yaw;
                                best_pitch_delta = d_pitch;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Determine strict KovaaK's Session State */
    kovaaks_session_state_t s_state = KOVAAKS_STATE_MENU_OR_LOBBY;
    if (player_char > 0x10000ULL && bot_count > 0) {
        if (pause_loc != 0) {
            s_state = KOVAAKS_STATE_PAUSED;
        } else {
            s_state = KOVAAKS_STATE_ACTIVE_ROUND;
        }
    }

    pthread_mutex_lock(&g_state.telemetry_mutex);

    /* Detect Round Start / Challenge Restart transition */
    bool new_round_started = false;
    if (s_state == KOVAAKS_STATE_ACTIVE_ROUND) {
        if (g_state.calib.active_round_number == 0) {
            g_state.calib.active_round_number = 1;
        } else if ((challenge_start_ticks > 0 &&
                    g_state.calib.live_challenge_start_ticks > 0 &&
                    challenge_start_ticks != g_state.calib.live_challenge_start_ticks) ||
                   (g_state.calib.live_kill_count > 2 && kill_count == 0)) {
            new_round_started = true;
            g_state.calib.active_round_number++;
        }
    }

    g_state.calib.session_state = s_state;
    g_state.calib.kovaaks_running = true;
    g_state.calib.kovaaks_pid = pid;
    g_state.calib.ue4_memory_attached = (player_ctrl > 0x10000ULL);
    g_state.calib.uworld_ptr = uworld;
    g_state.calib.cam_yaw = have_cam ? pov.rotation.yaw : ctrl_rot.yaw;
    g_state.calib.cam_pitch = have_cam ? pov.rotation.pitch : ctrl_rot.pitch;
    g_state.calib.cam_fov = have_cam ? pov.fov : 103.0f;
    g_state.calib.active_bot_count = bot_count;
    if (best_ang_dist_sq < 1e8f) {
        g_state.calib.nearest_bot_yaw_delta = best_yaw_delta;
        g_state.calib.nearest_bot_pitch_delta = best_pitch_delta;
    }

    g_state.calib.live_kill_count = kill_count;
    g_state.calib.live_overshots = overshots;
    g_state.calib.live_damage_done = damage_done;
    g_state.calib.live_score = last_score;
    g_state.calib.live_last_shot_time = last_shot_time;
    if (challenge_start_ticks > 0) {
        g_state.calib.live_challenge_start_ticks = challenge_start_ticks;
    }
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    /* If a new Challenge Round started and auto_calibrate is ON, apply inter-round adaptation from previous round */
    if (new_round_started && atomic_load_explicit(&g_state.auto_calibrate, memory_order_relaxed)) {
        calibration_apply_damped_update(true);
    }

    return s_state;
}

/* Parse a KovaaK's stats CSV file written at the end of a Challenge round */
static void parse_kovaaks_csv_file(const char *filepath, bool is_live_round_end) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    int kills = 0;
    int total_shots = 0;
    int total_hits = 0;
    int total_overshots = 0;
    float sum_ttk = 0.0f;
    float horiz_sens = 0.0f;
    char sens_scale[32] = {0};
    char scenario_name[96] = {0};

    const char *base = strrchr(filepath, '/');
    base = base ? (base + 1) : filepath;
    const char *sep = strstr(base, " - Challenge");
    if (!sep) sep = strstr(base, " Stats.csv");
    size_t nlen = sep ? (size_t)(sep - base) : strlen(base);
    if (nlen >= sizeof(scenario_name)) nlen = sizeof(scenario_name) - 1;
    memcpy(scenario_name, base, nlen);
    scenario_name[nlen] = '\0';

    char line[512];
    bool in_kill_table = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Kill #,", 7) == 0) {
            in_kill_table = true;
            continue;
        }
        if (in_kill_table) {
            if (line[0] == '\r' || line[0] == '\n' || !isdigit((unsigned char)line[0])) {
                in_kill_table = false;
            } else {
                int kill_num = 0, shots = 0, hits = 0, cheated = 0, overshots = 0;
                char ts[32], bot[64], weap[64];
                float ttk = 0.0f, acc = 0.0f, dmg_done = 0.0f, dmg_poss = 0.0f, eff = 0.0f;
                int matched = sscanf(line, "%d,%31[^,],%63[^,],%63[^,],%fs,%d,%d,%f,%f,%f,%f,%d,%d",
                                     &kill_num, ts, bot, weap, &ttk, &shots, &hits, &acc,
                                     &dmg_done, &dmg_poss, &eff, &cheated, &overshots);
                if (matched < 5) {
                    matched = sscanf(line, "%d,%31[^,],%63[^,],%63[^,],%f,%d,%d,%f,%f,%f,%f,%d,%d",
                                     &kill_num, ts, bot, weap, &ttk, &shots, &hits, &acc,
                                     &dmg_done, &dmg_poss, &eff, &cheated, &overshots);
                }
                if (matched >= 7) {
                    kills++;
                    sum_ttk += ttk;
                    total_shots += shots;
                    total_hits += hits;
                    if (matched >= 13) total_overshots += overshots;
                }
                continue;
            }
        }

        if (strncmp(line, "Sens Scale:,", 12) == 0) {
            char *val = line + 12;
            size_t l = strcspn(val, "\r\n,");
            if (l >= sizeof(sens_scale)) l = sizeof(sens_scale) - 1;
            memcpy(sens_scale, val, l);
            sens_scale[l] = '\0';
        } else if (strncmp(line, "Horiz Sens:,", 12) == 0) {
            horiz_sens = strtof(line + 12, NULL);
        } else if (strncmp(line, "Scenario:,", 10) == 0) {
            char *val = line + 10;
            size_t l = strcspn(val, "\r\n,");
            if (l > 0 && l < sizeof(scenario_name)) {
                memcpy(scenario_name, val, l);
                scenario_name[l] = '\0';
            }
        }
    }
    fclose(f);

    pthread_mutex_lock(&g_state.telemetry_mutex);
    snprintf(g_state.calib.last_scenario, sizeof(g_state.calib.last_scenario), "%s", scenario_name);
    g_state.calib.last_kills = kills;
    g_state.calib.last_accuracy = (total_shots > 0) ? ((float)total_hits * 100.0f / (float)total_shots) : 0.0f;
    g_state.calib.last_avg_ttk = (kills > 0) ? (sum_ttk / (float)kills) : 0.0f;
    g_state.calib.last_overshots = total_overshots;
    if (horiz_sens > 0.0f) g_state.calib.csv_horiz_sens = horiz_sens;
    if (sens_scale[0] != '\0') {
        snprintf(g_state.calib.csv_sens_scale, sizeof(g_state.calib.csv_sens_scale), "%s", sens_scale);
    }
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    /* When a Challenge Round completes and writes Stats.csv, trigger inter-round adaptation if enabled */
    if (is_live_round_end && atomic_load_explicit(&g_state.auto_calibrate, memory_order_relaxed)) {
        calibration_apply_damped_update(true);
    }
}

static void load_latest_kovaaks_csv(void) {
    DIR *dir = opendir(KOVAAKS_STATS_DIR);
    if (!dir) return;

    char latest_path[512] = {0};
    time_t latest_mtime = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcmp(ent->d_name + len - 4, ".csv") != 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", KOVAAKS_STATS_DIR, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && st.st_mtime >= latest_mtime) {
            latest_mtime = st.st_mtime;
            snprintf(latest_path, sizeof(latest_path), "%s", full);
        }
    }
    closedir(dir);

    if (latest_path[0] != '\0') {
        parse_kovaaks_csv_file(latest_path, false);
    }
}

/* Record a validated in-scenario ballistic flick (P_1 vs T) into octant k */
static void record_ballistic_flick(uint64_t ts_ns, float p1_x, float p1_y,
                                   float t_x, float t_y, float v_peak) {
    float t_mag_sq = t_x * t_x + t_y * t_y;
    float t_mag = sqrtf(t_mag_sq);
    if (t_mag < 35.0f) return; /* Ignore tiny micro-movements (< 35 counts) */

    /* Radial landing ratio r_i = (P_1 . T) / ||T||^2 (Cartesian Y-up) */
    float dot = p1_x * t_x + p1_y * t_y;
    float r_i = dot / t_mag_sq;
    if (r_i < 0.35f || r_i > 2.20f) return; /* Reject non-ballistic camera resets */

    float theta_t = atan2f(t_y, t_x);
    if (theta_t < 0.0f) theta_t += TWO_PI;

    float theta_p1 = atan2f(p1_y, p1_x);
    if (theta_p1 < 0.0f) theta_p1 += TWO_PI;

    float skew_deg = wrap_deg_180((theta_p1 - theta_t) * (180.0f / PI));
    if (fabsf(skew_deg) > 35.0f) return;

    /* Map target direction to nearest octant k in {0..7} */
    int k = ((int)(theta_t * INV_OCTANT_STEP + 0.5f)) & 7;

    pthread_mutex_lock(&g_state.telemetry_mutex);
    octant_stats_entry_t *st = &g_state.calib.octant[k];

    int slot = st->trial_count < MAX_OCTANT_TRIALS ? st->trial_count : (st->total_flicks % MAX_OCTANT_TRIALS);
    st->ratios[slot] = r_i;
    st->skews_deg[slot] = skew_deg;
    if (st->trial_count < MAX_OCTANT_TRIALS) {
        st->trial_count++;
    }
    st->total_flicks++;
    st->last_ratio = r_i;
    st->median_ratio = compute_median(st->ratios, st->trial_count);

    float sum_skew = 0.0f;
    for (int i = 0; i < st->trial_count; i++) sum_skew += st->skews_deg[i];
    st->mean_skew_deg = sum_skew / (float)st->trial_count;

    /* Compute damped weight update recommendation for inter-round application:
     * M_k^(new) = M_k^(old) * (1 + alpha * (1 / r_tilde_k - 1)), clamped to +/- 5% */
    float cur_w = atomic_load_explicit(&g_state.octant_weights[k], memory_order_relaxed);
    float raw_new = cur_w * (1.0f + CALIB_LEARNING_RATE_ALPHA * ((1.0f / st->median_ratio) - 1.0f));
    float max_delta = CALIB_MAX_STEP_CLAMP * cur_w;
    float delta = raw_new - cur_w;
    if (delta > max_delta)  delta = max_delta;
    if (delta < -max_delta) delta = -max_delta;
    st->suggested_weight = cur_w + delta;

    g_state.calib.last_flick_time_ns = ts_ns;
    g_state.calib.last_flick_octant  = k;
    g_state.calib.last_p1_x          = p1_x;
    g_state.calib.last_p1_y          = p1_y;
    g_state.calib.last_t_x           = t_x;
    g_state.calib.last_t_y           = t_y;
    g_state.calib.last_v_peak        = v_peak;
    g_state.calib.last_landing_ratio = r_i;
    g_state.calib.last_skew_deg      = skew_deg;
    g_state.calib.total_valid_flicks++;
    pthread_mutex_unlock(&g_state.telemetry_mutex);
}

/* Record a verified 3D ballistic submovement event directly from FpsAimForge plugin */
static void record_faf_submovement(const faf_submovement_event_t *ev) {
    if (!ev) return;
    int k = ev->octant & 7;
    float r_i = ev->radial_ratio;
    float skew_deg = ev->angular_error_deg;

    if (r_i < 0.35f || r_i > 2.20f) return;
    if (fabsf(skew_deg) > 40.0f) return;

    float rad_t  = ev->target_angle_deg * (PI / 180.0f);
    float rad_p1 = (ev->target_angle_deg + skew_deg) * (PI / 180.0f);
    float t_x    = ev->target_dist_deg * cosf(rad_t);
    float t_y    = ev->target_dist_deg * sinf(rad_t);
    float p1_mag = ev->target_dist_deg * r_i;
    float p1_x   = p1_mag * cosf(rad_p1);
    float p1_y   = p1_mag * sinf(rad_p1);

    pthread_mutex_lock(&g_state.telemetry_mutex);
    octant_stats_entry_t *st = &g_state.calib.octant[k];

    int slot = st->trial_count < MAX_OCTANT_TRIALS ? st->trial_count : (st->total_flicks % MAX_OCTANT_TRIALS);
    st->ratios[slot] = r_i;
    st->skews_deg[slot] = skew_deg;
    if (st->trial_count < MAX_OCTANT_TRIALS) {
        st->trial_count++;
    }
    st->total_flicks++;
    st->last_ratio = r_i;
    st->median_ratio = compute_median(st->ratios, st->trial_count);

    float sum_skew = 0.0f;
    for (int i = 0; i < st->trial_count; i++) sum_skew += st->skews_deg[i];
    st->mean_skew_deg = sum_skew / (float)st->trial_count;

    /* Damped weight update recommendation:
     * M_k^(new) = M_k^(old) * (1 + alpha * (1 / r_tilde_k - 1)), clamped to +/- 5% */
    float cur_w = atomic_load_explicit(&g_state.octant_weights[k], memory_order_relaxed);
    float raw_new = cur_w * (1.0f + CALIB_LEARNING_RATE_ALPHA * ((1.0f / st->median_ratio) - 1.0f));
    float max_delta = CALIB_MAX_STEP_CLAMP * cur_w;
    float delta = raw_new - cur_w;
    if (delta > max_delta)  delta = max_delta;
    if (delta < -max_delta) delta = -max_delta;
    st->suggested_weight = cur_w + delta;

    g_state.calib.last_flick_time_ns = ev->timestamp_ns;
    g_state.calib.last_flick_octant  = k;
    g_state.calib.last_p1_x          = p1_x;
    g_state.calib.last_p1_y          = p1_y;
    g_state.calib.last_t_x           = t_x;
    g_state.calib.last_t_y           = t_y;
    g_state.calib.last_v_peak        = ev->peak_vel_deg_s;
    g_state.calib.last_landing_ratio = r_i;
    g_state.calib.last_skew_deg      = skew_deg;
    g_state.calib.total_valid_flicks++;
    pthread_mutex_unlock(&g_state.telemetry_mutex);
}

static anymouse_faf_shm_t *open_faf_shm(void) {
    /* Try opening existing shm WITHOUT O_CREAT first so Linux fs.protected_regular=1
     * never blocks root from opening a /dev/shm file owned by the unprivileged user. */
    int fd = shm_open(ANYMOUSE_FAF_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        fd = shm_open(ANYMOUSE_FAF_SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return NULL;
        (void)fchmod(fd, 0666);
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(anymouse_faf_shm_t)) {
        if (ftruncate(fd, sizeof(anymouse_faf_shm_t)) < 0) {
            close(fd);
            return NULL;
        }
    }

    anymouse_faf_shm_t *shm = (anymouse_faf_shm_t *)mmap(
        NULL, sizeof(anymouse_faf_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) return NULL;
    if (shm->magic != ANYMOUSE_FAF_MAGIC || shm->version != ANYMOUSE_FAF_VERSION) {
        memset(shm, 0, sizeof(*shm));
        shm->magic   = ANYMOUSE_FAF_MAGIC;
        shm->version = ANYMOUSE_FAF_VERSION;
    }
    return shm;
}

static bool poll_faf_telemetry(anymouse_faf_shm_t *shm, uint32_t *prev_rounds_completed) {
    if (!shm || shm->magic != ANYMOUSE_FAF_MAGIC) return false;

    int32_t faf_pid = atomic_load_explicit(&shm->faf_pid, memory_order_acquire);
    bool alive = (faf_pid > 1 && (kill((pid_t)faf_pid, 0) == 0 || errno == EPERM));

    if (!alive) {
        pthread_mutex_lock(&g_state.telemetry_mutex);
        g_state.calib.faf_connected = false;
        g_state.calib.faf_pid = -1;
        g_state.calib.faf_run_state = FAF_STATE_NOT_STARTED;
        g_state.calib.session_state = KOVAAKS_STATE_OFFLINE;
        pthread_mutex_unlock(&g_state.telemetry_mutex);
        return false;
    }

    int32_t  rstate      = atomic_load_explicit(&shm->run_state, memory_order_acquire);
    uint32_t round_id    = atomic_load_explicit(&shm->round_id, memory_order_relaxed);
    uint32_t completed   = atomic_load_explicit(&shm->rounds_completed, memory_order_relaxed);
    uint32_t aborted     = atomic_load_explicit(&shm->rounds_aborted, memory_order_relaxed);

    /* Consume any new verified submovement events from the lock-free ring buffer */
    uint32_t w = atomic_load_explicit(&shm->write_idx, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&shm->read_idx, memory_order_relaxed);
    if (w - r > FAF_EVENT_RING_SIZE) {
        r = w - FAF_EVENT_RING_SIZE;
    }
    while (r < w) {
        faf_submovement_event_t ev = shm->events[r & FAF_EVENT_RING_MASK];
        r++;
        record_faf_submovement(&ev);
    }
    atomic_store_explicit(&shm->read_idx, r, memory_order_release);

    kovaaks_session_state_t mapped_state = KOVAAKS_STATE_MENU_OR_LOBBY;
    if (rstate == FAF_STATE_RUNNING) {
        mapped_state = KOVAAKS_STATE_ACTIVE_ROUND;
    } else if (rstate == FAF_STATE_WAITING_FOR_CLICK_TO_START ||
               rstate == FAF_STATE_START_COUNTDOWN) {
        mapped_state = KOVAAKS_STATE_PAUSED;
    }

    pthread_mutex_lock(&g_state.telemetry_mutex);
    g_state.calib.faf_connected        = true;
    g_state.calib.faf_pid              = (pid_t)faf_pid;
    g_state.calib.faf_run_state        = rstate;
    g_state.calib.faf_round_id         = round_id;
    g_state.calib.faf_rounds_completed = completed;
    g_state.calib.faf_rounds_aborted   = aborted;
    snprintf(g_state.calib.faf_scenario, sizeof(g_state.calib.faf_scenario), "%s", shm->scenario_name);
    g_state.calib.faf_cm_per_360       = shm->cm_per_360;
    g_state.calib.faf_elapsed_sec      = shm->elapsed_seconds;
    g_state.calib.faf_duration_sec     = shm->duration_seconds;
    g_state.calib.faf_shots            = (int)shm->num_shots;
    g_state.calib.faf_hits             = (int)shm->num_hits;
    g_state.calib.faf_kills            = (int)shm->num_kills;

    /* Mirror into unified session state fields for GUI display */
    g_state.calib.session_state        = mapped_state;
    g_state.calib.kovaaks_running      = true;
    g_state.calib.ue4_memory_attached  = true;
    g_state.calib.kovaaks_pid          = (pid_t)faf_pid;
    g_state.calib.cam_yaw              = shm->cam_yaw_rad * (180.0f / PI);
    g_state.calib.cam_pitch            = shm->cam_pitch_rad * (180.0f / PI);
    g_state.calib.active_bot_count     = (int)shm->active_target_count;
    g_state.calib.live_kill_count      = (int)shm->num_kills;
    g_state.calib.active_round_number  = (int)round_id;
    if (shm->scenario_name[0] != '\0') {
        snprintf(g_state.calib.last_scenario, sizeof(g_state.calib.last_scenario), "%s", shm->scenario_name);
    }
    g_state.calib.last_kills           = (int)shm->num_kills;
    g_state.calib.last_accuracy        = (shm->num_shots > 0.0)
                                             ? (float)(shm->num_hits * 100.0 / shm->num_shots)
                                             : 0.0f;
    if (shm->active_target_count > 0) {
        g_state.calib.nearest_bot_yaw_delta   = (shm->active_targets[0].yaw_rad - shm->cam_yaw_rad) * (180.0f / PI);
        g_state.calib.nearest_bot_pitch_delta = (shm->active_targets[0].pitch_rad - shm->cam_pitch_rad) * (180.0f / PI);
    }
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    /* Trigger Inter-Round Damped Adaptation when an FpsAimForge round completes! */
    if (*prev_rounds_completed != 0xFFFFFFFFU && completed > *prev_rounds_completed) {
        if (atomic_load_explicit(&g_state.auto_calibrate, memory_order_relaxed)) {
            calibration_apply_damped_update(true);
        }
    }
    *prev_rounds_completed = completed;

    return true;
}

/* 5ms moving-average velocity window entry */
typedef struct {
    uint64_t ts_ns;
    float    dist;
} vel_sample_t;

#define VEL_WIN_MAX 128

void *calibration_thread(void *arg) {
    (void)arg;

    calibration_reset_stats();

    /* Open FpsAimForge (FAF) POSIX shared memory telemetry channel */
    anymouse_faf_shm_t *faf_shm = open_faf_shm();
    uint32_t prev_faf_rounds = 0xFFFFFFFFU;

    /* Sunset KovaaK's backend on Linux unless ANYMOUSE_ENABLE_KOVAAKS=1 is explicitly set */
    const char *enable_kovaaks_env = getenv("ANYMOUSE_ENABLE_KOVAAKS");
    bool kovaaks_enabled = (enable_kovaaks_env && enable_kovaaks_env[0] == '1');

    int inotify_fd = -1;
    int inotify_wd = -1;
    if (kovaaks_enabled) {
        load_latest_kovaaks_csv();
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd >= 0) {
            inotify_wd = inotify_add_watch(inotify_fd, KOVAAKS_STATS_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
        }
    }

    vel_sample_t vel_win[VEL_WIN_MAX];
    int vel_win_count = 0;

    enum {
        FLICK_IDLE = 0,
        FLICK_ACCELERATING,
        FLICK_CORRECTING
    } state = FLICK_IDLE;

    uint64_t flick_start_ns = 0;
    uint64_t p1_capture_ns  = 0;
    float    cur_x = 0.0f, cur_y = 0.0f;
    float    p1_x  = 0.0f, p1_y  = 0.0f;
    float    v_peak = 0.0f;
    uint8_t  prev_buttons = 0;

    uint64_t last_proc_scan_ns = 0;
    uint64_t last_ue4_poll_ns  = 0;
    pid_t    kovaaks_pid = -1;
    uint64_t kovaaks_base = UE4_DEFAULT_IMAGE_BASE;
    kovaaks_session_state_t session_state = KOVAAKS_STATE_OFFLINE;

    while (atomic_load_explicit(&g_state.keep_running, memory_order_relaxed)) {
        bool did_work = false;

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_now);
        uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + (uint64_t)ts_now.tv_nsec;

        /* 1. Primary Phase 2 Telemetry: Poll FpsAimForge (FAF) Native Shared Memory Plugin */
        if (!faf_shm) {
            faf_shm = open_faf_shm();
        }
        bool faf_active = poll_faf_telemetry(faf_shm, &prev_faf_rounds);

        /* 2. Sunset Fallback: Poll KovaaK's only if FAF is offline and ANYMOUSE_ENABLE_KOVAAKS=1 */
        if (!faf_active && kovaaks_enabled) {
            if (kovaaks_pid <= 0) {
                session_state = KOVAAKS_STATE_OFFLINE;
                if (now_ns - last_proc_scan_ns > 1500000000ULL) {
                    last_proc_scan_ns = now_ns;
                    kovaaks_pid = find_kovaaks_pid(&kovaaks_base);
                    if (kovaaks_pid <= 0) {
                        pthread_mutex_lock(&g_state.telemetry_mutex);
                        g_state.calib.session_state = KOVAAKS_STATE_OFFLINE;
                        g_state.calib.kovaaks_running = false;
                        g_state.calib.ue4_memory_attached = false;
                        g_state.calib.kovaaks_pid = -1;
                        g_state.calib.active_bot_count = 0;
                        pthread_mutex_unlock(&g_state.telemetry_mutex);
                    }
                }
            } else if (now_ns - last_ue4_poll_ns > 8000000ULL) {
                last_ue4_poll_ns = now_ns;
                char proc_dir[64];
                snprintf(proc_dir, sizeof(proc_dir), "/proc/%d", kovaaks_pid);
                if (access(proc_dir, F_OK) != 0) {
                    kovaaks_pid = -1;
                    session_state = KOVAAKS_STATE_OFFLINE;
                } else {
                    session_state = poll_kovaaks_ue4_memory(kovaaks_pid, kovaaks_base);
                }
            }
        } else if (!faf_active) {
            session_state = KOVAAKS_STATE_OFFLINE;
        }

        /* 3. STRICT SESSION GATING:
         * When using FpsAimForge (faf_active == true), submovements are extracted directly inside
         * FpsAimForge's 3D Camera + TargetManager by libanymouse_faf.so.
         * Drain tick_ring when FAF is active or when KovaaK's is not in KOVAAKS_STATE_ACTIVE_ROUND. */
        uint32_t tail = atomic_load_explicit(&g_state.ring_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&g_state.ring_head, memory_order_acquire);

        if (faf_active || !kovaaks_enabled || session_state != KOVAAKS_STATE_ACTIVE_ROUND) {
            if (tail != head) {
                atomic_store_explicit(&g_state.ring_tail, head, memory_order_release);
            }
            state = FLICK_IDLE;
            vel_win_count = 0;
        } else {
            while (tail != head) {
                did_work = true;
                tick_event_t ev = g_state.tick_ring[tail & TICK_RING_MASK];
                tail++;

                float dx = (float)ev.scaled_dx;
                float dy = (float)(-ev.scaled_dy);
                float step_dist = sqrtf(dx * dx + dy * dy);

                if (vel_win_count == VEL_WIN_MAX) {
                    memmove(&vel_win[0], &vel_win[1], (VEL_WIN_MAX - 1) * sizeof(vel_sample_t));
                    vel_win_count--;
                }
                vel_win[vel_win_count++] = (vel_sample_t){ .ts_ns = ev.timestamp_ns, .dist = step_dist };

                uint64_t cutoff_ns = (ev.timestamp_ns > 5000000ULL) ? (ev.timestamp_ns - 5000000ULL) : 0;
                int first_valid = 0;
                while (first_valid < vel_win_count && vel_win[first_valid].ts_ns < cutoff_ns) {
                    first_valid++;
                }
                if (first_valid > 0) {
                    vel_win_count -= first_valid;
                    memmove(&vel_win[0], &vel_win[first_valid], (size_t)vel_win_count * sizeof(vel_sample_t));
                }

                float win_dist = 0.0f;
                for (int i = 0; i < vel_win_count; i++) win_dist += vel_win[i].dist;
                float v_smooth = win_dist / 5.0f;

                bool left_click_edge = ((ev.buttons & 0x01) != 0) && ((prev_buttons & 0x01) == 0);
                prev_buttons = ev.buttons;

                switch (state) {
                    case FLICK_IDLE:
                        if (v_smooth > 0.45f) {
                            state = FLICK_ACCELERATING;
                            flick_start_ns = ev.timestamp_ns;
                            cur_x = dx;
                            cur_y = dy;
                            p1_x = cur_x;
                            p1_y = cur_y;
                            v_peak = v_smooth;
                        }
                        break;

                    case FLICK_ACCELERATING:
                        cur_x += dx;
                        cur_y += dy;
                        if (v_smooth > v_peak) {
                            v_peak = v_smooth;
                        }
                        if (v_peak >= 1.20f && v_smooth < (SUBMOVEMENT_VEL_RATIO * v_peak)) {
                            p1_x = cur_x;
                            p1_y = cur_y;
                            p1_capture_ns = ev.timestamp_ns;
                            state = FLICK_CORRECTING;
                        } else if (left_click_edge && v_peak >= 1.20f) {
                            p1_x = cur_x;
                            p1_y = cur_y;
                            record_ballistic_flick(ev.timestamp_ns, p1_x, p1_y, cur_x, cur_y, v_peak);
                            state = FLICK_IDLE;
                        } else if ((ev.timestamp_ns - flick_start_ns) > 600000000ULL) {
                            state = FLICK_IDLE;
                        }
                        break;

                    case FLICK_CORRECTING:
                        cur_x += dx;
                        cur_y += dy;
                        if (left_click_edge) {
                            /* Shot fired in active KovaaK's scenario after primary submovement P_1 */
                            record_ballistic_flick(ev.timestamp_ns, p1_x, p1_y, cur_x, cur_y, v_peak);
                            state = FLICK_IDLE;
                        } else if ((ev.timestamp_ns - p1_capture_ns) > 380000000ULL) {
                            state = FLICK_IDLE;
                        }
                        break;
                }
            }
            atomic_store_explicit(&g_state.ring_tail, tail, memory_order_release);
        }

        /* 3. Check inotify for completed KovaaK's Challenge round CSV files */
        if (inotify_fd >= 0 && inotify_wd >= 0) {
            char ibuf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t n = read(inotify_fd, ibuf, sizeof(ibuf));
            if (n > 0) {
                ssize_t offset = 0;
                while (offset < n) {
                    struct inotify_event *iev = (struct inotify_event *)&ibuf[offset];
                    if (iev->len > 0 && strstr(iev->name, ".csv") != NULL) {
                        char csv_path[512];
                        snprintf(csv_path, sizeof(csv_path), "%s/%s", KOVAAKS_STATS_DIR, iev->name);
                        parse_kovaaks_csv_file(csv_path, true);
                    }
                    offset += (ssize_t)sizeof(struct inotify_event) + iev->len;
                }
            }
        }

        if (!did_work) {
            usleep(2000);
        }
    }

    if (inotify_fd >= 0) {
        close(inotify_fd);
    }
    return NULL;
}
