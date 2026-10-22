#define _GNU_SOURCE
#include "core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Finalmouse UltralightX HID Protocol Constants */
#define FINALMOUSE_VID           0x361D
#define FINALMOUSE_ULX_PID       0x0100
#define FINALMOUSE_REPORT_MAIN   0x04
#define FINALMOUSE_REPORT_INPUT  0x05

#define FM_CMD_DPI               16
#define FM_CMD_POLLING_RATE      17
#define FM_CMD_MOTION_SYNC       18
#define FM_CMD_LOD               21
#define FM_CMD_WAKE_ALL          96

/* Attack Shark / GearHub Constants */
#define GEARHUB_VID              0x3151
#define GEARHUB_PID_24G          0x402D

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void build_fm_report(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t out_buf[64]) {
    memset(out_buf, 0, 64);
    if (payload_len > 60) payload_len = 60;
    out_buf[0] = FINALMOUSE_REPORT_MAIN;
    out_buf[1] = (uint8_t)(2 + payload_len);
    out_buf[2] = (uint8_t)(0x80 | (cmd & 0x7F));
    out_buf[3] = (uint8_t)payload_len;
    if (payload && payload_len > 0) {
        memcpy(&out_buf[4], payload, payload_len);
    }
}

static bool find_mouse_hidraw_node(mouse_hw_status_t *out_info) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return false;

    bool found = false;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char uevent_path[512];
        snprintf(uevent_path, sizeof(uevent_path), "/sys/class/hidraw/%s/device/uevent", ent->d_name);

        FILE *f = fopen(uevent_path, "r");
        if (!f) continue;

        uint16_t vid = 0, pid = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "HID_ID=", 7) == 0) {
                unsigned int bus = 0, v = 0, p = 0;
                if (sscanf(line + 7, "%x:%x:%x", &bus, &v, &p) == 3) {
                    vid = (uint16_t)v;
                    pid = (uint16_t)p;
                }
            }
        }
        fclose(f);

        char dev_node[64];
        snprintf(dev_node, sizeof(dev_node), "/dev/%s", ent->d_name);
        bool acc = (access(dev_node, R_OK | W_OK) == 0);

        if (vid == FINALMOUSE_VID && pid == FINALMOUSE_ULX_PID) {
            memset(out_info, 0, sizeof(*out_info));
            out_info->vid = vid;
            out_info->pid = pid;
            out_info->accessible = acc;
            snprintf(out_info->brand, sizeof(out_info->brand), "Finalmouse");
            snprintf(out_info->model, sizeof(out_info->model), "UltralightX");
            snprintf(out_info->node, sizeof(out_info->node), "%s", dev_node);
            found = true;
            break; /* Prefer Finalmouse ULX if connected */
        } else if (!found && vid == GEARHUB_VID && pid == GEARHUB_PID_24G) {
            memset(out_info, 0, sizeof(*out_info));
            out_info->vid = vid;
            out_info->pid = pid;
            out_info->accessible = acc;
            snprintf(out_info->brand, sizeof(out_info->brand), "Attack Shark");
            snprintf(out_info->model, sizeof(out_info->model), "R3 2.4G Wireless");
            snprintf(out_info->node, sizeof(out_info->node), "%s", dev_node);
            found = true;
        }
    }

    closedir(dir);
    return found;
}

bool hid_query_active_mouse(mouse_hw_status_t *out_status) {
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info)) {
        memset(out_status, 0, sizeof(*out_status));
        return false;
    }

    if (info.vid != FINALMOUSE_VID) {
        /* GearHub / Attack Shark detected */
        info.connected = info.accessible;
        info.dpi = atomic_load(&g_state.hardware_cpi);
        *out_status = info;
        return info.connected;
    }

    int fd = open(info.node, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        *out_status = info;
        return false;
    }

    uint8_t req[64];
    build_fm_report(FM_CMD_WAKE_ALL, NULL, 0, req);
    ssize_t wr = write(fd, req, sizeof(req));
    (void)wr;

    uint64_t deadline = now_ms() + 350;
    int packets = 0;

    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 35);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            uint8_t buf[64];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n >= 4 && buf[0] == FINALMOUSE_REPORT_INPUT) {
                packets++;
                uint8_t inner_len = buf[1];
                if (inner_len > n - 2) inner_len = (uint8_t)(n - 2);
                if (inner_len < 2) continue;

                uint8_t cmd = buf[2];
                uint8_t plen = buf[3];
                if (plen > inner_len - 2) plen = (uint8_t)(inner_len - 2);
                const uint8_t *payload = &buf[4];

                if ((cmd == 3 || cmd == 16) && plen >= 2) {
                    info.dpi = (int)(payload[0] | (payload[1] << 8));
                } else if ((cmd == 4 || cmd == 17) && plen >= 2) {
                    info.polling_rate_hz = (int)(payload[0] | (payload[1] << 8));
                } else if (cmd == 5 && plen >= 2) {
                    int mv = (int)(payload[0] | (payload[1] << 8));
                    info.battery_mv = mv;
                    int pct = (mv - 3400) * 100 / (4200 - 3400);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    info.battery_pct = pct;
                } else if (cmd == 11 && plen > 0) {
                    size_t cpy = plen < sizeof(info.dongle_fw) - 1 ? plen : sizeof(info.dongle_fw) - 1;
                    memcpy(info.dongle_fw, payload, cpy);
                    info.dongle_fw[cpy] = '\0';
                } else if (cmd == 12 && plen > 0) {
                    size_t cpy = plen < sizeof(info.mouse_fw) - 1 ? plen : sizeof(info.mouse_fw) - 1;
                    memcpy(info.mouse_fw, payload, cpy);
                    info.mouse_fw[cpy] = '\0';
                } else if (cmd == 13 && plen >= 1) {
                    info.rssi_dbm = (payload[0] <= 127) ? (int)payload[0] : ((int)payload[0] - 256);
                } else if (cmd == 14 && plen > 0) {
                    size_t cpy = plen < sizeof(info.serial) - 1 ? plen : sizeof(info.serial) - 1;
                    memcpy(info.serial, payload, cpy);
                    info.serial[cpy] = '\0';
                } else if (cmd == 18 && plen >= 1) {
                    info.motion_sync = (payload[0] == 1);
                } else if (cmd == 21 && plen >= 1) {
                    info.lod_mm = (int)payload[0];
                }
            }
        }
    }

    close(fd);
    info.connected = (packets > 0);
    if (info.dpi > 0) {
        atomic_store(&g_state.hardware_cpi, info.dpi);
    }
    *out_status = info;

    pthread_mutex_lock(&g_state.telemetry_mutex);
    g_state.hw_status = info;
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    return info.connected;
}

bool hid_set_mouse_cpi(int cpi) {
    if (cpi < 50 || cpi > 26000) return false;
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info) || info.vid != FINALMOUSE_VID) return false;

    int fd = open(info.node, O_RDWR);
    if (fd < 0) return false;

    uint8_t payload[2] = { (uint8_t)(cpi & 0xFF), (uint8_t)((cpi >> 8) & 0xFF) };
    uint8_t req[64];
    build_fm_report(FM_CMD_DPI, payload, 2, req);
    ssize_t wr = write(fd, req, sizeof(req));
    close(fd);

    if (wr == sizeof(req)) {
        atomic_store(&g_state.hardware_cpi, cpi);
        return true;
    }
    return false;
}

bool hid_set_mouse_motion_sync(bool enabled) {
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info) || info.vid != FINALMOUSE_VID) return false;

    int fd = open(info.node, O_RDWR);
    if (fd < 0) return false;

    uint8_t payload[1] = { enabled ? 1 : 0 };
    uint8_t req[64];
    build_fm_report(FM_CMD_MOTION_SYNC, payload, 1, req);
    ssize_t wr = write(fd, req, sizeof(req));
    close(fd);
    return (wr == sizeof(req));
}

bool hid_set_mouse_polling_rate(int hz) {
    if (hz != 500 && hz != 1000 && hz != 2000 && hz != 4000 && hz != 8000) return false;
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info) || info.vid != FINALMOUSE_VID) return false;

    int fd = open(info.node, O_RDWR);
    if (fd < 0) return false;

    uint8_t payload[2] = { (uint8_t)(hz & 0xFF), (uint8_t)((hz >> 8) & 0xFF) };
    uint8_t req[64];
    build_fm_report(FM_CMD_POLLING_RATE, payload, 2, req);
    ssize_t wr = write(fd, req, sizeof(req));
    close(fd);
    return (wr == sizeof(req));
}
