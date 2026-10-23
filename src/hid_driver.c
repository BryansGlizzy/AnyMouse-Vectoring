#define _GNU_SOURCE
#include "core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

/* Attack Shark / GearHub Constants (OpenMouse-Project/mouse-protocol src/gearhub) */
#define GEARHUB_VID              0x3151
#define GEARHUB_PID_24G          0x402D
#define GEARHUB_REPORT_SIZE      64
#define GH_CMD_GET_OPTIONPARAM0  0xD3
#define GH_CMD_SET_OPTIONPARAM0  0x53
#define GH_DONGLE_SELECT_TARGET  0xF6
#define GH_DONGLE_GET_STATUS     0xF7
#define GH_DONGLE_NOTICE_READ    0xFC
#define GH_TARGET_MOUSE          0x05
#define GH_OPT0_REPORT_RATE      9

static const int FINALMOUSE_RATES[] = { 500, 1000, 2000, 4000, 8000, 0 };
static const int GEARHUB_RATES[]    = { 125, 250, 500, 1000, 2000, 4000, 8000, 0 };

/* GearHub report-rate codes are bit flags, not indices */
static const struct { int hz; uint8_t code; } GEARHUB_RATE_CODES[] = {
    { 8000, 129 }, { 4000, 130 }, { 2000, 132 }, { 1000, 1 }, { 500, 2 }, { 250, 4 }, { 125, 8 },
};

/* Serializes multi-step relay exchanges with the GearHub 2.4G receiver */
static pthread_mutex_t g_hid_mutex = PTHREAD_MUTEX_INITIALIZER;

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

/* Walk a HID report descriptor and report whether it declares an application
 * collection with the given usage page / usage (selects the vendor config interface). */
static bool descriptor_has_collection(const char *hidraw_name, uint16_t want_page, uint16_t want_usage) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/report_descriptor", hidraw_name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t d[4096];
    size_t n = fread(d, 1, sizeof(d), f);
    fclose(f);

    uint32_t page = 0, usage_page = 0, usage = 0;
    bool have_usage = false;
    size_t i = 0;
    while (i < n) {
        uint8_t b = d[i];
        if (b == 0xFE) { /* Long item */
            if (i + 1 >= n) break;
            i += 3 + d[i + 1];
            continue;
        }
        size_t sz = b & 0x03;
        if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        uint32_t val = 0;
        for (size_t k = 0; k < sz; k++) val |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + sz;

        switch (b & 0xFC) {
            case 0x04: /* Usage Page (global) */
                page = val;
                break;
            case 0x08: /* Usage (local); 4-byte form carries its own page */
                if (!have_usage) {
                    usage_page = (sz == 4) ? (val >> 16) : page;
                    usage = val & 0xFFFF;
                    have_usage = true;
                }
                break;
            case 0xA0: /* Collection */
                if (have_usage && usage_page == want_page && usage == want_usage) return true;
                have_usage = false;
                break;
            case 0x80: case 0x90: case 0xB0: case 0xC0: /* Input/Output/Feature/End Collection */
                have_usage = false;
                break;
            default:
                break;
        }
    }
    return false;
}

static void fill_known_device(mouse_hw_status_t *out, uint16_t vid, uint16_t pid,
                              const char *dev_node, bool acc) {
    memset(out, 0, sizeof(*out));
    out->vid = vid;
    out->pid = pid;
    out->accessible = acc;
    snprintf(out->node, sizeof(out->node), "%s", dev_node);
    const int *rates;
    if (vid == FINALMOUSE_VID) {
        snprintf(out->brand, sizeof(out->brand), "Finalmouse");
        snprintf(out->model, sizeof(out->model), "UltralightX");
        rates = FINALMOUSE_RATES;
    } else {
        snprintf(out->brand, sizeof(out->brand), "Attack Shark");
        snprintf(out->model, sizeof(out->model), "R3 2.4G Wireless");
        rates = GEARHUB_RATES;
    }
    for (int i = 0; rates[i] && i < 7; i++) out->supported_rates[i] = rates[i];
}

/*
 * Locate the vendor configuration hidraw node for the mouse the RT engine holds.
 * If a generic (unsupported) mouse is grabbed there is nothing to control, so we
 * deliberately do not fall back to some other known mouse that happens to be plugged in.
 */
static bool find_mouse_hidraw_node(mouse_hw_status_t *out_info) {
    uint16_t want_vid = (uint16_t)atomic_load(&g_state.active_vid);
    uint16_t want_pid = (uint16_t)atomic_load(&g_state.active_pid);
    bool grabbed = atomic_load(&g_state.evdev_grabbed);
    if (grabbed && want_vid && !devices_is_hw_supported(want_vid, want_pid)) return false;

    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return false;

    int best_rank = 0;
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

        if (!devices_is_hw_supported(vid, pid)) continue;
        if (grabbed && want_vid && (vid != want_vid || pid != want_pid)) continue;

        /* Rank: vendor config collection beats any other interface; Finalmouse preferred when ungrabbed */
        bool cfg_iface = (vid == FINALMOUSE_VID)
                       ? descriptor_has_collection(ent->d_name, 0xFF00, 0x0001)
                       : descriptor_has_collection(ent->d_name, 0xFFFF, 0x0002);
        if (vid == GEARHUB_VID && !cfg_iface) continue; /* Shark motion/keyboard endpoints can't be configured */
        int rank = (cfg_iface ? 4 : 1) + (vid == FINALMOUSE_VID ? 2 : 0);
        if (rank <= best_rank) continue;

        char dev_node[64];
        snprintf(dev_node, sizeof(dev_node), "/dev/%.40s", ent->d_name);
        fill_known_device(out_info, vid, pid, dev_node, access(dev_node, R_OK | W_OK) == 0);
        best_rank = rank;
    }

    closedir(dir);
    return best_rank > 0;
}

/* ---- GearHub 2.4G relay: feature report 0x00, 64 bytes, F6/F7/FC dongle handshakes ---- */

static bool gh_send(int fd, const uint8_t *bytes, size_t len) {
    uint8_t buf[1 + GEARHUB_REPORT_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00; /* Report ID (interface has unnumbered reports) */
    memcpy(&buf[1], bytes, len > GEARHUB_REPORT_SIZE ? GEARHUB_REPORT_SIZE : len);
    return ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) >= 0;
}

static bool gh_read(int fd, uint8_t out[GEARHUB_REPORT_SIZE]) {
    uint8_t buf[1 + GEARHUB_REPORT_SIZE];
    memset(buf, 0, sizeof(buf));
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf) < 0) return false;
    memcpy(out, &buf[1], GEARHUB_REPORT_SIZE);
    return true;
}

/* Bit7 checksum: byte[7] = 255 - (sum(bytes 0..6) & 0xFF) */
static void gh_stamp_checksum(uint8_t *cmd) {
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum = (uint8_t)(sum + cmd[i]);
    cmd[7] = (uint8_t)(255 - sum);
}

static bool gh_select_mouse(int fd) {
    const uint8_t sel[2] = { GH_DONGLE_SELECT_TARGET, GH_TARGET_MOUSE };
    if (!gh_send(fd, sel, sizeof(sel))) return false;
    usleep(50000);
    return true;
}

static bool gh_wait_ready(int fd, bool for_send) {
    for (int attempt = 0; attempt < 8; attempt++) {
        usleep(100000);
        const uint8_t st = GH_DONGLE_GET_STATUS;
        uint8_t r[GEARHUB_REPORT_SIZE];
        if (!gh_send(fd, &st, 1)) return false;
        usleep(10000);
        if (!gh_read(fd, r)) return false;
        if (for_send ? (r[5] == 1) : (r[0] == 1)) return true;
    }
    return false;
}

static bool gh_get(int fd, uint8_t cmd_id, uint8_t out[GEARHUB_REPORT_SIZE]) {
    uint8_t cmd[9] = { cmd_id };
    gh_stamp_checksum(cmd);
    if (!gh_select_mouse(fd) || !gh_wait_ready(fd, true)) return false;
    if (!gh_send(fd, cmd, sizeof(cmd)) || !gh_wait_ready(fd, false)) return false;
    const uint8_t notice = GH_DONGLE_NOTICE_READ;
    if (!gh_send(fd, &notice, 1)) return false;
    usleep(10000);
    if (!gh_read(fd, out)) return false;

    bool all_zero = true;
    for (int i = 0; i < GEARHUB_REPORT_SIZE; i++) if (out[i]) { all_zero = false; break; }
    return !all_zero && out[0] == cmd_id;
}

/* The receiver occasionally answers a relay exchange with an empty or stale buffer; retry once */
static bool gh_get_retry(int fd, uint8_t cmd_id, uint8_t out[GEARHUB_REPORT_SIZE]) {
    if (gh_get(fd, cmd_id, out)) return true;
    usleep(100000);
    return gh_get(fd, cmd_id, out);
}

static int gh_decode_rate(uint8_t code) {
    for (size_t i = 0; i < sizeof(GEARHUB_RATE_CODES) / sizeof(GEARHUB_RATE_CODES[0]); i++) {
        if (GEARHUB_RATE_CODES[i].code == code) return GEARHUB_RATE_CODES[i].hz;
    }
    return 0;
}

static int gh_read_polling_rate(const char *node) {
    int fd = open(node, O_RDWR);
    if (fd < 0) return 0;
    uint8_t opt0[GEARHUB_REPORT_SIZE];
    int hz = gh_get_retry(fd, GH_CMD_GET_OPTIONPARAM0, opt0) ? gh_decode_rate(opt0[GH_OPT0_REPORT_RATE]) : 0;
    close(fd);
    return hz;
}

/* Read-modify-write of OPTIONPARAM0, mirroring upstream patchOptionParam0() */
static bool gh_write_polling_rate(const char *node, int hz) {
    uint8_t code = 0;
    for (size_t i = 0; i < sizeof(GEARHUB_RATE_CODES) / sizeof(GEARHUB_RATE_CODES[0]); i++) {
        if (GEARHUB_RATE_CODES[i].hz == hz) code = GEARHUB_RATE_CODES[i].code;
    }
    if (!code) return false;

    int fd = open(node, O_RDWR);
    if (fd < 0) return false;

    bool ok = false;
    uint8_t current[GEARHUB_REPORT_SIZE];
    /* Never write a settings block we could not read back intact */
    if (gh_get_retry(fd, GH_CMD_GET_OPTIONPARAM0, current) && gh_decode_rate(current[GH_OPT0_REPORT_RATE]) != 0) {
        uint8_t block[GEARHUB_REPORT_SIZE];
        memset(block, 0, sizeof(block));
        memcpy(&block[8], &current[8], GEARHUB_REPORT_SIZE - 8);
        block[0] = GH_CMD_SET_OPTIONPARAM0;
        block[17] = 255;
        block[18] = 8;
        block[GH_OPT0_REPORT_RATE] = code;
        gh_stamp_checksum(block);
        if (gh_select_mouse(fd) && gh_wait_ready(fd, true) && gh_send(fd, block, sizeof(block))) {
            usleep(50000);
            uint8_t verify[GEARHUB_REPORT_SIZE];
            ok = gh_get_retry(fd, GH_CMD_GET_OPTIONPARAM0, verify) && verify[GH_OPT0_REPORT_RATE] == code;
        }
    }
    close(fd);
    return ok;
}

bool hid_query_active_mouse(mouse_hw_status_t *out_status) {
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info)) {
        memset(out_status, 0, sizeof(*out_status));
        return false;
    }

    if (info.vid != FINALMOUSE_VID) {
        /* GearHub / Attack Shark: polling rate via the 2.4G relay; CPI write not implemented yet */
        if (info.accessible) {
            pthread_mutex_lock(&g_hid_mutex);
            info.polling_rate_hz = gh_read_polling_rate(info.node);
            pthread_mutex_unlock(&g_hid_mutex);
        }
        info.connected = info.accessible;
        info.dpi = atomic_load(&g_state.hardware_cpi);
        *out_status = info;
        pthread_mutex_lock(&g_state.telemetry_mutex);
        g_state.hw_status = info;
        pthread_mutex_unlock(&g_state.telemetry_mutex);
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
    mouse_hw_status_t info;
    if (!find_mouse_hidraw_node(&info)) return false;

    bool allowed = false;
    for (int i = 0; info.supported_rates[i]; i++) {
        if (info.supported_rates[i] == hz) allowed = true;
    }
    if (!allowed) return false;

    if (info.vid == GEARHUB_VID) {
        pthread_mutex_lock(&g_hid_mutex);
        bool ok = gh_write_polling_rate(info.node, hz);
        pthread_mutex_unlock(&g_hid_mutex);
        return ok;
    }

    int fd = open(info.node, O_RDWR);
    if (fd < 0) return false;

    uint8_t payload[2] = { (uint8_t)(hz & 0xFF), (uint8_t)((hz >> 8) & 0xFF) };
    uint8_t req[64];
    build_fm_report(FM_CMD_POLLING_RATE, payload, 2, req);
    ssize_t wr = write(fd, req, sizeof(req));
    close(fd);
    return (wr == sizeof(req));
}
