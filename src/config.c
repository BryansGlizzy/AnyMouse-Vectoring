#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The engine usually runs as root via sudo, but everything it writes for the
 * user (config, FpsAimForge bundle, game-directory files) must belong to the
 * invoking user, so Steam and the user can still update or delete it.
 */

static bool sudo_ids(uid_t *uid, gid_t *gid) {
    if (geteuid() != 0) return false;
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
    if (!su || !sg) return false;
    *uid = (uid_t)strtoul(su, NULL, 10);
    *gid = (gid_t)strtoul(sg, NULL, 10);
    return true;
}

bool config_user_home(char *out, size_t out_sz) {
    uid_t uid;
    gid_t gid;
    if (sudo_ids(&uid, &gid)) {
        struct passwd *pw = getpwuid(uid);
        if (pw && pw->pw_dir) {
            snprintf(out, out_sz, "%s", pw->pw_dir);
            return true;
        }
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, out_sz, "%s", home);
        return true;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        snprintf(out, out_sz, "%s", pw->pw_dir);
        return true;
    }
    return false;
}

bool config_exe_dir(char *out, size_t out_sz) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash) *slash = '\0';
    snprintf(out, out_sz, "%s", exe);
    return true;
}

void config_chown_to_user(const char *path) {
    uid_t uid;
    gid_t gid;
    if (sudo_ids(&uid, &gid) && chown(path, uid, gid) != 0) {
        fprintf(stderr, "[WARN] chown(%s): %s\n", path, strerror(errno));
    }
}

/* mkdir -p, handing every directory it creates to the invoking user */
bool config_mkdirs_for_user(const char *dir) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (mkdir(tmp, 0755) == 0) {
                config_chown_to_user(tmp);
            } else if (errno != EEXIST) {
                return false;
            }
            *p = saved;
            if (saved == '\0') break;
        }
    }
    return true;
}

static bool config_dir(char *dir, size_t dir_sz, bool create_dir) {
    uid_t uid;
    gid_t gid;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (!sudo_ids(&uid, &gid) && xdg && xdg[0] == '/') {
        snprintf(dir, dir_sz, "%s/anymouse", xdg);
    } else {
        char home[PATH_MAX];
        if (!config_user_home(home, sizeof(home))) return false;
        snprintf(dir, dir_sz, "%s/.config/anymouse", home);
    }
    return !create_dir || config_mkdirs_for_user(dir);
}

static bool config_path(char *out, size_t out_sz, bool create_dir) {
    char dir[PATH_MAX + 32];
    if (!config_dir(dir, sizeof(dir), create_dir)) return false;
    snprintf(out, out_sz, "%s/config", dir);
    return true;
}

/* Where the octant profile lives: <config dir>/profile.json (owned by the invoking user) */
bool config_profile_path(char *out, size_t out_sz, bool create_dir) {
    char dir[PATH_MAX + 32];
    if (!config_dir(dir, sizeof(dir), create_dir)) return false;
    snprintf(out, out_sz, "%s/profile.json", dir);
    return true;
}

/* Restore the saved device preference ("device=auto" or a device path) */
void config_load(void) {
    char path[PATH_MAX + 64];
    if (!config_path(path, sizeof(path), false)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "device=", 7) == 0) {
            const char *v = line + 7;
            pthread_mutex_lock(&g_state.device_mutex);
            snprintf(g_state.requested_path, sizeof(g_state.requested_path), "%s",
                     strcmp(v, "auto") == 0 ? "" : v);
            pthread_mutex_unlock(&g_state.device_mutex);
        }
    }
    fclose(f);
}

bool config_save(void) {
    char path[PATH_MAX + 64];
    if (!config_path(path, sizeof(path), true)) return false;
    char req[256];
    devices_get_request(req, sizeof(req));

    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# AnyMouse-Vectoring user config\n");
    fprintf(f, "device=%s\n", req[0] ? req : "auto");
    fclose(f);
    config_chown_to_user(path);
    return true;
}
