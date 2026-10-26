#define _GNU_SOURCE
#include "core.h"
#include "../common/amv_dongle_proto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/*
 * AnyMouse hardware dongle link.
 *
 * Identity comes from the protocol, not from USB IDs: any hidraw node whose report
 * descriptor declares the AnyMouse vendor collection (AMV_VENDOR_USAGE_PAGE) is a
 * candidate, and it counts as a dongle only once it answers the HELLO handshake with
 * AMV_HELLO_IDENT and our random nonce. Firmware older than HELLO (0.3.x) is still
 * recognised by a well-formed GET_INFO reply.
 *
 * Guard: the mouse interface that shares a USB device with a candidate is never
 * auto-grabbed by the RT engine while the candidate is unverified, unreachable or
 * verified as vectoring. Only a candidate that fails the handshake (an unrelated device
 * on the same usage page) releases its mouse. Failing safe avoids scaling motion twice.
 *
 * Sync: the dongle owns the weights. On link the engine adopts them; afterwards local
 * changes (GUI, presets, calibration) are pushed with SET_WEIGHTS at up to 50 Hz, and
 * EVENT_STATE frames from other clients (the web page) are applied to g_state.
 */

#define PROBE_TIMEOUT_MS    300
#define REQUEST_TIMEOUT_MS  300
#define SCAN_PERIOD_MS      1000
#define SYNC_PERIOD_MS      20
#define MAX_CANDIDATES      8
#define MAX_REJECTED        8
#define MAX_MICE            8
#define MICE_PERIOD_MS      2000
#define MOUSE_SET_TIMEOUT_MS 3000

/* Overridable for tests */
static const char *s_hidraw_class = "/sys/class/hidraw";
static const char *s_dev_dir = "/dev";

typedef struct {
    char name[32];          /* hidrawN */
    char key[PATH_MAX];     /* sysfs HID device dir (…/0003:VVVV:PPPP.NNNN): unique until reboot */
    char usb_parent[128];
} candidate_t;

typedef struct {
    int      fd;
    uint8_t  seq;
    bool     sync;              /* daemon link: apply events to g_state (false for one-shot CLI) */
    uint32_t gen;
    float    synced[8];         /* weights last agreed with the dongle */
    float    refused[8];        /* local weights the dongle (or our validator) refused: not resent */
    bool     has_refused;
    float    inflight[8];       /* weights of a SET_WEIGHTS awaiting its reply */
    bool     has_inflight;
    bool     mice_dirty;        /* an EVENT_MOUSE arrived: refetch the list */
} link_t;

typedef enum { PROBE_GONE, PROBE_SILENT, PROBE_FOREIGN, PROBE_INCOMPATIBLE, PROBE_OK } probe_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static dongle_status_t s_status;                 /* guarded by s_mtx */
static char s_linked_key[PATH_MAX];              /* candidate currently linked or incompatible */
static char s_rejected[MAX_REJECTED][PATH_MAX];  /* candidates that failed the handshake */
static int  s_rejected_n;
static dongle_mouse_t s_mice[MAX_MICE];          /* guarded by s_mtx */
static int  s_nmice = -1;                        /* -1: unknown / unsupported */

/* One pending mouse write, executed by the dongle thread (it owns the hidraw link) */
static pthread_mutex_t s_job_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_job_cv = PTHREAD_COND_INITIALIZER;
static struct { bool pending, done; uint8_t index, cmd; uint16_t value; int status; } s_job;

static uint64_t dongle_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Same rule as the firmware (amv_ctrl.c). Checks the bits for NaN/Inf because the
 * engine is built with -ffast-math, under which isfinite() may be folded to true. */
static bool weight_ok(float w) {
    uint32_t b;
    memcpy(&b, &w, sizeof(b));
    if ((b & 0x7F800000u) == 0x7F800000u) return false;
    return w >= (float)AMV_WEIGHT_MIN && w <= (float)AMV_WEIGHT_MAX;
}

static bool weights_ok(const float w[8]) {
    for (int i = 0; i < 8; i++) if (!weight_ok(w[i])) return false;
    return true;
}

/* ---- discovery ---- */

static int scan_candidates(candidate_t *out, int max) {
    DIR *dir = opendir(s_hidraw_class);
    if (!dir) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
        if (!hid_descriptor_has_collection(ent->d_name, AMV_VENDOR_USAGE_PAGE, AMV_VENDOR_USAGE)) continue;
        char link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/%s/device", s_hidraw_class, ent->d_name);
        candidate_t *c = &out[n];
        if (!realpath(link, c->key)) continue;
        snprintf(c->name, sizeof(c->name), "%.31s", ent->d_name);
        devices_usb_parent_of(c->key, c->usb_parent, sizeof(c->usb_parent));
        n++;
    }
    closedir(dir);
    return n;
}

static bool is_rejected_locked(const char *key) {
    for (int i = 0; i < s_rejected_n; i++) if (strcmp(s_rejected[i], key) == 0) return true;
    return false;
}

static void reject(const char *key) {
    pthread_mutex_lock(&s_mtx);
    if (!is_rejected_locked(key)) {
        if (s_rejected_n == MAX_REJECTED) {
            memmove(s_rejected[0], s_rejected[1], sizeof(s_rejected[0]) * (MAX_REJECTED - 1));
            s_rejected_n--;
        }
        snprintf(s_rejected[s_rejected_n++], PATH_MAX, "%s", key);
    }
    pthread_mutex_unlock(&s_mtx);
}

/* A candidate holds back its mouse unless it failed the handshake, or it is a verified
 * dongle that says its mouse output is not vectored. */
static bool candidate_guards(const candidate_t *c) {
    pthread_mutex_lock(&s_mtx);
    bool guards = !is_rejected_locked(c->key);
    if (guards && strcmp(s_linked_key, c->key) == 0 && s_status.link == DONGLE_LINKED && !s_status.vectoring) {
        guards = false;
    }
    pthread_mutex_unlock(&s_mtx);
    return guards;
}

bool dongle_guards_usb_device(const char *usb_parent) {
    if (!usb_parent || !usb_parent[0]) return false;
    candidate_t c[MAX_CANDIDATES];
    int n = scan_candidates(c, MAX_CANDIDATES);
    for (int i = 0; i < n; i++) {
        if (strcmp(c[i].usb_parent, usb_parent) == 0 && candidate_guards(&c[i])) return true;
    }
    return false;
}

bool dongle_guard_active(void) {
    candidate_t c[MAX_CANDIDATES];
    int n = scan_candidates(c, MAX_CANDIDATES);
    for (int i = 0; i < n; i++) if (candidate_guards(&c[i])) return true;
    return false;
}

bool dongle_get_status(dongle_status_t *out) {
    pthread_mutex_lock(&s_mtx);
    *out = s_status;
    pthread_mutex_unlock(&s_mtx);
    return out->link == DONGLE_LINKED;
}

static void publish(const dongle_status_t *st, const char *key) {
    pthread_mutex_lock(&s_mtx);
    s_status = *st;
    snprintf(s_linked_key, sizeof(s_linked_key), "%s", key ? key : "");
    pthread_mutex_unlock(&s_mtx);
}

/* ---- transport: 64-byte frames, no report IDs ---- */

static void random_bytes(uint8_t *out, size_t n) {
    if (getrandom(out, n, 0) == (ssize_t)n) return;
    uint64_t t = dongle_now_ms() ^ (uint64_t)getpid();
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(t >> ((i % 8) * 8)) ^ (uint8_t)(rand() & 0xFF);
}

/* Other clients count from 1; starting elsewhere makes a mixed-up reply unlikely */
static void start_seq(link_t *l) {
    random_bytes(&l->seq, 1);
}

static uint8_t next_seq(link_t *l) {
    l->seq++;
    if (l->seq == 0) l->seq = 1;   /* seq 0 is reserved for events */
    return l->seq;
}

static bool frame_send(link_t *l, uint8_t cmd, uint8_t seq, const void *pl, uint8_t len) {
    uint8_t buf[1 + AMV_FRAME_SIZE] = { 0 };   /* hidraw: leading report ID 0 */
    amv_hdr_t *h = (amv_hdr_t *)(buf + 1);
    h->magic0 = AMV_MAGIC0;
    h->magic1 = AMV_MAGIC1;
    h->ver = AMV_PROTO_VERSION;
    h->cmd = cmd;
    h->seq = seq;
    h->len = len;
    if (len) memcpy(buf + 1 + AMV_HEADER_SIZE, pl, len);
    return write(l->fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
}

static void apply_weights_to_engine(const float w[8]) {
    bool changed = false;
    for (int i = 0; i < 8; i++) {
        if (atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed) != w[i]) {
            atomic_store_explicit(&g_state.octant_weights[i], w[i], memory_order_relaxed);
            changed = true;
        }
    }
    if (changed) atomic_fetch_add(&g_state.dongle_weights_rev, 1U);
}

/* The dongle's state is authoritative: record it and, on the daemon link, mirror it into g_state */
static void adopt_state(link_t *l, const amv_state_payload_t *p, bool to_engine) {
    float w[8];
    memcpy(w, p->w, sizeof(w));
    l->gen = p->gen;
    memcpy(l->synced, w, sizeof(w));
    if (l->sync && to_engine) apply_weights_to_engine(w);
    pthread_mutex_lock(&s_mtx);
    s_status.gen = p->gen;
    s_status.bypass = p->bypass != 0;
    s_status.active_slot = p->active_slot;
    memcpy(s_status.weights, w, sizeof(w));
    pthread_mutex_unlock(&s_mtx);
}

static void handle_event(link_t *l, const uint8_t *f) {
    const amv_hdr_t *h = (const amv_hdr_t *)f;
    if (h->len < sizeof(amv_state_payload_t)) return;
    amv_state_payload_t p;
    memcpy(&p, f + AMV_HEADER_SIZE, sizeof(p));
    /* The echo of our own in-flight SET_WEIGHTS must not overwrite newer local edits */
    bool own_echo = l->has_inflight && memcmp(p.w, l->inflight, sizeof(l->inflight)) == 0;
    bool same = memcmp(p.w, l->synced, sizeof(l->synced)) == 0;
    adopt_state(l, &p, !own_echo && !same);
}

/* Read one frame. 1: got one, 0: nothing pending, -1: device gone. */
static int frame_read(link_t *l, uint8_t f[AMV_FRAME_SIZE], ssize_t *n_out) {
    ssize_t n = read(l->fd, f, AMV_FRAME_SIZE);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (n == 0) return -1;
    *n_out = n;
    return 1;
}

static bool is_protocol_frame(const uint8_t *f, ssize_t n) {
    return n >= AMV_HEADER_SIZE && f[0] == AMV_MAGIC0 && f[1] == AMV_MAGIC1;
}

/* Wait for the reply to (cmd, seq); events seen meanwhile are applied. Frames without the
 * AnyMouse magic set *foreign. 1: reply in rep, 0: timeout, -1: device gone. */
static int wait_reply(link_t *l, uint8_t cmd, uint8_t seq, int timeout_ms,
                      uint8_t rep[AMV_FRAME_SIZE], bool *foreign) {
    uint64_t deadline = dongle_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = dongle_now_ms();
        if (now >= deadline) return 0;
        struct pollfd p = { .fd = l->fd, .events = POLLIN };
        int r = poll(&p, 1, (int)(deadline - now));
        if (r < 0 && errno != EINTR) return -1;
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        uint8_t f[AMV_FRAME_SIZE];
        ssize_t n = 0;
        int got;
        while ((got = frame_read(l, f, &n)) > 0) {
            if (!is_protocol_frame(f, n)) {
                if (foreign) *foreign = true;
                continue;
            }
            const amv_hdr_t *h = (const amv_hdr_t *)f;
            if (h->cmd == AMV_CMD_EVENT_STATE) {
                handle_event(l, f);
            } else if (h->cmd == AMV_CMD_EVENT_MOUSE) {
                l->mice_dirty = true;
            } else if (h->cmd == cmd && h->seq == seq) {
                memset(rep, 0, AMV_FRAME_SIZE);
                memcpy(rep, f, (size_t)n);
                return 1;
            }
            /* Otherwise: a reply meant for another client on the same interface */
        }
        if (got < 0) return -1;
    }
}

static int request(link_t *l, uint8_t cmd, const void *pl, uint8_t len, int timeout_ms,
                   uint8_t rep[AMV_FRAME_SIZE], bool *foreign) {
    uint8_t seq = next_seq(l);
    if (!frame_send(l, cmd, seq, pl, len)) return -1;
    return wait_reply(l, cmd, seq, timeout_ms, rep, foreign);
}

/* Fill the request from a state-returning reply (OK or STALE). */
static bool state_from_reply(const uint8_t *rep, amv_state_payload_t *p) {
    const amv_hdr_t *h = (const amv_hdr_t *)rep;
    if (h->len < sizeof(*p)) return false;
    memcpy(p, rep + AMV_HEADER_SIZE, sizeof(*p));
    return true;
}

/* ---- handshake ---- */

static probe_t handshake(link_t *l, dongle_status_t *st) {
    amv_hello_req_t req;
    random_bytes(req.nonce, sizeof(req.nonce));
    uint8_t rep[AMV_FRAME_SIZE];
    bool foreign = false;
    int r = request(l, AMV_CMD_HELLO, &req, sizeof(req), PROBE_TIMEOUT_MS, rep, &foreign);
    if (r < 0) return PROBE_GONE;
    if (r == 0) return foreign ? PROBE_FOREIGN : PROBE_SILENT;

    const amv_hdr_t *h = (const amv_hdr_t *)rep;
    st->proto_ver = h->ver;
    if (h->ver != AMV_PROTO_VERSION) return PROBE_INCOMPATIBLE;

    if (h->status == AMV_ST_BAD_CMD) {
        /* Firmware older than HELLO: a well-formed GET_INFO reply still proves the protocol */
        r = request(l, AMV_CMD_GET_INFO, NULL, 0, PROBE_TIMEOUT_MS, rep, &foreign);
        if (r < 0) return PROBE_GONE;
        if (r == 0) return PROBE_SILENT;
        amv_info_payload_t info;
        if (h->status != AMV_ST_OK || h->len < sizeof(info)) return PROBE_FOREIGN;   /* h now reads this reply */
        memcpy(&info, rep + AMV_HEADER_SIZE, sizeof(info));
        if (info.proto_ver != AMV_PROTO_VERSION) return PROBE_INCOMPATIBLE;
        st->legacy = true;
        st->vectoring = true;   /* every pre-HELLO build vectors its mouse output */
        st->caps = AMV_CAP_VECTORED_MOUSE;
        st->fw[0] = info.fw_major; st->fw[1] = info.fw_minor; st->fw[2] = info.fw_patch;
        memcpy(st->mac, info.mac, sizeof(st->mac));
        return PROBE_OK;
    }

    amv_hello_rep_t hello;
    if (h->status != AMV_ST_OK || h->len < sizeof(hello)) return PROBE_FOREIGN;
    memcpy(&hello, rep + AMV_HEADER_SIZE, sizeof(hello));
    if (hello.ident != AMV_HELLO_IDENT || memcmp(hello.nonce, req.nonce, sizeof(req.nonce)) != 0) {
        return PROBE_FOREIGN;
    }
    if (hello.proto_ver != AMV_PROTO_VERSION) return PROBE_INCOMPATIBLE;
    st->legacy = false;
    st->caps = hello.caps;
    st->vectoring = (hello.caps & AMV_CAP_VECTORED_MOUSE) != 0;
    st->fw[0] = hello.fw_major; st->fw[1] = hello.fw_minor; st->fw[2] = hello.fw_patch;
    memcpy(st->mac, hello.mac, sizeof(st->mac));
    return PROBE_OK;
}

static int fetch_state(link_t *l, bool to_engine) {
    uint8_t rep[AMV_FRAME_SIZE];
    int r = request(l, AMV_CMD_GET_WEIGHTS, NULL, 0, REQUEST_TIMEOUT_MS, rep, NULL);
    if (r <= 0) return r;
    amv_state_payload_t p;
    if (((amv_hdr_t *)rep)->status != AMV_ST_OK || !state_from_reply(rep, &p)) return 0;
    adopt_state(l, &p, to_engine);
    return 1;
}

/* ---- weight sync ---- */

/* Push local weight changes. 0: nothing to do or retry later, -1: device gone. */
static int sync_step(link_t *l) {
    float local[8];
    for (int i = 0; i < 8; i++) local[i] = atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed);
    if (memcmp(local, l->synced, sizeof(local)) == 0) return 0;
    if (l->has_refused && memcmp(local, l->refused, sizeof(local)) == 0) return 0;
    if (!weights_ok(local)) {
        memcpy(l->refused, local, sizeof(local));
        l->has_refused = true;
        fprintf(stderr, "[DONGLE] Not sending weights outside %.2f..%.2f to the dongle\n",
                AMV_WEIGHT_MIN, AMV_WEIGHT_MAX);
        return 0;
    }

    amv_set_weights_req_t req = { .base_gen = l->gen };
    memcpy(req.w, local, sizeof(local));
    memcpy(l->inflight, local, sizeof(local));
    l->has_inflight = true;
    uint8_t rep[AMV_FRAME_SIZE];
    int r = request(l, AMV_CMD_SET_WEIGHTS, &req, sizeof(req), REQUEST_TIMEOUT_MS, rep, NULL);
    l->has_inflight = false;
    if (r <= 0) return r;

    const amv_hdr_t *h = (const amv_hdr_t *)rep;
    amv_state_payload_t p;
    if (h->status == AMV_ST_OK && state_from_reply(rep, &p)) {
        /* The engine already holds these (or newer) values: don't write them back */
        adopt_state(l, &p, false);
    } else if (h->status == AMV_ST_STALE && state_from_reply(rep, &p)) {
        /* Someone else changed the dongle first: its state wins */
        adopt_state(l, &p, true);
    } else {
        memcpy(l->refused, local, sizeof(local));
        l->has_refused = true;
        fprintf(stderr, "[DONGLE] Dongle refused the weights (status %u)\n", h->status);
    }
    return 0;
}

/* Apply queued events without waiting. -1: device gone. */
static int drain_events(link_t *l) {
    uint8_t f[AMV_FRAME_SIZE];
    ssize_t n = 0;
    int got;
    while ((got = frame_read(l, f, &n)) > 0) {
        if (!is_protocol_frame(f, n)) continue;
        uint8_t cmd = ((const amv_hdr_t *)f)->cmd;
        if (cmd == AMV_CMD_EVENT_STATE) handle_event(l, f);
        else if (cmd == AMV_CMD_EVENT_MOUSE) l->mice_dirty = true;
    }
    return got;
}

/* ---- mice behind the dongle ---- */

static bool same_mice(const dongle_mouse_t *a, int na, const dongle_mouse_t *b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        amv_mouse_info_t x = a[i].info, y = b[i].info;
        x.measured_hz = y.measured_hz = 0;      /* changes all the time; not worth a GUI refresh */
        if (memcmp(&x, &y, sizeof x) != 0 || a[i].has_caps != b[i].has_caps) return false;
    }
    return true;
}

static void publish_mice(const dongle_mouse_t *m, int n) {
    pthread_mutex_lock(&s_mtx);
    bool changed = !same_mice(s_mice, s_nmice < 0 ? 0 : s_nmice, m, n < 0 ? 0 : n) || (s_nmice < 0) != (n < 0);
    if (n > 0) memcpy(s_mice, m, sizeof(m[0]) * (size_t)n);
    s_nmice = n;
    pthread_mutex_unlock(&s_mtx);
    if (changed) atomic_fetch_add(&g_state.dongle_mice_rev, 1U);
}

/* Read the whole list (and caps for known mice). -1: link gone; 0: done (list published). */
static int fetch_mice(link_t *l) {
    dongle_mouse_t m[MAX_MICE];
    int n = 0;
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        uint8_t rep[AMV_FRAME_SIZE];
        int r = request(l, AMV_CMD_GET_MOUSE, &i, 1, REQUEST_TIMEOUT_MS, rep, NULL);
        if (r < 0) return -1;
        if (r == 0) return 0;                          /* keep the previous list */
        const amv_hdr_t *h = (const amv_hdr_t *)rep;
        if (h->status == AMV_ST_BAD_CMD) { publish_mice(NULL, -1); return 0; }   /* firmware < 0.5.0 */
        if (h->status != AMV_ST_OK || h->len < sizeof(amv_mouse_info_t)) break;
        memset(&m[n], 0, sizeof m[n]);
        memcpy(&m[n].info, rep + AMV_HEADER_SIZE, sizeof m[n].info);
        m[n].info.name[sizeof m[n].info.name - 1] = '\0';
        if (m[n].info.tier == AMV_TIER_KNOWN) {
            r = request(l, AMV_CMD_GET_MOUSE_CAPS, &i, 1, REQUEST_TIMEOUT_MS, rep, NULL);
            if (r < 0) return -1;
            h = (const amv_hdr_t *)rep;
            if (r == 1 && h->status == AMV_ST_OK && h->len >= sizeof(amv_mouse_caps_t)) {
                memcpy(&m[n].caps, rep + AMV_HEADER_SIZE, sizeof m[n].caps);
                m[n].caps.firmware[sizeof m[n].caps.firmware - 1] = '\0';
                m[n].has_caps = true;
            }
        }
        n++;
    }
    publish_mice(m, n);
    l->mice_dirty = false;
    return 0;
}

int dongle_get_mice(dongle_mouse_t *out, int max) {
    pthread_mutex_lock(&s_mtx);
    int n = s_status.link == DONGLE_LINKED ? s_nmice : -1;
    if (n > max) n = max;
    if (n > 0) memcpy(out, s_mice, sizeof(out[0]) * (size_t)n);
    pthread_mutex_unlock(&s_mtx);
    return n;
}

int dongle_mouse_set(uint8_t index, bool cpi, uint16_t value) {
    dongle_status_t st;
    if (!dongle_get_status(&st)) return -1;
    pthread_mutex_lock(&s_job_mtx);
    while (s_job.pending) pthread_cond_wait(&s_job_cv, &s_job_mtx);
    s_job.pending = true;
    s_job.done = false;
    s_job.index = index;
    s_job.cmd = cpi ? AMV_CMD_SET_MOUSE_CPI : AMV_CMD_SET_MOUSE_POLLING;
    s_job.value = value;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += (MOUSE_SET_TIMEOUT_MS / 1000) + 2;
    int rc = 0;
    while (!s_job.done && rc == 0) rc = pthread_cond_timedwait(&s_job_cv, &s_job_mtx, &dl);
    int status = s_job.done ? s_job.status : -1;
    s_job.pending = false;
    pthread_cond_broadcast(&s_job_cv);
    pthread_mutex_unlock(&s_job_mtx);
    return status;
}

/* Dongle thread: run a pending mouse write. -1: link gone. */
static int run_job(link_t *l) {
    pthread_mutex_lock(&s_job_mtx);
    bool todo = s_job.pending && !s_job.done;
    amv_mouse_set_req_t req = { .index = s_job.index, .value = s_job.value };
    uint8_t cmd = s_job.cmd;
    pthread_mutex_unlock(&s_job_mtx);
    if (!todo) return 0;
    uint8_t rep[AMV_FRAME_SIZE];
    int r = request(l, cmd, &req, sizeof req, MOUSE_SET_TIMEOUT_MS, rep, NULL);
    int status = r == 1 ? ((const amv_hdr_t *)rep)->status : -1;
    if (r >= 0) fetch_mice(l);
    pthread_mutex_lock(&s_job_mtx);
    s_job.status = status;
    s_job.done = true;
    pthread_cond_broadcast(&s_job_cv);
    pthread_mutex_unlock(&s_job_mtx);
    return r < 0 ? -1 : 0;
}

static const char *DRIVER_BRAND[] = { "", "Razer", "Finalmouse", "Attack Shark" };

bool dongle_fill_hw_status(mouse_hw_status_t *out) {
    dongle_mouse_t m[MAX_MICE];
    int n = dongle_get_mice(m, MAX_MICE);
    for (int i = 0; i < n; i++) {
        const amv_mouse_info_t *x = &m[i].info;
        if (x->tier == AMV_TIER_IGNORED) continue;
        memset(out, 0, sizeof(*out));
        out->connected = out->accessible = out->via_dongle = true;
        out->tier = x->tier;
        out->vid = x->vid;
        out->pid = x->pid;
        out->dpi = x->cpi;
        out->polling_rate_hz = x->device_hz ? x->device_hz : x->nominal_hz;
        out->battery_pct = x->battery == 0xFF ? 0 : x->battery;
        out->battery_mv = 0;
        out->can_set_cpi = x->tier == AMV_TIER_KNOWN && (x->flags & AMV_MF_CAN_SET_CPI);
        out->can_set_rate = x->tier == AMV_TIER_KNOWN && (x->flags & AMV_MF_CAN_SET_RATE);
        snprintf(out->brand, sizeof(out->brand), "%s", x->driver < 4 ? DRIVER_BRAND[x->driver] : "");
        snprintf(out->model, sizeof(out->model), "%.31s", x->name);
        snprintf(out->node, sizeof(out->node), "via AnyMouse dongle, %s",
                 x->tier == AMV_TIER_KNOWN ? "known protocol" : x->tier == AMV_TIER_BOOT ? "boot fallback" : "generic HID");
        if (m[i].has_caps) {
            snprintf(out->mouse_fw, sizeof(out->mouse_fw), "%s", m[i].caps.firmware);
            for (int k = 0; k < m[i].caps.n_rates && k < 7; k++) out->supported_rates[k] = m[i].caps.rates[k];
        }
        return true;
    }
    return false;
}

/* Index of the first mouse the dongle uses, or -1 */
static int first_mouse_index(void) {
    dongle_mouse_t m[MAX_MICE];
    int n = dongle_get_mice(m, MAX_MICE);
    for (int i = 0; i < n; i++) if (m[i].info.tier != AMV_TIER_IGNORED) return m[i].info.index;
    return -1;
}

int dongle_first_mouse_set(bool cpi, uint16_t value) {
    int i = first_mouse_index();
    return i < 0 ? -1 : dongle_mouse_set((uint8_t)i, cpi, value);
}

/* ---- link lifecycle ---- */

static void format_status_line(const dongle_status_t *st, char *out, size_t sz) {
    snprintf(out, sz, "fw %u.%u.%u, protocol v%u, id %02x%02x%02x%02x%02x%02x%s",
             st->fw[0], st->fw[1], st->fw[2], st->proto_ver,
             st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5],
             st->legacy ? " (pre-HELLO firmware: please update)" : "");
}

/* Open the first eligible candidate and run the handshake. Leaves l->fd open only when linked. */
static void try_link(link_t *l, uint64_t *next_scan) {
    candidate_t c[MAX_CANDIDATES];
    int n = scan_candidates(c, MAX_CANDIDATES);
    dongle_status_t st = { 0 };
    const candidate_t *pick = NULL;
    for (int i = 0; i < n && !pick; i++) {
        pthread_mutex_lock(&s_mtx);
        bool skip = is_rejected_locked(c[i].key) ||
                    (strcmp(s_linked_key, c[i].key) == 0 && s_status.link == DONGLE_INCOMPATIBLE);
        pthread_mutex_unlock(&s_mtx);
        if (!skip) pick = &c[i];
    }
    if (!pick) {
        /* Keep reporting an incompatible dongle while it is still plugged in */
        dongle_status_t cur;
        dongle_get_status(&cur);
        bool still_there = false;
        for (int i = 0; i < n; i++) if (strcmp(c[i].key, s_linked_key) == 0) still_there = true;
        if (!(cur.link == DONGLE_INCOMPATIBLE && still_there)) publish(&st, NULL);
        return;
    }

    snprintf(st.node, sizeof(st.node), "%s/%s", s_dev_dir, pick->name);
    int fd = open(st.node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        st.link = DONGLE_NO_ACCESS;
        snprintf(st.detail, sizeof(st.detail), "cannot open %s: %s (run: sudo ./anymouse --install-udev)",
                 st.node, strerror(errno));
        publish(&st, pick->key);
        return;
    }
    l->fd = fd;
    start_seq(l);
    l->has_refused = false;
    l->has_inflight = false;

    probe_t pr = handshake(l, &st);
    if (pr == PROBE_OK && fetch_state(l, true) <= 0) pr = PROBE_SILENT;

    char line[160];
    switch (pr) {
    case PROBE_OK:
        st.link = DONGLE_LINKED;
        format_status_line(&st, line, sizeof(line));
        snprintf(st.detail, sizeof(st.detail), "%s", line);
        /* adopt_state already filled gen/bypass/weights; keep them */
        pthread_mutex_lock(&s_mtx);
        st.gen = s_status.gen;
        st.bypass = s_status.bypass;
        st.active_slot = s_status.active_slot;
        memcpy(st.weights, s_status.weights, sizeof(st.weights));
        pthread_mutex_unlock(&s_mtx);
        publish(&st, pick->key);
        fetch_mice(l);
        printf("[DONGLE] Linked AnyMouse dongle on %s: %s; adopted its weights (gen %u)%s\n",
               st.node, line, st.gen, st.vectoring ? "; software vectoring stays idle" : "");
        /* The RT engine may hold another mouse: let it re-select (it goes idle while a dongle vectors) */
        if (atomic_load(&g_state.evdev_grabbed)) devices_request_reselect();
        return;
    case PROBE_INCOMPATIBLE:
        st.link = DONGLE_INCOMPATIBLE;
        snprintf(st.detail, sizeof(st.detail), "%s speaks protocol v%u; this app speaks v%u (update one of them)",
                 st.node, st.proto_ver, AMV_PROTO_VERSION);
        fprintf(stderr, "[DONGLE] %s\n", st.detail);
        publish(&st, pick->key);
        break;
    case PROBE_FOREIGN:
        fprintf(stderr, "[DONGLE] %s uses the AnyMouse usage page but failed the handshake; treating it as an ordinary device\n",
                st.node);
        reject(pick->key);
        publish(&(dongle_status_t){ 0 }, NULL);
        *next_scan = 0;   /* look at the next candidate right away */
        break;
    case PROBE_SILENT:
        st.link = DONGLE_NOT_RESPONDING;
        snprintf(st.detail, sizeof(st.detail), "%s is not answering yet; retrying", st.node);
        publish(&st, pick->key);
        break;
    case PROBE_GONE:
        publish(&(dongle_status_t){ 0 }, NULL);
        break;
    }
    close(l->fd);
    l->fd = -1;
}

static void drop_link(link_t *l) {
    close(l->fd);
    l->fd = -1;
    printf("[DONGLE] AnyMouse dongle disconnected\n");
    publish(&(dongle_status_t){ 0 }, NULL);
    publish_mice(NULL, -1);
    pthread_mutex_lock(&s_job_mtx);              /* fail a write that was waiting for this link */
    if (s_job.pending && !s_job.done) { s_job.status = -1; s_job.done = true; pthread_cond_broadcast(&s_job_cv); }
    pthread_mutex_unlock(&s_job_mtx);
}

void *dongle_thread(void *arg) {
    (void)arg;
    link_t l = { .fd = -1, .sync = true };
    uint64_t next_scan = 0, next_mice = 0;
    while (atomic_load_explicit(&g_state.keep_running, memory_order_relaxed)) {
        if (l.fd < 0) {
            if (dongle_now_ms() < next_scan) {
                usleep(50000);
                continue;
            }
            next_scan = dongle_now_ms() + SCAN_PERIOD_MS;
            try_link(&l, &next_scan);
            continue;
        }
        struct pollfd p = { .fd = l.fd, .events = POLLIN };
        int r = poll(&p, 1, SYNC_PERIOD_MS);
        bool gone = (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) ||
                    (r > 0 && (p.revents & POLLIN) && drain_events(&l) < 0) ||
                    sync_step(&l) < 0 || run_job(&l) < 0;
        if (!gone && (l.mice_dirty || dongle_now_ms() >= next_mice)) {
            next_mice = dongle_now_ms() + MICE_PERIOD_MS;
            gone = fetch_mice(&l) < 0;
        }
        if (gone) drop_link(&l);
    }
    if (l.fd >= 0) close(l.fd);
    return NULL;
}

/* ---- one-shot CLI: --dongle-status, --dongle-bypass on|off ---- */

int dongle_cli(const char *action, const char *arg) {
    candidate_t c[MAX_CANDIDATES];
    int n = scan_candidates(c, MAX_CANDIDATES);
    if (n == 0) {
        printf("No AnyMouse dongle found (no HID interface declares usage page 0x%04X).\n", AMV_VENDOR_USAGE_PAGE);
        return 1;
    }
    int bypass = -1;
    if (strcmp(action, "--dongle-bypass") == 0) {
        if (arg && strcmp(arg, "on") == 0) bypass = 1;
        else if (arg && strcmp(arg, "off") == 0) bypass = 0;
        else {
            printf("Usage: --dongle-bypass on|off\n");
            return 2;
        }
    }

    for (int i = 0; i < n; i++) {
        dongle_status_t st = { 0 };
        snprintf(st.node, sizeof(st.node), "%.30s/%.31s", s_dev_dir, c[i].name);
        link_t l = { .fd = open(st.node, O_RDWR | O_NONBLOCK | O_CLOEXEC), .sync = false };
        if (l.fd < 0) {
            printf("%s: cannot open: %s (run: sudo ./anymouse --install-udev)\n", st.node, strerror(errno));
            continue;
        }
        start_seq(&l);
        probe_t pr = handshake(&l, &st);
        if (pr != PROBE_OK) {
            static const char *why[] = { "disconnected", "no reply", "not an AnyMouse dongle (failed the handshake)",
                                         "incompatible protocol version" };
            printf("%s: %s\n", st.node, why[pr]);
            close(l.fd);
            continue;
        }
        int rc = 0;
        if (bypass >= 0) {
            uint8_t on = (uint8_t)bypass, rep[AMV_FRAME_SIZE];
            amv_state_payload_t p;
            if (request(&l, AMV_CMD_SET_BYPASS, &on, 1, REQUEST_TIMEOUT_MS, rep, NULL) == 1 &&
                ((amv_hdr_t *)rep)->status == AMV_ST_OK && state_from_reply(rep, &p)) {
                adopt_state(&l, &p, false);
                printf("Bypass %s.\n", p.bypass ? "on (all weights 1.0, profile kept)" : "off");
            } else {
                printf("Setting bypass failed.\n");
                rc = 1;
            }
        } else if (fetch_state(&l, false) == 1) {
            char line[160];
            format_status_line(&st, line, sizeof(line));
            dongle_status_t cur;
            dongle_get_status(&cur);
            printf("AnyMouse dongle on %s\n", st.node);
            printf("  Identity   : %s\n", line);
            printf("  Vectoring  : %s\n", st.vectoring ? "in hardware (software engine stays idle)" : "off");
            printf("  Weights    : gen %u, slot %u, bypass %s\n", cur.gen, cur.active_slot, cur.bypass ? "on" : "off");
            printf("  ");
            for (int k = 0; k < 8; k++) printf("%s=%.3f ", OCTANT_INFO[k].short_name, cur.weights[k]);
            printf("\n");
            l.sync = false;
            fetch_mice(&l);
            pthread_mutex_lock(&s_mtx);
            int nm = s_nmice;
            dongle_mouse_t mice[MAX_MICE];
            if (nm > 0) memcpy(mice, s_mice, sizeof(mice[0]) * (size_t)nm);
            pthread_mutex_unlock(&s_mtx);
            static const char *TIER[] = { "?", "known protocol", "generic HID", "boot fallback", "ignored" };
            if (nm < 0) printf("  Mice       : not reported by this firmware (needs 0.5.0)\n");
            else if (nm == 0) printf("  Mice       : none plugged into the dongle\n");
            for (int k = 0; k < nm; k++) {
                const amv_mouse_info_t *x = &mice[k].info;
                printf("  Mouse %d    : %s [%04x:%04x] %s", k, x->name, x->vid, x->pid, x->tier <= 4 ? TIER[x->tier] : "?");
                if (x->tier != AMV_TIER_IGNORED) {
                    printf(", CPI %s%u, polling %u Hz", x->cpi ? "" : "unknown ", x->cpi, x->device_hz ? x->device_hz : x->nominal_hz);
                    if (x->battery != 0xFF) printf(", battery %u%%", x->battery);
                    if (mice[k].has_caps && mice[k].caps.firmware[0]) printf(", firmware %s", mice[k].caps.firmware);
                    if (x->tier == AMV_TIER_KNOWN) printf("%s", (x->flags & AMV_MF_VERIFIED) ? " (writable)" : " (read-only: model not verified)");
                }
                printf("\n");
            }
        } else {
            printf("%s: no reply to GET_WEIGHTS\n", st.node);
            rc = 1;
        }
        close(l.fd);
        return rc;
    }
    return 1;
}
