#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define PI 3.14159265358979323846f
#define TWO_PI (2.0f * PI)
#define INV_OCTANT_STEP (4.0f / PI) // 1.0 / (PI / 4.0)
#define UDP_PORT 42069

#define DEFAULT_DEV_ULX "/dev/input/by-id/usb-Finalmouse_UltralightX_dongle_BE1E28B86F3859BD-event-mouse"
#define DEFAULT_DEV_ATTACKSHARK "/dev/input/by-id/usb-_2.4G_Wireless_Mouse-event-mouse"

static volatile sig_atomic_t keep_running = 1;
static void sig_handler(int sig) { (void)sig; keep_running = 0; }

// Octant order (Cartesian Y-up):
// 0: E (0°), 1: NE (45°), 2: N (90°), 3: NW (135°),
// 4: W (180°), 5: SW (225°), 6: S (270°), 7: SE (315°)
static float octant_weights[8] = {
    0.25f, 0.25f, 0.25f, 0.25f,
    0.25f, 0.25f, 0.25f, 0.25f
};

static float rem_x = 0.0f;
static float rem_y = 0.0f;

static inline void scale_vector(int32_t raw_dx, int32_t raw_dy, int32_t *out_dx, int32_t *out_dy) {
    if (__builtin_expect(raw_dx == 0 && raw_dy == 0, 0)) {
        *out_dx = 0;
        *out_dy = 0;
        return;
    }

    // Screen Y is inverted (up is negative dy), so negate raw_dy for standard polar [0, 2*PI)
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

    // Cosine C1 smooth interpolation between adjacent octants
    float smooth_t = (1.0f - cosf(t * PI)) * 0.5f;
    float scale = octant_weights[k0] * (1.0f - smooth_t) + octant_weights[k1] * smooth_t;

    // Error diffusion / sub-pixel remainder accumulation
    float exact_x = (f_dx * scale) + rem_x;
    float exact_y = (f_dy * scale) + rem_y;

    int32_t ix = (int32_t)exact_x; // truncates toward zero in C99
    int32_t iy = (int32_t)exact_y;

    rem_x = exact_x - (float)ix;
    rem_y = exact_y - (float)iy;

    *out_dx = ix;
    *out_dy = iy;
}

static int setup_uinput_device(void) {
    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        perror("open /dev/uinput");
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
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x361D;
    usetup.id.product = 0x0101;
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "Octant Polar Vector Mouse");

    if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("ioctl uinput setup/create");
        close(ufd);
        return -1;
    }
    return ufd;
}

static int setup_udp_socket(void) {
    int sfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sfd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind UDP 42069");
        close(sfd);
        return -1;
    }
    return sfd;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [DEVICE_PATH] [BASE_SCALE]\n", prog);
    printf("  DEVICE_PATH: Path to evdev mouse node (default: auto-detect ULX or Attack Shark)\n");
    printf("  BASE_SCALE:  Uniform initial multiplier across all 8 octants (e.g. 0.25)\n");
    printf("Examples:\n");
    printf("  sudo %s                                                # Auto-detects mouse, default 0.25x\n", prog);
    printf("  sudo %s " DEFAULT_DEV_ULX " 0.25                       # Finalmouse ULX @ 0.25x (3200->800 DPI)\n", prog);
    printf("  sudo %s " DEFAULT_DEV_ATTACKSHARK " 0.125              # Attack Shark R3 @ 0.125x (6400->800 DPI)\n", prog);
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    const char *dev_path = NULL;
    if (argc > 1 && argv[1][0] == '/') {
        dev_path = argv[1];
    } else {
        // Auto-detect available default mouse
        if (access(DEFAULT_DEV_ULX, F_OK) == 0) {
            dev_path = DEFAULT_DEV_ULX;
        } else if (access(DEFAULT_DEV_ATTACKSHARK, F_OK) == 0) {
            dev_path = DEFAULT_DEV_ATTACKSHARK;
        } else {
            dev_path = DEFAULT_DEV_ULX; // Fallback to ULX default path
        }
    }

    int scale_arg_idx = (argc > 1 && argv[1][0] == '/') ? 2 : 1;
    if (argc > scale_arg_idx) {
        float base = strtof(argv[scale_arg_idx], NULL);
        if (base > 0.01f && base <= 4.0f) {
            for (int i = 0; i < 8; i++) octant_weights[i] = base;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Optional RT scheduling if permitted by PAM/ulimit or sudo
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
        printf("[RT] SCHED_FIFO priority 80 & memory locked.\n");
    } else {
        printf("[INFO] Running without SCHED_FIFO (run with sudo or RT privileges for microsecond scheduling).\n");
    }

    int evfd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (evfd < 0) {
        fprintf(stderr, "Failed to open input device: %s\n", dev_path);
        perror("Error details (check file permissions, sudo, or 'input' group)");
        return 1;
    }

    int ufd = setup_uinput_device();
    if (ufd < 0) {
        close(evfd);
        return 1;
    }

    int sfd = setup_udp_socket();

    // Wait 150ms for udev/compositor to register virtual mouse before grabbing hardware
    usleep(150000);

    if (ioctl(evfd, EVIOCGRAB, 1) < 0) {
        perror("ioctl EVIOCGRAB failed (another process may have grabbed the device)");
        close(ufd);
        close(evfd);
        if (sfd >= 0) close(sfd);
        return 1;
    }

    printf("[ACTIVE] Grabbed: %s\n", dev_path);
    printf("[WEIGHTS] E:%.3f NE:%.3f N:%.3f NW:%.3f W:%.3f SW:%.3f S:%.3f SE:%.3f\n",
           octant_weights[0], octant_weights[1], octant_weights[2], octant_weights[3],
           octant_weights[4], octant_weights[5], octant_weights[6], octant_weights[7]);
    printf("[IPC] Listening on UDP 127.0.0.1:%d for 8x float32 weight updates (Ctrl+C to exit)\n", UDP_PORT);

    struct pollfd fds[2] = {
        { .fd = evfd, .events = POLLIN },
        { .fd = sfd,  .events = POLLIN }
    };

    struct input_event ev_buf[64];
    struct input_event out_buf[64];
    int32_t acc_dx = 0, acc_dy = 0;

    while (keep_running) {
        int ret = poll(fds, (sfd >= 0) ? 2 : 1, -1);
        if (__builtin_expect(ret <= 0, 0)) continue;

        // 1. Hot-path: Mouse evdev hardware events
        if (fds[0].revents & POLLIN) {
            ssize_t rd = read(evfd, ev_buf, sizeof(ev_buf));
            if (rd > 0) {
                int count = (int)(rd / (ssize_t)sizeof(struct input_event));
                int out_idx = 0;

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
                    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                        if (acc_dx != 0 || acc_dy != 0) {
                            int32_t s_dx, s_dy;
                            scale_vector(acc_dx, acc_dy, &s_dx, &s_dy);
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
                        out_buf[out_idx++] = *e;
                    } else if (e->type == EV_KEY) {
                        out_buf[out_idx++] = *e;
                    }
                }

                if (out_idx > 0) {
                    ssize_t wr = write(ufd, out_buf, (size_t)out_idx * sizeof(struct input_event));
                    (void)wr;
                }
            } else if (rd < 0 && errno == ENODEV) {
                fprintf(stderr, "\n[WARN] Mouse unplugged or went to sleep. Exiting.\n");
                break;
            }
        }

        // 2. Cold-path: Live weight updates over UDP (32 bytes = 8 floats)
        if (sfd >= 0 && (fds[1].revents & POLLIN)) {
            float incoming[8];
            ssize_t n = recv(sfd, incoming, sizeof(incoming), 0);
            if (n == sizeof(incoming)) {
                int valid = 1;
                for (int i = 0; i < 8; i++) {
                    if (isnan(incoming[i]) || incoming[i] < 0.01f || incoming[i] > 5.0f) {
                        valid = 0;
                        break;
                    }
                }
                if (valid) {
                    memcpy(octant_weights, incoming, sizeof(octant_weights));
                    printf("[LIVE UPDATE] E:%.3f NE:%.3f N:%.3f NW:%.3f W:%.3f SW:%.3f S:%.3f SE:%.3f\n",
                           octant_weights[0], octant_weights[1], octant_weights[2], octant_weights[3],
                           octant_weights[4], octant_weights[5], octant_weights[6], octant_weights[7]);
                }
            }
        }
    }

    ioctl(evfd, EVIOCGRAB, 0);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    close(evfd);
    if (sfd >= 0) close(sfd);
    printf("\n[EXIT] Released mouse grab cleanly.\n");
    return 0;
}
