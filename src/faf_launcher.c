/*
 * AnyMouse-Vectoring — FpsAimForge Steam/Direct Native Launcher Wrapper
 * =====================================================================
 * Installed by `anymouse --install-faf` as <game dir>/FpsAimForge, with the real
 * game moved to <game dir>/FpsAimForge.bin and the plugin at
 * <game dir>/libanymouse_faf.so. Paths are resolved from this executable's own
 * location, so any Steam library works. Steam's LD_PRELOAD
 * (gameoverlayrenderer.so) and the process PID are preserved via execv().
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Lets the installer tell this wrapper apart from the real game binary */
__attribute__((used)) static const char LAUNCHER_MARKER[] = "ANYMOUSE_FAF_LAUNCHER v2";

#define FAF_PLUGIN_NAME "libanymouse_faf.so"

int main(int argc, char **argv) {
    (void)argc;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        perror("readlink /proc/self/exe");
        return 1;
    }
    self[n] = '\0';

    char real_bin[PATH_MAX + 8];
    snprintf(real_bin, sizeof(real_bin), "%s.bin", self);

    char plugin[PATH_MAX + 32];
    char *slash = strrchr(self, '/');
    if (slash) *slash = '\0';
    snprintf(plugin, sizeof(plugin), "%s/" FAF_PLUGIN_NAME, self);

    if (access(plugin, R_OK) == 0) {
        const char *old_preload = getenv("LD_PRELOAD");
        char new_preload[8192];
        if (old_preload && old_preload[0] != '\0') {
            if (strstr(old_preload, plugin) == NULL) {
                snprintf(new_preload, sizeof(new_preload), "%s:%s", plugin, old_preload);
                setenv("LD_PRELOAD", new_preload, 1);
            }
        } else {
            setenv("LD_PRELOAD", plugin, 1);
        }
    } else {
        fprintf(stderr, "[%s] %s not found; starting FpsAimForge without telemetry\n",
                LAUNCHER_MARKER, plugin);
    }
    execv(real_bin, argv);
    perror("execv FpsAimForge.bin");
    return 1;
}
