#define _GNU_SOURCE
#include "core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * Universal mouse discovery over /sys/class/input.
 *
 * A candidate is any physical event node exposing REL_X + REL_Y + BTN_LEFT.
 * Our own uinput clone shares the grabbed mouse's name and VID:PID, so it is
 * excluded by its sysfs location (/sys/devices/virtual/input) rather than by ID.
 */

#define BITS_PER_LONG (sizeof(long) * 8)

static bool read_sys_line(const char *path, char *out, size_t out_sz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)out_sz, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok;
}

/* Test bit `bit` in a sysfs capability bitmap ("hex hex hex", most significant word first) */
static bool sys_cap_has_bit(const char *cap_line, unsigned bit) {
    unsigned long words[16];
    int n = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", cap_line);
    for (char *tok = strtok(buf, " "); tok && n < 16; tok = strtok(NULL, " ")) {
        words[n++] = strtoul(tok, NULL, 16);
    }
    unsigned idx = bit / BITS_PER_LONG;
    if ((int)idx >= n) return false;
    return (words[n - 1 - idx] >> (bit % BITS_PER_LONG)) & 1UL;
}

static bool read_cap(const char *event_name, const char *cap, char *out, size_t out_sz) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/input/%s/device/capabilities/%s", event_name, cap);
    return read_sys_line(path, out, out_sz);
}

/* Extract ".../usbN/N-P" (the physical USB device, shared by all its interfaces) */
static void usb_parent_of(const char *real, char *out, size_t out_sz) {
    out[0] = '\0';
    const char *p = strstr(real, "/usb");
    if (!p) return;
    p++;
    const char *slash = strchr(p, '/');
    if (!slash) return;
    const char *end = strchr(slash + 1, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/* Does any event node on the same USB device look like a full keyboard (KEY_A + EV_REP)? */
static bool usb_parent_has_keyboard(const char *usb_parent) {
    if (!usb_parent[0]) return false;
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return false;
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", ent->d_name);
        if (!realpath(link, real)) continue;
        char parent[128];
        usb_parent_of(real, parent, sizeof(parent));
        if (strcmp(parent, usb_parent) != 0) continue;

        char ev[128], key[512];
        if (read_cap(ent->d_name, "ev", ev, sizeof(ev)) &&
            read_cap(ent->d_name, "key", key, sizeof(key)) &&
            sys_cap_has_bit(ev, EV_REP) && sys_cap_has_bit(key, KEY_A)) {
            found = true;
        }
    }
    closedir(dir);
    return found;
}

/* Map /dev/input/eventN to its stable /dev/input/by-id symlink, if one exists */
static void find_by_id_link(const char *event_name, char *out, size_t out_sz) {
    out[0] = '\0';
    DIR *dir = opendir("/dev/input/by-id");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char link[PATH_MAX], target[256];
        snprintf(link, sizeof(link), "/dev/input/by-id/%s", ent->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strcmp(base, event_name) == 0) {
            snprintf(out, out_sz, "%s", link);
            break;
        }
    }
    closedir(dir);
}

bool devices_is_hw_supported(uint16_t vid, uint16_t pid) {
    return (vid == 0x361D && pid == 0x0100) ||  /* Finalmouse UltralightX */
           (vid == 0x3151 && pid == 0x402D);    /* Attack Shark R3 / GearHub 2.4G */
}

static int score_device(const mouse_device_t *d, const char *rel, const char *key, bool kbd_sibling) {
    int score = 0;
    if (devices_is_hw_supported(d->vid, d->pid)) score += 100;
    if (strcasestr(d->name, "mouse")) score += 10;
    if (sys_cap_has_bit(rel, REL_WHEEL)) score += 5;
    if (sys_cap_has_bit(key, BTN_RIGHT)) score += 5;
    if (d->byid_path[0]) score += 2;
    /* Keyboards / macro pads that expose an auxiliary mouse interface */
    if (kbd_sibling) score -= 40;
    return score;
}

int devices_enumerate(mouse_device_t *out, int max) {
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", ent->d_name);
        if (!realpath(link, real)) continue;
        if (strstr(real, "/devices/virtual/")) continue; /* uinput clones, including our own */

        char rel[128], key[512];
        if (!read_cap(ent->d_name, "rel", rel, sizeof(rel))) continue;
        if (!read_cap(ent->d_name, "key", key, sizeof(key))) continue;
        if (!sys_cap_has_bit(rel, REL_X) || !sys_cap_has_bit(rel, REL_Y)) continue;
        if (!sys_cap_has_bit(key, BTN_LEFT)) continue;

        mouse_device_t *d = &out[count];
        memset(d, 0, sizeof(*d));
        snprintf(d->event_path, sizeof(d->event_path), "/dev/input/%.40s", ent->d_name);

        char path[PATH_MAX], val[64];
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", ent->d_name);
        if (!read_sys_line(path, d->name, sizeof(d->name))) snprintf(d->name, sizeof(d->name), "Unknown mouse");
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/vendor", ent->d_name);
        if (read_sys_line(path, val, sizeof(val))) d->vid = (uint16_t)strtoul(val, NULL, 16);
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/product", ent->d_name);
        if (read_sys_line(path, val, sizeof(val))) d->pid = (uint16_t)strtoul(val, NULL, 16);

        find_by_id_link(ent->d_name, d->byid_path, sizeof(d->byid_path));
        usb_parent_of(real, d->usb_parent, sizeof(d->usb_parent));
        d->hw_supported = devices_is_hw_supported(d->vid, d->pid);
        d->score = score_device(d, rel, key, usb_parent_has_keyboard(d->usb_parent));
        count++;
    }
    closedir(dir);

    /* Highest score first; ties broken by path for a stable order */
    for (int i = 1; i < count; i++) {
        mouse_device_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && (out[j].score < tmp.score ||
                          (out[j].score == tmp.score && strcmp(out[j].event_path, tmp.event_path) > 0))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
    return count;
}

const char *devices_preferred_path(const mouse_device_t *d) {
    return d->byid_path[0] ? d->byid_path : d->event_path;
}

bool devices_pick_auto(mouse_device_t *out) {
    mouse_device_t list[DEVICES_MAX];
    int n = devices_enumerate(list, DEVICES_MAX);
    if (n <= 0) return false;
    *out = list[0];
    return true;
}

/* Resolve a configured path (by-id link or eventN) to the matching discovered device */
bool devices_lookup(const char *path, mouse_device_t *out) {
    char want[PATH_MAX];
    if (!realpath(path, want)) return false;
    mouse_device_t list[DEVICES_MAX];
    int n = devices_enumerate(list, DEVICES_MAX);
    for (int i = 0; i < n; i++) {
        char have[PATH_MAX];
        if (realpath(list[i].event_path, have) && strcmp(have, want) == 0) {
            *out = list[i];
            return true;
        }
    }
    return false;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/*
 * "Move the mouse you want" identification: listen (without grabbing) to every
 * candidate and return the one that produced the most motion within timeout_ms.
 * The currently grabbed device delivers nothing to other readers, so it is
 * measured through the RT engine's tick counter instead.
 */
int devices_identify_by_motion(mouse_device_t *out, int timeout_ms) {
    mouse_device_t list[DEVICES_MAX];
    int n = devices_enumerate(list, DEVICES_MAX);
    if (n <= 0) return -1;

    int fds[DEVICES_MAX];
    long motion[DEVICES_MAX];
    char active_real[PATH_MAX] = "";
    if (atomic_load(&g_state.evdev_grabbed)) {
        char held[256];
        pthread_mutex_lock(&g_state.device_mutex);
        snprintf(held, sizeof(held), "%s", g_state.evdev_path);
        pthread_mutex_unlock(&g_state.device_mutex);
        if (!realpath(held, active_real)) active_real[0] = '\0';
    }
    int active_idx = -1;

    for (int i = 0; i < n; i++) {
        motion[i] = 0;
        fds[i] = -1;
        char real[PATH_MAX];
        if (active_real[0] && realpath(list[i].event_path, real) && strcmp(real, active_real) == 0) {
            active_idx = i;
            continue;
        }
        fds[i] = open(list[i].event_path, O_RDONLY | O_NONBLOCK);
    }

    uint64_t ticks0 = atomic_load(&g_state.total_ticks_processed);
    uint64_t deadline = mono_ms() + (uint64_t)timeout_ms;
    struct pollfd pfds[DEVICES_MAX];
    int map[DEVICES_MAX];

    while (mono_ms() < deadline) {
        int np = 0;
        for (int i = 0; i < n; i++) {
            if (fds[i] >= 0) {
                pfds[np] = (struct pollfd){ .fd = fds[i], .events = POLLIN };
                map[np++] = i;
            }
        }
        if (np == 0) {
            usleep(50000);
        } else if (poll(pfds, (nfds_t)np, 50) > 0) {
            for (int p = 0; p < np; p++) {
                if (!(pfds[p].revents & POLLIN)) continue;
                struct input_event evs[64];
                ssize_t rd = read(pfds[p].fd, evs, sizeof(evs));
                for (int e = 0; rd > 0 && e < (int)(rd / (ssize_t)sizeof(evs[0])); e++) {
                    if (evs[e].type == EV_REL && (evs[e].code == REL_X || evs[e].code == REL_Y)) {
                        motion[map[p]] += labs((long)evs[e].value);
                    }
                }
            }
        }
        if (active_idx >= 0) {
            /* Each RT tick is one motion/click frame; weight comparably to raw counts */
            motion[active_idx] = (long)(atomic_load(&g_state.total_ticks_processed) - ticks0) * 4;
        }
    }

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (fds[i] >= 0) close(fds[i]);
        if (motion[i] > 50 && (best < 0 || motion[i] > motion[best])) best = i;
    }
    if (best >= 0) *out = list[best];
    return best >= 0 ? 0 : -1;
}

/* Ask the RT engine to switch devices. path == NULL or "" means automatic selection. */
void devices_request_switch(const char *path) {
    pthread_mutex_lock(&g_state.device_mutex);
    snprintf(g_state.requested_path, sizeof(g_state.requested_path), "%s", path ? path : "");
    pthread_mutex_unlock(&g_state.device_mutex);
    atomic_fetch_add(&g_state.device_request_gen, 1U);
}

void devices_get_request(char *out, size_t out_sz) {
    pthread_mutex_lock(&g_state.device_mutex);
    snprintf(out, out_sz, "%s", g_state.requested_path);
    pthread_mutex_unlock(&g_state.device_mutex);
}
