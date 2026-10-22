/*
 * AnyMouse-Vectoring — FpsAimForge Steam/Direct Native Launcher Wrapper
 * =====================================================================
 * Installed at /mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge/FpsAimForge
 * Automatically injects libanymouse_faf.so into FpsAimForge.bin while
 * preserving Steam's LD_PRELOAD (gameoverlayrenderer.so) and PID.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAF_REAL_BIN   "/mnt/iscsi-nvme/SteamLibrary/steamapps/common/FpsAimForge/FpsAimForge.bin"
#define FAF_PLUGIN_SO  "/home/future/Desktop/AnyMouse-Vectoring/libanymouse_faf.so"

int main(int argc, char **argv) {
    (void)argc;
    if (access(FAF_PLUGIN_SO, R_OK) == 0) {
        const char *old_preload = getenv("LD_PRELOAD");
        char new_preload[4096];
        if (old_preload && old_preload[0] != '\0') {
            if (strstr(old_preload, FAF_PLUGIN_SO) == NULL) {
                snprintf(new_preload, sizeof(new_preload), "%s:%s", FAF_PLUGIN_SO, old_preload);
                setenv("LD_PRELOAD", new_preload, 1);
            }
        } else {
            setenv("LD_PRELOAD", FAF_PLUGIN_SO, 1);
        }
    }
    execv(FAF_REAL_BIN, argv);
    perror("execv FpsAimForge.bin");
    return 1;
}
