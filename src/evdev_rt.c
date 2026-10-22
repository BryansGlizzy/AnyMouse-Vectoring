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

    evfd = open(g_state.evdev_path, O_RDONLY | O_NONBLOCK);
    if (evfd < 0) {
        snprintf(g_state.evdev_error, sizeof(g_state.evdev_error),
                 "open(%s) failed: %s (run with sudo or input group)",
                 g_state.evdev_path, strerror(errno));
        fprintf(stderr, "[WARN] %s\n", g_state.evdev_error);
    } else {
        ufd = setup_uinput_device(evfd);
        if (ufd < 0) {
            snprintf(g_state.evdev_error, sizeof(g_state.evdev_error),
                     "open(/dev/uinput) failed: %s", strerror(errno));
            fprintf(stderr, "[WARN] %s\n", g_state.evdev_error);
            close(evfd);
            evfd = -1;
        } else {
            /* Wait 150ms for compositor/udev to register virtual mouse before grabbing hardware */
            usleep(150000);
            if (ioctl(evfd, EVIOCGRAB, 1) < 0) {
                snprintf(g_state.evdev_error, sizeof(g_state.evdev_error),
                         "EVIOCGRAB failed: %s", strerror(errno));
                fprintf(stderr, "[WARN] %s\n", g_state.evdev_error);
                ioctl(ufd, UI_DEV_DESTROY);
                close(ufd);
                close(evfd);
                ufd = -1;
                evfd = -1;
            } else {
                atomic_store(&g_state.evdev_grabbed, true);
                g_state.evdev_error[0] = '\0';
                printf("[ACTIVE] Grabbed hardware mouse: %s\n", g_state.evdev_path);
            }
        }
    }

    struct pollfd fds[2];
    struct input_event ev_buf[64];
    struct input_event out_buf[64];
    int32_t acc_dx = 0, acc_dy = 0;
    uint8_t button_mask = 0;

    while (atomic_load_explicit(&g_state.keep_running, memory_order_relaxed)) {
        int nfds = 0;
        if (evfd >= 0) {
            fds[nfds++] = (struct pollfd){ .fd = evfd, .events = POLLIN };
        }
        if (sfd >= 0) {
            fds[nfds++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
        }

        if (nfds == 0) {
            usleep(50000);
            continue;
        }

        int ret = poll(fds, (nfds_t)nfds, 100);
        if (ret <= 0) continue;

        /* 1. Hot-path: Mouse evdev hardware events */
        if (evfd >= 0 && (fds[0].revents & POLLIN)) {
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
                    (void)wr;
                }
            } else if (rd < 0 && errno == ENODEV) {
                fprintf(stderr, "\n[WARN] Mouse unplugged or went to sleep.\n");
                break;
            }
        }

        /* 2. Optional External UDP listener (for CLI/FpsAimForge compatibility) */
        int udp_idx = (evfd >= 0) ? 1 : 0;
        if (sfd >= 0 && (fds[udp_idx].revents & POLLIN)) {
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

    if (evfd >= 0) {
        ioctl(evfd, EVIOCGRAB, 0);
        close(evfd);
    }
    if (ufd >= 0) {
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
    }
    if (sfd >= 0) {
        close(sfd);
    }
    atomic_store(&g_state.evdev_grabbed, false);
    printf("[EXIT] evdev_rt_thread released mouse grab cleanly.\n");
    return NULL;
}
