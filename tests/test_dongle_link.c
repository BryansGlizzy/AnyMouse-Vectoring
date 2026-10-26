/*
 * Host-side tests for the AnyMouse dongle link (src/dongle.c) and its guard in src/devices.c.
 * Linux only (uses the real sources). A fake dongle answers over a SOCK_SEQPACKET socketpair,
 * which keeps hidraw's one-report-per-read semantics; discovery runs against a fake sysfs tree.
 *
 *   make test-dongle
 */
#include "../src/devices.c"
#include "../src/dongle.c"

#include <math.h>
#include <sys/socket.h>
#include <sys/stat.h>

anymouse_state_t g_state;
const octant_info_t OCTANT_INFO[8] = {
    { "East", "E", 0, "" }, { "North-East", "NE", 45, "" }, { "North", "N", 90, "" }, { "North-West", "NW", 135, "" },
    { "West", "W", 180, "" }, { "South-West", "SW", 225, "" }, { "South", "S", 270, "" }, { "South-East", "SE", 315, "" },
};

/* Stub: which fake hidraw nodes declare the AnyMouse vendor collection */
static const char *s_fake_candidates[4];
bool hid_descriptor_has_collection(const char *name, uint16_t page, uint16_t usage) {
    if (page != AMV_VENDOR_USAGE_PAGE || usage != AMV_VENDOR_USAGE) return false;
    for (int i = 0; i < 4 && s_fake_candidates[i]; i++) if (strcmp(name, s_fake_candidates[i]) == 0) return true;
    return false;
}

static int s_fail;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); s_fail++; } printf("\n"); } while (0)

/* ---------- fake dongle ---------- */

typedef enum { FAKE_GOOD, FAKE_LEGACY, FAKE_FOREIGN, FAKE_SILENT, FAKE_BAD_NONCE, FAKE_V2, FAKE_NO_VECTOR } fake_mode_t;

typedef struct {
    int fd;
    fake_mode_t mode;
    pthread_t tid;
    pthread_mutex_t mtx;
    uint32_t gen;
    float w[8];
    uint8_t bypass;
    int sets;               /* SET_WEIGHTS received */
    uint32_t last_base_gen;
    bool bad_report_id;     /* a frame arrived without the leading report ID 0 */
    uint16_t cpi, hz;       /* the Razer behind the fake dongle */
    uint16_t user_cpi;      /* label for the generic mouse */
    int mouse_sets;
} fake_t;

/* Two things behind the fake dongle: a verified Razer (known protocol) and a keyboard (ignored) */
static void fake_mouse_info(const fake_t *f, uint8_t i, amv_mouse_info_t *o) {
    memset(o, 0, sizeof *o);
    o->index = i; o->count = 2; o->battery = 0xFF; o->speed = 1;
    if (i == 0) {
        o->tier = AMV_TIER_KNOWN; o->flags = AMV_MF_WHEEL | AMV_MF_VERIFIED | AMV_MF_CAN_SET_CPI | AMV_MF_CAN_SET_RATE;
        o->vid = 0x1532; o->pid = 0x0086; o->nominal_hz = 1000; o->measured_hz = 733; o->device_hz = f->hz; o->cpi = f->cpi;
        o->cpi_source = AMV_CPI_DEVICE; o->battery = 100; o->num_buttons = 5; o->x_bits = 16; o->driver = AMV_DRV_RAZER; o->uid = 1;
        snprintf(o->name, sizeof o->name, "Razer Basilisk Ultimate");
    } else {
        o->tier = AMV_TIER_IGNORED; o->vid = 0x05ac; o->pid = 0x024f; o->reason = AMV_IGN_NOT_MOUSE; o->uid = 2;
        snprintf(o->name, sizeof o->name, "USB Keyboard");
    }
}

static void fake_send(fake_t *f, uint8_t cmd, uint8_t seq, uint8_t status, uint8_t ver, const void *pl, uint8_t len) {
    uint8_t out[AMV_FRAME_SIZE] = { AMV_MAGIC0, AMV_MAGIC1, ver, cmd, seq, status, len, 0 };
    if (len) memcpy(out + AMV_HEADER_SIZE, pl, len);
    if (write(f->fd, out, sizeof(out)) != (ssize_t)sizeof(out)) { /* peer closed */ }
}

static void fake_state(fake_t *f, amv_state_payload_t *p) {
    p->gen = f->gen;
    memcpy(p->w, f->w, sizeof(p->w));
    p->bypass = f->bypass;
    p->active_slot = 0;
}

/* Another client changed the dongle: announce it as the firmware does */
static void fake_push_event(fake_t *f, int octant, float value) {
    pthread_mutex_lock(&f->mtx);
    f->w[octant] = value;
    f->gen++;
    amv_event_payload_t ev;
    fake_state(f, &ev.st);
    ev.source = AMV_SRC_HOST;
    fake_send(f, AMV_CMD_EVENT_STATE, 0, AMV_ST_OK, AMV_PROTO_VERSION, &ev, sizeof(ev));
    pthread_mutex_unlock(&f->mtx);
}

static void *fake_main(void *arg) {
    fake_t *f = arg;
    uint8_t in[1 + AMV_FRAME_SIZE];
    for (;;) {
        ssize_t n = read(f->fd, in, sizeof(in));
        if (n <= 0) return NULL;
        if (n != (ssize_t)sizeof(in) || in[0] != 0) { f->bad_report_id = true; continue; }
        const uint8_t *fr = in + 1;
        const amv_hdr_t *h = (const amv_hdr_t *)fr;
        const uint8_t *pl = fr + AMV_HEADER_SIZE;
        if (f->mode == FAKE_SILENT) continue;
        if (f->mode == FAKE_FOREIGN) {
            uint8_t junk[AMV_FRAME_SIZE];
            memset(junk, 0x5A, sizeof(junk));
            if (write(f->fd, junk, sizeof(junk)) < 0) return NULL;
            continue;
        }
        uint8_t ver = f->mode == FAKE_V2 ? 2 : AMV_PROTO_VERSION;
        pthread_mutex_lock(&f->mtx);
        amv_state_payload_t st;
        if (f->mode == FAKE_LEGACY && h->cmd >= AMV_CMD_GET_MOUSE && h->cmd <= AMV_CMD_SET_MOUSE_POLLING) {
            fake_send(f, h->cmd, h->seq, AMV_ST_BAD_CMD, ver, NULL, 0);   /* firmware before 0.5.0 */
            pthread_mutex_unlock(&f->mtx);
            continue;
        }
        switch (h->cmd) {
        case AMV_CMD_HELLO: {
            if (f->mode == FAKE_LEGACY) { fake_send(f, h->cmd, h->seq, AMV_ST_BAD_CMD, ver, NULL, 0); break; }
            amv_hello_rep_t r = { .ident = AMV_HELLO_IDENT, .proto_ver = ver, .fw_major = 0, .fw_minor = 4, .fw_patch = 0,
                                  .caps = f->mode == FAKE_NO_VECTOR ? 0 : AMV_CAP_VECTORED_MOUSE,
                                  .mac = { 0x30, 0xED, 0xA0, 0xEA, 0x6C, 0xAB } };
            memcpy(r.nonce, pl, sizeof(r.nonce));
            if (f->mode == FAKE_BAD_NONCE) r.nonce[0] ^= 0xFF;
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &r, sizeof(r));
            break;
        }
        case AMV_CMD_GET_INFO: {
            amv_info_payload_t r = { .proto_ver = AMV_PROTO_VERSION, .fw_major = 0, .fw_minor = 3, .num_slots = 4 };
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &r, sizeof(r));
            break;
        }
        case AMV_CMD_GET_WEIGHTS:
            fake_state(f, &st);
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &st, sizeof(st));
            break;
        case AMV_CMD_SET_WEIGHTS: {
            amv_set_weights_req_t req;
            memcpy(&req, pl, sizeof(req));
            f->sets++;
            f->last_base_gen = req.base_gen;
            uint8_t status = AMV_ST_OK;
            if (req.base_gen != AMV_GEN_ANY && req.base_gen != f->gen) {
                status = AMV_ST_STALE;
            } else {
                memcpy(f->w, req.w, sizeof(f->w));
                f->gen++;
                /* Like the firmware, announce the change to every client (here before the reply) */
                amv_event_payload_t ev;
                fake_state(f, &ev.st);
                ev.source = AMV_SRC_HOST;
                fake_send(f, AMV_CMD_EVENT_STATE, 0, AMV_ST_OK, ver, &ev, sizeof(ev));
            }
            fake_state(f, &st);
            fake_send(f, h->cmd, h->seq, status, ver, &st, sizeof(st));
            break;
        }
        case AMV_CMD_SET_BYPASS:
            f->bypass = pl[0];
            fake_state(f, &st);
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &st, sizeof(st));
            break;
        case AMV_CMD_GET_MOUSE: {
            amv_mouse_info_t m;
            if (pl[0] > 1) { uint8_t r[2] = { pl[0], 2 }; fake_send(f, h->cmd, h->seq, AMV_ST_NO_MOUSE, ver, r, 2); break; }
            fake_mouse_info(f, pl[0], &m);
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &m, sizeof m);
            break;
        }
        case AMV_CMD_GET_MOUSE_CAPS: {
            if (pl[0] != 0) { fake_send(f, h->cmd, h->seq, AMV_ST_UNSUPPORTED, ver, NULL, 0); break; }
            amv_mouse_caps_t c = { .index = 0, .driver = AMV_DRV_RAZER, .n_rates = 3, .rates = { 125, 500, 1000 },
                                   .cpi_min = 100, .cpi_max = 20000, .cpi_step = 50 };
            snprintf(c.firmware, sizeof c.firmware, "1.7");
            fake_send(f, h->cmd, h->seq, AMV_ST_OK, ver, &c, sizeof c);
            break;
        }
        case AMV_CMD_SET_MOUSE_CPI:
        case AMV_CMD_SET_MOUSE_POLLING: {
            amv_mouse_set_req_t r;
            memcpy(&r, pl, sizeof r);
            f->mouse_sets++;
            amv_mouse_info_t m;
            uint8_t st = AMV_ST_OK;
            if (r.index != 0) st = AMV_ST_UNSUPPORTED;
            else if (h->cmd == AMV_CMD_SET_MOUSE_CPI) { if (r.value < 100 || r.value > 20000) st = AMV_ST_BAD_VALUE; else f->cpi = r.value; }
            else { if (r.value != 125 && r.value != 500 && r.value != 1000) st = AMV_ST_BAD_VALUE; else f->hz = r.value; }
            fake_mouse_info(f, r.index > 1 ? 0 : r.index, &m);
            if (st == AMV_ST_OK) {
                amv_mouse_event_t ev = { .count = 2, .change = AMV_MEV_UPDATED, .uid = 1 };
                fake_send(f, AMV_CMD_EVENT_MOUSE, 0, AMV_ST_OK, ver, &ev, sizeof ev);
            }
            fake_send(f, h->cmd, h->seq, st, ver, &m, sizeof m);
            break;
        }
        default:
            fake_send(f, h->cmd, h->seq, AMV_ST_BAD_CMD, ver, NULL, 0);
        }
        pthread_mutex_unlock(&f->mtx);
    }
}

/* Returns the host end, configured like an opened hidraw node */
static int fake_start(fake_t *f, fake_mode_t mode) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); exit(2); }
    memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->mtx, NULL);
    f->fd = sv[1];
    f->mode = mode;
    f->gen = 41;
    f->cpi = 1600;
    f->hz = 1000;
    for (int i = 0; i < 8; i++) f->w[i] = 1.0f + 0.1f * (float)i;
    pthread_create(&f->tid, NULL, fake_main, f);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    return sv[0];
}

static void fake_stop(fake_t *f, int host_fd) {
    if (host_fd >= 0) close(host_fd);
    shutdown(f->fd, SHUT_RDWR);
    pthread_join(f->tid, NULL);
    close(f->fd);
}

static void set_local(const float w[8]) {
    for (int i = 0; i < 8; i++) atomic_store(&g_state.octant_weights[i], w[i]);
}

static void get_local(float w[8]) {
    for (int i = 0; i < 8; i++) w[i] = atomic_load(&g_state.octant_weights[i]);
}

/* ---------- tests ---------- */

static void test_weight_rule(void) {
    printf("weight validation (built with -ffast-math)\n");
    volatile float zero = 0.0f;
    CHECK(weight_ok(0.01f) && weight_ok(5.0f) && weight_ok(1.0f), "boundaries 0.01 and 5.0 accepted");
    CHECK(!weight_ok(0.0099f) && !weight_ok(5.01f) && !weight_ok(0.0f) && !weight_ok(-1.0f), "out of range refused");
    CHECK(!weight_ok(zero / zero), "NaN refused");
    CHECK(!weight_ok(1.0f / zero) && !weight_ok(-1.0f / zero), "Inf refused");
}

static void test_handshake(void) {
    printf("handshake classification\n");
    static const struct { fake_mode_t mode; probe_t want; const char *what; } cases[] = {
        { FAKE_GOOD, PROBE_OK, "current firmware: verified by HELLO" },
        { FAKE_LEGACY, PROBE_OK, "pre-HELLO firmware: recognised by GET_INFO" },
        { FAKE_FOREIGN, PROBE_FOREIGN, "device on the same usage page, not our protocol" },
        { FAKE_SILENT, PROBE_SILENT, "silent device: not rejected, retried" },
        { FAKE_BAD_NONCE, PROBE_FOREIGN, "wrong nonce echo: rejected" },
        { FAKE_V2, PROBE_INCOMPATIBLE, "other protocol version" },
        { FAKE_NO_VECTOR, PROBE_OK, "dongle without vectored output" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_t f;
        link_t l = { .fd = fake_start(&f, cases[i].mode) };
        dongle_status_t st = { 0 };
        probe_t got = handshake(&l, &st);
        CHECK(got == cases[i].want, "%s (got %d)", cases[i].what, got);
        if (cases[i].mode == FAKE_GOOD) {
            CHECK(!st.legacy && st.vectoring && st.fw[1] == 4 && st.mac[5] == 0xAB, "  identity, caps and fw read from HELLO");
            CHECK(!f.bad_report_id, "  frames written with the hidraw report-ID prefix");
        }
        if (cases[i].mode == FAKE_LEGACY) CHECK(st.legacy && st.vectoring && st.fw[1] == 3, "  flagged legacy, assumed vectoring");
        if (cases[i].mode == FAKE_NO_VECTOR) CHECK(!st.vectoring, "  vectoring cap off");
        fake_stop(&f, l.fd);
    }
}

static void test_sync(void) {
    printf("weight sync\n");
    fake_t f;
    link_t l = { .fd = fake_start(&f, FAKE_GOOD), .sync = true };
    float w[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    set_local(w);
    dongle_status_t st = { 0 };
    handshake(&l, &st);
    uint32_t rev0 = atomic_load(&g_state.dongle_weights_rev);
    CHECK(fetch_state(&l, true) == 1, "GET_WEIGHTS on link");
    get_local(w);
    CHECK(w[3] == f.w[3] && l.gen == 41, "engine adopted the dongle's weights and generation");
    CHECK(atomic_load(&g_state.dongle_weights_rev) != rev0, "GUI resync counter bumped");

    CHECK(sync_step(&l) == 0 && f.sets == 0, "no change: nothing sent");

    w[2] = 1.75f;
    set_local(w);
    rev0 = atomic_load(&g_state.dongle_weights_rev);
    CHECK(sync_step(&l) == 0 && f.sets == 1 && f.last_base_gen == 41, "local edit sent with base_gen 41");
    CHECK(l.gen == 42 && f.w[2] == 1.75f, "dongle applied it, link at gen 42");
    CHECK(atomic_load(&g_state.dongle_weights_rev) == rev0, "own change (and its event echo) not written back");
    CHECK(sync_step(&l) == 0 && f.sets == 1, "not resent");

    /* Another client changes the dongle: the event updates the engine */
    fake_push_event(&f, 0, 2.5f);
    usleep(20000);
    CHECK(drain_events(&l) == 0, "event drained");
    get_local(w);
    CHECK(w[0] == 2.5f && l.gen == 43, "web-page edit reached the engine (gen 43)");
    CHECK(sync_step(&l) == 0 && f.sets == 1, "adopted edit is not echoed back");

    /* The dongle moved on without us seeing it: our edit is stale, the dongle wins */
    pthread_mutex_lock(&f.mtx);
    f.w[7] = 3.0f;
    f.gen = 50;
    pthread_mutex_unlock(&f.mtx);
    w[1] = 0.25f;
    set_local(w);
    CHECK(sync_step(&l) == 0 && f.sets == 2, "stale edit sent once");
    get_local(w);
    CHECK(l.gen == 50 && w[7] == 3.0f && w[1] == f.w[1], "STALE: engine adopted the dongle's state");

    /* Values the dongle would refuse are never sent */
    w[4] = 6.0f;
    set_local(w);
    CHECK(sync_step(&l) == 0 && f.sets == 2, "out-of-range local weight not sent");
    CHECK(sync_step(&l) == 0 && f.sets == 2, "and not retried");
    w[4] = 1.0f;
    set_local(w);
    CHECK(sync_step(&l) == 0 && f.sets == 3, "next valid edit goes through");

    /* The echo of an in-flight set must not undo a newer local edit */
    float newer[8];
    get_local(newer);
    newer[5] = 1.9f;
    set_local(newer);
    l.has_inflight = true;
    memcpy(l.inflight, l.synced, sizeof(l.inflight));
    l.inflight[5] = 1.8f;
    uint8_t ev[AMV_FRAME_SIZE] = { AMV_MAGIC0, AMV_MAGIC1, AMV_PROTO_VERSION, AMV_CMD_EVENT_STATE, 0, 0,
                                   sizeof(amv_event_payload_t), 0 };
    amv_event_payload_t ep = { .st = { .gen = l.gen + 1 } };
    memcpy(ep.st.w, l.inflight, sizeof(ep.st.w));
    memcpy(ev + AMV_HEADER_SIZE, &ep, sizeof(ep));
    handle_event(&l, ev);
    l.has_inflight = false;
    get_local(w);
    CHECK(w[5] == 1.9f, "echo of in-flight set left the newer local edit alone");

    /* Unplug */
    shutdown(f.fd, SHUT_RDWR);
    usleep(20000);
    CHECK(drain_events(&l) < 0, "unplug detected");
    fake_stop(&f, l.fd);
}

typedef struct { uint8_t index; bool cpi; uint16_t value; int out; } job_args_t;

static void *job_thread(void *arg) {
    job_args_t *j = arg;
    j->out = dongle_mouse_set(j->index, j->cpi, j->value);
    return NULL;
}

/* Issue a write from another thread and execute it here, the way the dongle thread does */
static int write_through(link_t *l, uint8_t index, bool cpi, uint16_t value) {
    job_args_t j = { index, cpi, value, 99 };
    pthread_t t;
    pthread_create(&t, NULL, job_thread, &j);
    for (int i = 0; i < 400; i++) {
        pthread_mutex_lock(&s_job_mtx);
        bool pending = s_job.pending && !s_job.done;
        pthread_mutex_unlock(&s_job_mtx);
        if (pending) break;
        usleep(5000);
    }
    run_job(l);
    pthread_join(t, NULL);
    return j.out;
}

static void test_mice(void) {
    printf("mice behind the dongle\n");
    fake_t f;
    link_t l = { .fd = fake_start(&f, FAKE_GOOD), .sync = true };
    dongle_status_t st = { 0 };
    handshake(&l, &st);
    st.link = DONGLE_LINKED;
    publish(&st, "fake");

    CHECK(fetch_mice(&l) == 0, "list fetched");
    dongle_mouse_t m[MAX_MICE];
    int n = dongle_get_mice(m, MAX_MICE);
    CHECK(n == 2, "two entries (a mouse and an ignored keyboard)");
    CHECK(m[0].info.tier == AMV_TIER_KNOWN && m[0].has_caps && m[0].caps.n_rates == 3 && m[0].caps.cpi_max == 20000 && !strcmp(m[0].caps.firmware, "1.7"),
          "known mouse comes with its capabilities");
    CHECK(m[1].info.tier == AMV_TIER_IGNORED && !m[1].has_caps && m[1].info.reason == AMV_IGN_NOT_MOUSE, "keyboard listed as ignored, no caps asked for");
    CHECK(!strcmp(m[0].info.name, "Razer Basilisk Ultimate") && m[0].info.vid == 0x1532, "name and VID:PID decoded");

    mouse_hw_status_t hw;
    CHECK(dongle_fill_hw_status(&hw) && hw.via_dongle && hw.dpi == 1600 && hw.polling_rate_hz == 1000 && hw.battery_pct == 100,
          "hardware panel gets CPI 1600, 1000 Hz, battery 100%% from the dongle");
    CHECK(hw.can_set_cpi && hw.can_set_rate && hw.supported_rates[0] == 125 && hw.supported_rates[2] == 1000 && hw.supported_rates[3] == 0 &&
          !strcmp(hw.brand, "Razer") && !strcmp(hw.mouse_fw, "1.7"), "write permissions, rate list, brand and firmware carried over");

    uint32_t rev = atomic_load(&g_state.dongle_mice_rev);
    CHECK(fetch_mice(&l) == 0 && atomic_load(&g_state.dongle_mice_rev) == rev, "an unchanged list (only the measured rate moving) does not wake the GUI");

    int st1 = write_through(&l, 0, true, 3200);
    CHECK(st1 == AMV_ST_OK && f.cpi == 3200 && f.mouse_sets == 1, "SET_MOUSE_CPI from another thread reached the dongle; caller got OK");
    CHECK(atomic_load(&g_state.dongle_mice_rev) != rev && dongle_get_mice(m, MAX_MICE) == 2 && m[0].info.cpi == 3200,
          "list refreshed after the write (CPI now 3200) and the GUI is woken");
    CHECK(write_through(&l, 0, false, 500) == AMV_ST_OK && f.hz == 500, "polling rate write reached the dongle");
    CHECK(write_through(&l, 0, false, 777) == AMV_ST_BAD_VALUE && f.hz == 500, "a rate the mouse does not accept comes back as BAD_VALUE, nothing changed");
    CHECK(write_through(&l, 0, true, 50) == AMV_ST_BAD_VALUE && f.cpi == 3200, "a CPI out of range comes back as BAD_VALUE");
    CHECK(write_through(&l, 1, true, 800) == AMV_ST_UNSUPPORTED, "writing to an ignored device is refused");
    CHECK(first_mouse_index() == 0, "the desktop panel targets the first used mouse (skipping ignored devices)");

    /* Old firmware: GET_MOUSE answers BAD_CMD and the app reports 'not available' */
    fake_t g;
    link_t l2 = { .fd = fake_start(&g, FAKE_LEGACY), .sync = true };
    CHECK(fetch_mice(&l2) == 0 && dongle_get_mice(m, MAX_MICE) == -1, "pre-0.5.0 firmware: mice reported as unavailable (-1)");
    fake_stop(&g, l2.fd);

    /* Unplug clears everything and fails a waiting write */
    publish(&(dongle_status_t){ 0 }, NULL);
    CHECK(dongle_get_mice(m, MAX_MICE) == -1 && !dongle_fill_hw_status(&hw), "no link: no mice, hardware panel falls back to direct hidraw");
    CHECK(dongle_mouse_set(0, true, 800) == -1, "a write with no link fails at once");
    fake_stop(&f, l.fd);
}

static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void test_guard(void) {
    printf("guard (fake sysfs)\n");
    char root[] = "/tmp/amv_sysfs_XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char cls[PATH_MAX], dev[PATH_MAX], devdir[PATH_MAX], link[PATH_MAX];
    snprintf(cls, sizeof(cls), "%s/class/hidraw", root);
    snprintf(devdir, sizeof(devdir), "%s/dev", root);
    mkdirs(devdir);
    /* hidraw0: a dongle's vendor interface on usb1/1-2; hidraw1: unrelated device on usb1/1-3 */
    const char *nodes[][2] = {
        { "hidraw0", "devices/pci0/usb1/1-2/1-2:1.1/0003:1234:5678.0007" },
        { "hidraw1", "devices/pci0/usb1/1-3/1-3:1.0/0003:046D:C077.0003" },
    };
    for (int i = 0; i < 2; i++) {
        snprintf(dev, sizeof(dev), "%s/%s", root, nodes[i][1]);
        mkdirs(dev);
        snprintf(link, sizeof(link), "%s/%s", cls, nodes[i][0]);
        mkdirs(link);
        strcat(link, "/device");
        if (symlink(dev, link) != 0) { perror("symlink"); exit(2); }
    }
    s_hidraw_class = cls;
    s_dev_dir = devdir;
    s_fake_candidates[0] = "hidraw0";

    candidate_t c[MAX_CANDIDATES];
    int n = scan_candidates(c, MAX_CANDIDATES);
    CHECK(n == 1 && strcmp(c[0].usb_parent, "usb1/1-2") == 0, "only the 0xFF4D interface is a candidate (usb1/1-2), whatever its VID:PID");
    CHECK(dongle_guards_usb_device("usb1/1-2"), "unverified candidate already guards its mouse");
    CHECK(!dongle_guards_usb_device("usb1/1-3") && !dongle_guards_usb_device(""), "other USB devices are not guarded");
    CHECK(dongle_guard_active(), "guard active");

    /* The node cannot be opened (no /dev/hidraw0 here): reported, still guarded */
    link_t l = { .fd = -1, .sync = true };
    uint64_t next = 0;
    try_link(&l, &next);
    dongle_status_t st;
    dongle_get_status(&st);
    CHECK(st.link == DONGLE_NO_ACCESS && l.fd < 0, "unopenable node reported as NO_ACCESS: %s", st.detail);
    CHECK(dongle_guards_usb_device("usb1/1-2"), "NO_ACCESS still guards (fail safe)");

    /* Linked dongle that says its output is not vectored: mouse released */
    pthread_mutex_lock(&s_mtx);
    snprintf(s_linked_key, sizeof(s_linked_key), "%s", c[0].key);
    s_status.link = DONGLE_LINKED;
    s_status.vectoring = false;
    pthread_mutex_unlock(&s_mtx);
    CHECK(!dongle_guards_usb_device("usb1/1-2"), "verified non-vectoring dongle does not guard");
    pthread_mutex_lock(&s_mtx);
    s_status.vectoring = true;
    pthread_mutex_unlock(&s_mtx);
    CHECK(dongle_guards_usb_device("usb1/1-2"), "verified vectoring dongle guards");

    /* Failed the handshake: an ordinary device again */
    publish(&(dongle_status_t){ 0 }, NULL);
    reject(c[0].key);
    CHECK(!dongle_guards_usb_device("usb1/1-2") && !dongle_guard_active(), "rejected candidate releases its mouse");

    /* Same port, new device instance (new sysfs key): guarded again */
    snprintf(dev, sizeof(dev), "%s/devices/pci0/usb1/1-2/1-2:1.1/0003:1234:5678.0009", root);
    mkdirs(dev);
    snprintf(link, sizeof(link), "%s/hidraw0/device", cls);
    unlink(link);
    if (symlink(dev, link) != 0) { perror("symlink"); exit(2); }
    CHECK(dongle_guards_usb_device("usb1/1-2"), "re-plugged device is judged afresh");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { /* best effort */ }
}

int main(void) {
    atomic_store(&g_state.keep_running, true);
    test_weight_rule();
    test_handshake();
    test_sync();
    test_mice();
    test_guard();
    printf(s_fail ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}
