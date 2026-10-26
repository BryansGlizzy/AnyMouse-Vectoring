#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * FpsAimForge integration installer.
 *
 * Layout inside the game directory once installed:
 *   FpsAimForge             <- AnyMouse launcher (faf_launcher), injects the plugin
 *   FpsAimForge.bin         <- the real game binary
 *   libanymouse_faf.so      <- telemetry plugin
 * and the scenario bundle in <FpsAimForge data dir>/bundles/AnyMouse.bundle.json.
 *
 * When Steam updates the game it rewrites FpsAimForge with the real binary,
 * which detection reports as FAF_INSTALL_NEEDS_REPAIR.
 */

#define FAF_STEAM_APPID      "5165380"
#define FAF_EXE_NAME         "FpsAimForge"
#define FAF_LAUNCHER_MARKER  "ANYMOUSE_FAF_LAUNCHER"
#define FAF_PLUGIN_NAME      "libanymouse_faf.so"
#define FAF_BUNDLE_NAME      "AnyMouse.bundle.json"

extern char **environ;

static bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Read a whole (small) file into a malloc'd buffer */
static uint8_t *read_file(const char *path, size_t max, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || (size_t)st.st_size > max) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = 0;
    *out_len = n;
    return buf;
}

static bool files_identical(const char *a, const char *b) {
    size_t la = 0, lb = 0;
    uint8_t *da = read_file(a, 64u << 20, &la);
    uint8_t *db = read_file(b, 64u << 20, &lb);
    bool same = da && db && la == lb && memcmp(da, db, la) == 0;
    free(da);
    free(db);
    return same;
}

/* Our launcher is ~16 KB and carries a marker string; the real game is ~14 MB.
 * Launchers built before the marker existed are recognised by their plugin/exec strings. */
static bool is_anymouse_launcher(const char *path) {
    size_t len = 0;
    uint8_t *d = read_file(path, 1u << 20, &len);
    bool hit = d && (memmem(d, len, FAF_LAUNCHER_MARKER, strlen(FAF_LAUNCHER_MARKER)) != NULL ||
                     (memmem(d, len, FAF_PLUGIN_NAME, strlen(FAF_PLUGIN_NAME)) != NULL &&
                      memmem(d, len, "execv FpsAimForge.bin", 21) != NULL));
    free(d);
    return hit;
}

static off_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_size : -1;
}

/* Copy src -> dst atomically (temp file + rename) so a running game never maps a half-written file */
static bool install_file(const char *src, const char *dst, mode_t mode) {
    size_t len = 0;
    uint8_t *data = read_file(src, 64u << 20, &len);
    if (!data) return false;

    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.anymouse-tmp", dst);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    bool ok = fd >= 0 && write(fd, data, len) == (ssize_t)len;
    if (fd >= 0) {
        if (fchmod(fd, mode) != 0) ok = false;
        if (close(fd) != 0) ok = false;
    }
    free(data);
    if (ok && rename(tmp, dst) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    config_chown_to_user(dst);
    return true;
}

/* Extract the value of the first `"key" "value"` pair on a VDF line */
static bool vdf_line_value(const char *line, const char *key, char *out, size_t out_sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '"');
    if (!p) return false;
    p++;
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < out_sz; p++) {
        if (*p == '\\' && p[1]) p++; /* VDF escapes backslashes */
        out[o++] = *p;
    }
    out[o] = '\0';
    return o > 0;
}

typedef struct {
    char path[PATH_MAX];
    bool flatpak;
} steam_root_t;

static int find_steam_roots(steam_root_t *roots, int max) {
    char home[PATH_MAX];
    if (!config_user_home(home, sizeof(home))) return 0;

    const struct { const char *rel; bool flatpak; } candidates[] = {
        { ".local/share/Steam", false },
        { ".steam/steam", false },
        { ".steam/root", false },
        { ".var/app/com.valvesoftware.Steam/.local/share/Steam", true },
        { ".var/app/com.valvesoftware.Steam/data/Steam", true },
        { "snap/steam/common/.local/share/Steam", false },
    };

    int n = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && n < max; i++) {
        char p[PATH_MAX], real[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", home, candidates[i].rel);
        if (!realpath(p, real) || !is_dir(real)) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (strcmp(roots[k].path, real) == 0) dup = true;
        if (dup) continue;
        snprintf(roots[n].path, sizeof(roots[n].path), "%s", real);
        roots[n].flatpak = candidates[i].flatpak;
        n++;
    }
    return n;
}

/* Look for FpsAimForge's appmanifest in one library and resolve its install dir */
static bool library_find_faf(const char *lib, char *game_dir, size_t sz) {
    char acf[PATH_MAX + 64];
    snprintf(acf, sizeof(acf), "%s/steamapps/appmanifest_" FAF_STEAM_APPID ".acf", lib);
    FILE *f = fopen(acf, "r");
    if (!f) return false;
    char line[1024], installdir[256] = "";
    while (fgets(line, sizeof(line), f)) {
        if (vdf_line_value(line, "installdir", installdir, sizeof(installdir))) break;
    }
    fclose(f);
    if (!installdir[0]) snprintf(installdir, sizeof(installdir), FAF_EXE_NAME);
    snprintf(game_dir, sz, "%s/steamapps/common/%s", lib, installdir);
    return is_dir(game_dir);
}

static bool locate_game(char *game_dir, size_t sz, bool *flatpak) {
    steam_root_t roots[8];
    int nroots = find_steam_roots(roots, 8);
    for (int r = 0; r < nroots; r++) {
        *flatpak = roots[r].flatpak;
        if (library_find_faf(roots[r].path, game_dir, sz)) return true;

        char vdf[PATH_MAX + 64];
        snprintf(vdf, sizeof(vdf), "%s/steamapps/libraryfolders.vdf", roots[r].path);
        FILE *f = fopen(vdf, "r");
        if (!f) continue;
        char line[1024], lib[PATH_MAX];
        bool found = false;
        while (!found && fgets(line, sizeof(line), f)) {
            if (vdf_line_value(line, "path", lib, sizeof(lib))) {
                found = library_find_faf(lib, game_dir, sz);
            }
        }
        fclose(f);
        if (found) return true;
    }
    return false;
}

/* Files shipped next to the anymouse binary (repo checkout or release folder) */
static bool source_paths(char *launcher, char *plugin, char *bundle, size_t sz) {
    char dir[PATH_MAX];
    if (!config_exe_dir(dir, sizeof(dir))) return false;
    snprintf(launcher, sz, "%s/faf_launcher", dir);
    snprintf(plugin, sz, "%s/" FAF_PLUGIN_NAME, dir);
    snprintf(bundle, sz, "%s/faf/" FAF_BUNDLE_NAME, dir);
    if (!path_exists(bundle)) snprintf(bundle, sz, "%s/" FAF_BUNDLE_NAME, dir);
    return path_exists(launcher) && path_exists(plugin) && path_exists(bundle);
}

bool faf_install_detect(faf_install_info_t *out) {
    memset(out, 0, sizeof(*out));
    bool flatpak = false;
    if (!locate_game(out->game_dir, sizeof(out->game_dir), &flatpak)) {
        out->state = FAF_INSTALL_GAME_NOT_FOUND;
        out->game_dir[0] = '\0';
        snprintf(out->detail, sizeof(out->detail), "FpsAimForge not found in any Steam library");
        return false;
    }

    char home[PATH_MAX];
    config_user_home(home, sizeof(home));
    snprintf(out->bundle_dir, sizeof(out->bundle_dir), "%s%s/.local/share/FpsAimForge/bundles", home,
             flatpak ? "/.var/app/com.valvesoftware.Steam" : "");

    char exe[PATH_MAX + 32], bin[PATH_MAX + 32], plugin[PATH_MAX + 32], bundle[PATH_MAX + 32];
    snprintf(exe, sizeof(exe), "%s/" FAF_EXE_NAME, out->game_dir);
    snprintf(bin, sizeof(bin), "%s/" FAF_EXE_NAME ".bin", out->game_dir);
    snprintf(plugin, sizeof(plugin), "%s/" FAF_PLUGIN_NAME, out->game_dir);
    snprintf(bundle, sizeof(bundle), "%s/" FAF_BUNDLE_NAME, out->bundle_dir);

    bool launcher = is_anymouse_launcher(exe);
    if (!launcher) {
        if (path_exists(bin)) {
            out->state = FAF_INSTALL_NEEDS_REPAIR;
            snprintf(out->detail, sizeof(out->detail), "Game was updated by Steam; launcher needs reinstalling");
        } else {
            out->state = FAF_INSTALL_NOT_INSTALLED;
            snprintf(out->detail, sizeof(out->detail), "Found FpsAimForge; AnyMouse integration not installed");
        }
        return true;
    }
    if (!path_exists(bin)) {
        out->state = FAF_INSTALL_NEEDS_REPAIR;
        snprintf(out->detail, sizeof(out->detail), "Launcher installed but FpsAimForge.bin is missing (verify game files in Steam)");
        return true;
    }

    char src_launcher[PATH_MAX], src_plugin[PATH_MAX], src_bundle[PATH_MAX];
    bool have_src = source_paths(src_launcher, src_plugin, src_bundle, PATH_MAX);
    if (!path_exists(plugin) || !path_exists(bundle)) {
        out->state = FAF_INSTALL_OUTDATED;
        snprintf(out->detail, sizeof(out->detail), "Installed, but %s is missing",
                 !path_exists(plugin) ? "the telemetry plugin" : "the AnyMouse scenario bundle");
    } else if (have_src && (!files_identical(src_plugin, plugin) || !files_identical(src_bundle, bundle) ||
                            !files_identical(src_launcher, exe))) {
        out->state = FAF_INSTALL_OUTDATED;
        snprintf(out->detail, sizeof(out->detail), "Installed, but older than this build (update available)");
    } else {
        out->state = FAF_INSTALL_OK;
        snprintf(out->detail, sizeof(out->detail), "Installed and up to date");
    }
    return true;
}

bool faf_install_apply(char *msg, size_t msg_sz) {
    faf_install_info_t info;
    if (!faf_install_detect(&info)) {
        snprintf(msg, msg_sz, "%s", info.detail);
        return false;
    }
    char src_launcher[PATH_MAX], src_plugin[PATH_MAX], src_bundle[PATH_MAX];
    if (!source_paths(src_launcher, src_plugin, src_bundle, PATH_MAX)) {
        snprintf(msg, msg_sz, "Build artifacts missing next to anymouse (run `make` first)");
        return false;
    }

    char exe[PATH_MAX + 32], bin[PATH_MAX + 32], plugin[PATH_MAX + 32], bundle[PATH_MAX + 32];
    snprintf(exe, sizeof(exe), "%s/" FAF_EXE_NAME, info.game_dir);
    snprintf(bin, sizeof(bin), "%s/" FAF_EXE_NAME ".bin", info.game_dir);
    snprintf(plugin, sizeof(plugin), "%s/" FAF_PLUGIN_NAME, info.game_dir);
    snprintf(bundle, sizeof(bundle), "%s/" FAF_BUNDLE_NAME, info.bundle_dir);

    /* Move the real game binary aside (also replaces a stale .bin after a Steam update) */
    if (path_exists(exe) && !is_anymouse_launcher(exe)) {
        /* Refuse to overwrite the real game with anything that doesn't look like it */
        if (file_size(exe) < (1 << 20)) {
            snprintf(msg, msg_sz, "%s is unexpectedly small; not touching it (verify game files in Steam)", exe);
            return false;
        }
        if (rename(exe, bin) != 0) {
            snprintf(msg, msg_sz, "Could not move %s aside: %s", exe, strerror(errno));
            return false;
        }
    }
    if (!path_exists(bin)) {
        snprintf(msg, msg_sz, "FpsAimForge.bin missing; verify game files in Steam, then install again");
        return false;
    }
    if (!install_file(src_plugin, plugin, 0755) || !install_file(src_launcher, exe, 0755)) {
        snprintf(msg, msg_sz, "Failed writing into %s: %s", info.game_dir, strerror(errno));
        return false;
    }
    if (!config_mkdirs_for_user(info.bundle_dir) || !install_file(src_bundle, bundle, 0644)) {
        snprintf(msg, msg_sz, "Failed writing bundle to %s: %s", info.bundle_dir, strerror(errno));
        return false;
    }
    snprintf(msg, msg_sz, "Installed into %s (bundle: %s)", info.game_dir, bundle);
    return true;
}

bool faf_install_remove(char *msg, size_t msg_sz) {
    faf_install_info_t info;
    if (!faf_install_detect(&info)) {
        snprintf(msg, msg_sz, "%s", info.detail);
        return false;
    }
    char exe[PATH_MAX + 32], bin[PATH_MAX + 32], plugin[PATH_MAX + 32], bundle[PATH_MAX + 32];
    snprintf(exe, sizeof(exe), "%s/" FAF_EXE_NAME, info.game_dir);
    snprintf(bin, sizeof(bin), "%s/" FAF_EXE_NAME ".bin", info.game_dir);
    snprintf(plugin, sizeof(plugin), "%s/" FAF_PLUGIN_NAME, info.game_dir);
    snprintf(bundle, sizeof(bundle), "%s/" FAF_BUNDLE_NAME, info.bundle_dir);

    if (is_anymouse_launcher(exe)) {
        if (!path_exists(bin) || rename(bin, exe) != 0) {
            snprintf(msg, msg_sz, "Could not restore original binary (verify game files in Steam)");
            return false;
        }
    }
    unlink(plugin);
    unlink(bundle);
    snprintf(msg, msg_sz, "Restored original FpsAimForge and removed AnyMouse files");
    return true;
}

static bool run_cmd(char *const argv[]) {
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) return false;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return (fclose(f) == 0) && ok;
}

/*
 * Grants the logged-in seat user (uaccess) access to mice, the two supported
 * vendor config interfaces, the AnyMouse dongle and /dev/uinput, so the engine can run without sudo
 * (it then uses the normal scheduler instead of SCHED_FIFO).
 */
bool udev_install_rules(char *msg, size_t msg_sz) {
    if (geteuid() != 0) {
        snprintf(msg, msg_sz, "Run as root: sudo ./anymouse --install-udev");
        return false;
    }
    static const char *RULES =
        "# Generated by AnyMouse-Vectoring (anymouse --install-udev)\n"
        "# Seat user may read mice and create the virtual output mouse.\n"
        "KERNEL==\"uinput\", SUBSYSTEM==\"misc\", MODE=\"0660\", GROUP=\"input\", TAG+=\"uaccess\", OPTIONS+=\"static_node=uinput\"\n"
        "SUBSYSTEM==\"input\", KERNEL==\"event*\", ENV{ID_INPUT_MOUSE}==\"1\", TAG+=\"uaccess\"\n"
        "# Vendor configuration interfaces (CPI / polling rate)\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"361d\", TAG+=\"uaccess\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"3151\", TAG+=\"uaccess\"\n"
        "# AnyMouse dongle control channel (also used by AnyMouse Studio in the browser).\n"
        "# Access only: the app identifies the dongle by its protocol handshake, not by this match.\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{manufacturer}==\"AnyMouse\", TAG+=\"uaccess\"\n";

    if (!write_text("/etc/udev/rules.d/70-anymouse.rules", RULES)) {
        snprintf(msg, msg_sz, "Failed writing /etc/udev/rules.d/70-anymouse.rules: %s", strerror(errno));
        return false;
    }
    if (is_dir("/etc/modules-load.d")) {
        write_text("/etc/modules-load.d/anymouse-uinput.conf", "uinput\n");
    }
    char *modprobe[] = { "modprobe", "uinput", NULL };
    char *reload[]   = { "udevadm", "control", "--reload-rules", NULL };
    char *trigger[]  = { "udevadm", "trigger", "--action=add", "--subsystem-match=input",
                         "--subsystem-match=hidraw", "--subsystem-match=misc", NULL };
    run_cmd(modprobe);
    if (!run_cmd(reload) || !run_cmd(trigger)) {
        snprintf(msg, msg_sz, "Rules written, but udevadm reload/trigger failed (replug devices or reboot)");
        return false;
    }
    snprintf(msg, msg_sz, "Installed /etc/udev/rules.d/70-anymouse.rules; AnyMouse can now run without sudo");
    return true;
}
