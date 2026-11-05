#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static float rem_x = 0.0f;
static float rem_y = 0.0f;

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void scale_vector(int32_t raw_dx, int32_t raw_dy,
                                int32_t *out_dx, int32_t *out_dy,
                                uint8_t *out_octant) {
    if (__builtin_expect(raw_dx == 0 && raw_dy == 0, 0)) {
        *out_dx = 0;
        *out_dy = 0;
        *out_octant = 0;
        return;
    }

    /* Screen Y is inverted (up is negative dy), so negate raw_dy for standard polar [0, 2*PI) */
    float f_dx = (float)raw_dx;
    float f_dy = (float)raw_dy;
    float theta = atan2f(-f_dy, f_dx);
    if (theta < 0.0f) {
        theta += TWO_PI;
    }

    float octant_pos = theta * INV_OCTANT_STEP;
    int k0 = ((int)octant_pos) & 7;
    int k1 = (k0 + 1) & 7;
    float t = octant_pos - (float)((int)octant_pos);

    /* Nearest octant for telemetry tagging (centered at +/- 22.5 deg around ray k) */
    int nearest_k = ((int)(octant_pos + 0.5f)) & 7;
    *out_octant = (uint8_t)nearest_k;

    /* Cosine C1 smooth interpolation between adjacent octants using lock-free atomic reads */
    float w0 = atomic_load_explicit(&g_state.octant_weights[k0], memory_order_relaxed);
    float w1 = atomic_load_explicit(&g_state.octant_weights[k1], memory_order_relaxed);
    float smooth_t = (1.0f - cosf(t * PI)) * 0.5f;
    float scale = w0 * (1.0f - smooth_t) + w1 * smooth_t;

    /* Error diffusion / sub-pixel remainder accumulation */
    float exact_x = (f_dx * scale) + rem_x;
    float exact_y = (f_dy * scale) + rem_y;

    int32_t ix = (int32_t)exact_x;
    int32_t iy = (int32_t)exact_y;

    rem_x = exact_x - (float)ix;
    rem_y = exact_y - (float)iy;

    *out_dx = ix;
    *out_dy = iy;
}

static inline void push_tick_event(uint64_t ts_ns, int32_t raw_dx, int32_t raw_dy,
                                   int32_t s_dx, int32_t s_dy,
                                   uint8_t buttons, uint8_t octant) {
    uint32_t head = atomic_load_explicit(&g_state.ring_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_state.ring_tail, memory_order_acquire);

    if ((head - tail) < TICK_RING_SIZE) {
        uint32_t slot = head & TICK_RING_MASK;
        g_state.tick_ring[slot] = (tick_event_t){
            .timestamp_ns = ts_ns,
            .raw_dx       = (int16_t)(raw_dx > 32767 ? 32767 : (raw_dx < -32768 ? -32768 : raw_dx)),
            .raw_dy       = (int16_t)(raw_dy > 32767 ? 32767 : (raw_dy < -32768 ? -32768 : raw_dy)),
            .scaled_dx    = (int16_t)(s_dx > 32767 ? 32767 : (s_dx < -32768 ? -32768 : s_dx)),
            .scaled_dy    = (int16_t)(s_dy > 32767 ? 32767 : (s_dy < -32768 ? -32768 : s_dy)),
            .buttons      = buttons,
            .octant_idx   = octant,
        };
        atomic_store_explicit(&g_state.ring_head, head + 1U, memory_order_release);
    }
    atomic_fetch_add_explicit(&g_state.total_ticks_processed, 1ULL, memory_order_relaxed);
}

static int setup_uinput_device(int evfd) {
    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        return -1;
    }

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(ufd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(ufd, UI_SET_KEYBIT, BTN_EXTRA);
    ioctl(ufd, UI_SET_KEYBIT, BTN_FORWARD);
    ioctl(ufd, UI_SET_KEYBIT, BTN_BACK);

    ioctl(ufd, UI_SET_EVBIT, EV_REL);
    ioctl(ufd, UI_SET_RELBIT, REL_X);
    ioctl(ufd, UI_SET_RELBIT, REL_Y);
    ioctl(ufd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(ufd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    ioctl(ufd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
#endif
#ifdef REL_HWHEEL_HI_RES
    ioctl(ufd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif

    ioctl(ufd, UI_SET_EVBIT, EV_SYN);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));

    /* Clone the exact USB Vendor/Product ID and Name of the grabbed physical mouse
     * so KWin Wayland / libinput applies the user's exact kcminputrc flat profile
     * (PointerAccelerationProfile=1, PointerAcceleration=-0.200) instead of falling
     * back to libinput's default 2x Adaptive acceleration curve! */
    if (evfd >= 0 && ioctl(evfd, EVIOCGID, &usetup.id) == 0) {
        usetup.id.bustype = BUS_USB;
    } else {
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor  = 0x361D;
        usetup.id.product = 0x0100;
        usetup.id.version = 0x0111;
    }

    if (evfd < 0 || ioctl(evfd, EVIOCGNAME(UINPUT_MAX_NAME_SIZE), usetup.name) <= 0) {
        snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "Finalmouse UltralightX dongle Mouse");
    }

    if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        close(ufd);
        return -1;
    }
    return ufd;
}

static int setup_udp_socket(void) {
    int sfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sfd < 0) return -1;

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sfd);
        return -1;
    }
    return sfd;
}

static uint64_t monotonic_ms(void) {
    return monotonic_ns() / 1000000ULL;
}

static void set_rt_error(const char *msg) {
    pthread_mutex_lock(&g_state.device_mutex);
    bool changed = strcmp(g_state.evdev_error, msg) != 0;
    snprintf(g_state.evdev_error, sizeof(g_state.evdev_error), "%s", msg);
    pthread_mutex_unlock(&g_state.device_mutex);
    if (changed && msg[0]) fprintf(stderr, "[WARN] %s\n", msg); /* Retries repeat every second */
}

/* Decide which device to hold: the user's explicit choice, otherwise the best-ranked mouse.
 * Returns false with *why set when nothing should be grabbed for a reason other than absence. */
static bool resolve_target_device(char *path, size_t path_sz, mouse_device_t *dev, char *why, size_t why_sz) {
    char req[256];
    devices_get_request(req, sizeof(req));
    memset(dev, 0, sizeof(*dev));
    why[0] = '\0';
    if (req[0]) {
        snprintf(path, path_sz, "%s", req);
        if (!devices_lookup(req, dev)) {
            snprintf(dev->name, sizeof(dev->name), "%s", req);
        } else if (dev->is_dongle) {
            snprintf(why, why_sz, "%.60s is the AnyMouse dongle's output, already vectored in hardware; not grabbing it",
                     dev->name);
            return false;
        }
        return access(req, F_OK) == 0;
    }
    if (!devices_pick_auto(dev)) {
        if (dongle_guard_active()) {
            snprintf(why, why_sz, "AnyMouse dongle is vectoring in hardware; software engine idle");
        }
        return false;
    }
    snprintf(path, path_sz, "%s", devices_preferred_path(dev));
    return true;
}

static void release_device(int *evfd, int *ufd) {
    if (*evfd >= 0) {
        ioctl(*evfd, EVIOCGRAB, 0);
        close(*evfd);
        *evfd = -1;
    }
    if (*ufd >= 0) {
        ioctl(*ufd, UI_DEV_DESTROY);
        close(*ufd);
        *ufd = -1;
    }
    atomic_store(&g_state.evdev_grabbed, false);
    atomic_store(&g_state.active_vid, 0);
    atomic_store(&g_state.active_pid, 0);
    rem_x = 0.0f;
    rem_y = 0.0f;
}

/* Open the physical mouse, clone it via uinput, then grab it. Returns false with an error message set. */
static bool acquire_device(const char *path, const mouse_device_t *dev, int *evfd, int *ufd) {
    char err[160];
    *evfd = open(path, O_RDONLY | O_NONBLOCK);
    if (*evfd < 0) {
        snprintf(err, sizeof(err), "open(%s) failed: %s (run with sudo or install udev rules)",
                 path, strerror(errno));
        set_rt_error(err);
        return false;
    }
    *ufd = setup_uinput_device(*evfd);
    if (*ufd < 0) {
        snprintf(err, sizeof(err), "open(/dev/uinput) failed: %s", strerror(errno));
        set_rt_error(err);
        close(*evfd);
        *evfd = -1;
        return false;
    }
    /* Wait 150ms for compositor/udev to register virtual mouse before grabbing hardware */
    usleep(150000);
    if (ioctl(*evfd, EVIOCGRAB, 1) < 0) {
        snprintf(err, sizeof(err), "EVIOCGRAB(%s) failed: %s", path, strerror(errno));
        set_rt_error(err);
        release_device(evfd, ufd);
        return false;
    }

    struct input_id id;
    if (ioctl(*evfd, EVIOCGID, &id) == 0) {
        atomic_store(&g_state.active_vid, id.vendor);
        atomic_store(&g_state.active_pid, id.product);
    }
    pthread_mutex_lock(&g_state.device_mutex);
    snprintf(g_state.evdev_path, sizeof(g_state.evdev_path), "%s", path);
    snprintf(g_state.active_name, sizeof(g_state.active_name), "%s", dev->name[0] ? dev->name : path);
    g_state.evdev_error[0] = '\0';
    pthread_mutex_unlock(&g_state.device_mutex);
    atomic_store(&g_state.evdev_grabbed, true);
    printf("[ACTIVE] Grabbed hardware mouse: %s (%s)\n", dev->name, path);
    return true;
}

void *evdev_rt_thread(void *arg) {
    (void)arg;

    /* Elevate this hot-path thread to SCHED_FIFO priority 80 if permitted */
    struct sched_param sp = { .sched_priority = 80 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
        printf("[RT] evdev_rt_thread: SCHED_FIFO priority 80 & memory locked.\n");
    } else {
        printf("[INFO] evdev_rt_thread: Running with standard scheduler (use sudo for SCHED_FIFO).\n");
    }

    int sfd = setup_udp_socket();
    int evfd = -1;
    int ufd  = -1;
    uint32_t seen_gen = atomic_load(&g_state.device_request_gen);
    uint64_t next_attempt_ms = 0;
    int uinput_write_failures = 0;
    int held_score = 0;

    /* Hotplug watch: lets auto mode move back to a better mouse (e.g. a re-plugged dongle)
     * without polling sysfs from this real-time thread. */
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0 && inotify_add_watch(ifd, "/dev/input", IN_CREATE) < 0) {
        close(ifd);
        ifd = -1;
    }
    uint64_t rescan_at_ms = 0;

    struct pollfd fds[3];
    struct input_event ev_buf[64];
    /* A frame split across reads re-emits REL_X + REL_Y at its SYN: up to 2 extra events */
    struct input_event out_buf[64 + 2];
    int32_t acc_dx = 0, acc_dy = 0;
    uint8_t button_mask = 0;

    while (atomic_load_explicit(&g_state.keep_running, memory_order_relaxed)) {
        /* Device (re)selection: GUI switch request, unplug, or nothing grabbed yet */
        uint32_t gen = atomic_load(&g_state.device_request_gen);
        if (gen != seen_gen) {
            seen_gen = gen;
            release_device(&evfd, &ufd);
            next_attempt_ms = 0;
        }
        if (evfd < 0 && monotonic_ms() >= next_attempt_ms) {
            char path[256], why[160];
            mouse_device_t dev;
            if (!resolve_target_device(path, sizeof(path), &dev, why, sizeof(why))) {
                char err[160];
                char req[256];
                devices_get_request(req, sizeof(req));
                if (why[0])      snprintf(err, sizeof(err), "%s", why);
                else if (req[0]) snprintf(err, sizeof(err), "Waiting for %.120s to be connected...", req);
                else             snprintf(err, sizeof(err), "No mouse detected; waiting for one to be plugged in...");
                set_rt_error(err);
            } else if (acquire_device(path, &dev, &evfd, &ufd)) {
                acc_dx = acc_dy = 0;
                button_mask = 0;
                uinput_write_failures = 0;
                held_score = dev.score;
            }
            if (evfd < 0) next_attempt_ms = monotonic_ms() + 1000;
        }

        /* A device appeared while in auto mode: switch if it outranks the one we hold */
        if (rescan_at_ms && monotonic_ms() >= rescan_at_ms) {
            rescan_at_ms = 0;
            char req[256];
            devices_get_request(req, sizeof(req));
            mouse_device_t best;
            char held_real[PATH_MAX], best_real[PATH_MAX];
            if (evfd >= 0 && !req[0] && devices_pick_auto(&best) && best.score > held_score &&
                realpath(g_state.evdev_path, held_real) && realpath(best.event_path, best_real) &&
                strcmp(held_real, best_real) != 0) {
                printf("[INFO] Better mouse connected (%s); switching.\n", best.name);
                release_device(&evfd, &ufd);
                next_attempt_ms = 0;
                continue;
            }
        }

        int nfds = 0;
        int ev_idx = -1, udp_idx = -1, ino_idx = -1;
        if (evfd >= 0) {
            ev_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = evfd, .events = POLLIN };
        }
        if (sfd >= 0) {
            udp_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
        }
        if (ifd >= 0) {
            ino_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = ifd, .events = POLLIN };
        }

        if (nfds == 0) {
            usleep(50000);
            continue;
        }

        int ret = poll(fds, (nfds_t)nfds, 100);
        if (ret <= 0) continue;

        if (ino_idx >= 0 && (fds[ino_idx].revents & POLLIN)) {
            char ibuf[4096];
            while (read(ifd, ibuf, sizeof(ibuf)) > 0) { /* drain */ }
            /* Give udev time to create by-id links and apply permissions */
            rescan_at_ms = monotonic_ms() + 800;
        }

        /* 1. Hot-path: Mouse evdev hardware events */
        if (ev_idx >= 0 && (fds[ev_idx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            set_rt_error("Mouse disconnected; waiting for it to come back...");
            release_device(&evfd, &ufd);
        } else if (ev_idx >= 0 && (fds[ev_idx].revents & POLLIN)) {
            ssize_t rd = read(evfd, ev_buf, sizeof(ev_buf));
            if (rd > 0) {
                int count = (int)(rd / (ssize_t)sizeof(struct input_event));
                int out_idx = 0;
                bool click_changed = false;

                for (int i = 0; i < count; i++) {
                    struct input_event *e = &ev_buf[i];
                    if (e->type == EV_REL) {
                        if (e->code == REL_X) {
                            acc_dx += e->value;
                        } else if (e->code == REL_Y) {
                            acc_dy += e->value;
                        } else {
                            out_buf[out_idx++] = *e;
                        }
                    } else if (e->type == EV_KEY) {
                        if (e->code == BTN_LEFT) {
                            if (e->value) button_mask |= 0x01;
                            else          button_mask &= ~0x01;
                            click_changed = true;
                        } else if (e->code == BTN_RIGHT) {
                            if (e->value) button_mask |= 0x02;
                            else          button_mask &= ~0x02;
                            click_changed = true;
                        }
                        out_buf[out_idx++] = *e;
                    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                        int32_t raw_x = acc_dx;
                        int32_t raw_y = acc_dy;
                        int32_t s_dx = 0, s_dy = 0;
                        uint8_t octant = 0;

                        if (raw_x != 0 || raw_y != 0) {
                            scale_vector(raw_x, raw_y, &s_dx, &s_dy, &octant);
                            acc_dx = 0;
                            acc_dy = 0;

                            if (s_dx != 0) {
                                out_buf[out_idx++] = (struct input_event){
                                    .time = e->time, .type = EV_REL, .code = REL_X, .value = s_dx
                                };
                            }
                            if (s_dy != 0) {
                                out_buf[out_idx++] = (struct input_event){
                                    .time = e->time, .type = EV_REL, .code = REL_Y, .value = s_dy
                                };
                            }
                        }

                        if (raw_x != 0 || raw_y != 0 || click_changed) {
                            uint64_t ts_ns = (uint64_t)e->time.tv_sec * 1000000000ULL +
                                             (uint64_t)e->time.tv_usec * 1000ULL;
                            if (ts_ns == 0) ts_ns = monotonic_ns();
                            push_tick_event(ts_ns, raw_x, raw_y, s_dx, s_dy, button_mask, octant);
                            click_changed = false;
                        }

                        out_buf[out_idx++] = *e;
                    }
                }

                if (out_idx > 0 && ufd >= 0) {
                    ssize_t wr = write(ufd, out_buf, (size_t)out_idx * sizeof(struct input_event));
                    if (wr < 0 && errno != EAGAIN) {
                        /* A dead virtual mouse would leave the user without a pointer: let go */
                        if (++uinput_write_failures >= 50) {
                            set_rt_error("Virtual mouse stopped accepting events; released the grab");
                            release_device(&evfd, &ufd);
                            next_attempt_ms = monotonic_ms() + 1000;
                        }
                    } else {
                        uinput_write_failures = 0;
                    }
                }
            } else if (rd < 0 && errno == ENODEV) {
                set_rt_error("Mouse unplugged or went to sleep; waiting for it to come back...");
                release_device(&evfd, &ufd);
            }
        }

        /* 2. Optional External UDP listener (for CLI/FpsAimForge compatibility) */
        if (udp_idx >= 0 && (fds[udp_idx].revents & POLLIN)) {
            float incoming[8];
            ssize_t n = recv(sfd, incoming, sizeof(incoming), 0);
            if (n == sizeof(incoming)) {
                bool valid = true;
                for (int i = 0; i < 8; i++) {
                    if (isnan(incoming[i]) || incoming[i] < 0.01f || incoming[i] > 5.0f) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    for (int i = 0; i < 8; i++) {
                        atomic_store_explicit(&g_state.octant_weights[i], incoming[i], memory_order_relaxed);
                    }
                }
            }
        }
    }

    release_device(&evfd, &ufd);
    if (sfd >= 0) {
        close(sfd);
    }
    if (ifd >= 0) {
        close(ifd);
    }
    printf("[EXIT] evdev_rt_thread released mouse grab cleanly.\n");
    return NULL;
}
