#include "mice.h"
#include <Arduino.h>
#include <string.h>
#include "amv_catalog.h"
#include "amv_store_teensy.h"
#include "host.h"
#include "js_engine.h"

extern "C" void amv_log(const char *fmt, ...);

#define MAX_MICE      8
#define MAX_IGNORED   8
#define MAX_SESS      4
#define MAX_CFG       5
#define SETTLE_MS     400       // a device's interfaces arrive one by one; wait this long after the last
#define SETTLE_READ_MS 700      // re-read this long after a write
#define REFRESH_MS    5000      // KNOWN mice: readStatus again (battery, a rate changed on the mouse, ...)
#define RATE_WINDOW   10        // measured rate: highest of the last 10 one-second samples
#define BIT_OF(id)    (1u << (id))

/* Writes allowed (VID << 16 | PID): models verified through a dongle on real hardware, the same list as the
 * ESP32-P4 (drv_openmouse.c): Basilisk Ultimate wired, the G Pro's Lightspeed receiver, the Finalmouse
 * UltralightX dongle. Every other model is read-only (README, write policy). */
static const uint32_t VERIFIED[] = { 0x15320086, 0x046dc547, 0x361d0100 };

struct ifref_t { host_if_t *h; uint32_t serial; };

static bool alive(const ifref_t &r) { return r.h && r.h->used && !r.h->gone && r.h->dev_serial == r.serial; }

// One OpenMouse driver session per physical device, over all its HID interfaces.
struct session_t {
    bool     used;
    void    *dev;
    int      sid;              // engine session, 0 = none
    bool     no_driver;        // OpenMouse has no driver for these interfaces
    bool     probed;           // a readStatus has succeeded (the mouse is KNOWN)
    bool     verified;
    bool     error;            // the last exchange failed
    bool     dying;            // the device left; waiting for a running call to finish
    ifref_t  mouse;            // x = -1
    ifref_t  cfg[MAX_CFG];     // x = 0..
    int      n_cfg;
    uint32_t next_ms;          // next refresh or retry
    uint8_t  fail_n;
    uint32_t retry_reports;    // the mouse's report count at the last failed probe
    // what the driver reported (apply())
    char     name[30], firmware[16];
    uint16_t cpi, device_hz, rates[8], cpi_min, cpi_max, cpi_step;
    uint8_t  n_rates, battery;
    bool     can_set_cpi, can_set_rate;
    uint16_t readable, writable, lod, lod_opts[6];
    uint8_t  n_lod, motion_sync, charging, link;
};

struct mouse_t {
    bool     used;
    ifref_t  r;
    uint16_t uid;
    uint8_t  tier;
    int8_t   sess;             // session index, -1 none
    uint16_t cpi_user;
    uint16_t hist[RATE_WINDOW];
    unsigned hslot;
    uint32_t sample_ms, sample_reports;
};

struct ignored_t {
    bool     used;
    void    *dev;
    ifref_t  r;                // one interface of the device, to see it leave
    uint16_t uid, vid, pid;
    uint8_t  speed, iface, reason;
    char     name[30];
};

static mouse_t   s_mice[MAX_MICE];
static ignored_t s_ign[MAX_IGNORED];
static session_t s_sess[MAX_SESS];
static uint16_t  s_next_uid;
static bool      s_engine;

// the engine's one running call
enum { JOB_NONE, JOB_PROBE, JOB_WRITE, JOB_CLOSE };
static int s_job = JOB_NONE, s_job_sess = -1;
// a write the protocol asked for
static struct { bool pending, started, done; int sess; uint8_t setting; uint16_t value; uint8_t status; } s_write;

static void emit(uint8_t change, uint16_t uid) { amv_proto_notify_mouse(change, uid); }

// ---------------------------------------------------------------------------------------------------------------
// transport for the engine (js_engine.h); h is the session

static host_if_t *iface_of(session_t *s, int x) {
    ifref_t *r = x < 0 ? &s->mouse : (x < s->n_cfg ? &s->cfg[x] : nullptr);
    return r && alive(*r) ? r->h : nullptr;
}

static int t_send(void *hh, int x, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len) {
    host_if_t *h = iface_of((session_t *)hh, x);
    if (!h) return -3;
    return type == 3 ? host_set_report(h, 3, id, data, len) : host_send_output(h, id, data, len);
}

static int t_recv(void *hh, int x, uint8_t id, uint8_t *data, uint16_t cap, int *actual) {
    host_if_t *h = iface_of((session_t *)hh, x);
    if (!h) return -3;
    return host_get_report(h, 3, id, data, cap, actual);
}

static int t_poll(void *hh, int *x, uint8_t *raw, int cap) {
    session_t *s = (session_t *)hh;
    for (int k = -1; k < s->n_cfg; k++) {
        host_if_t *h = iface_of(s, k);
        if (!h) continue;
        int n = host_pop_input(h, raw, cap);
        if (n > 0) { *x = k; return n; }
    }
    return 0;
}

static const js_transport_t s_transport = { t_send, t_recv, t_poll };

// ---------------------------------------------------------------------------------------------------------------

static void apply(session_t *s, const js_status_t *st) {
    bool w = s->verified;
    s->error = false;
    if (st->name[0]) snprintf(s->name, sizeof s->name, "%s", st->name);
    snprintf(s->firmware, sizeof s->firmware, "%s", st->firmware);
    s->cpi = st->cpi;
    s->device_hz = st->hz;
    s->battery = st->battery < 0 ? 0xFF : (uint8_t)(st->battery > 100 ? 100 : st->battery);
    s->n_rates = st->n_rates;
    memcpy(s->rates, st->rates, sizeof s->rates);
    s->cpi_min = st->cpi_min; s->cpi_max = st->cpi_max; s->cpi_step = st->cpi_step;
    s->charging = st->charging;
    s->link = st->link;
    s->can_set_cpi = w && st->can_cpi;
    s->can_set_rate = w && st->can_rate;
    s->readable = BIT_OF(AMV_SET_CPI) | BIT_OF(AMV_SET_POLLING) | BIT_OF(AMV_SET_LINK) |
                  (st->battery >= 0 ? BIT_OF(AMV_SET_BATTERY) : 0) | (st->charging ? BIT_OF(AMV_SET_CHARGING) : 0);
    s->writable = 0;
    s->n_lod = 0;
    if (st->lod && st->n_lods) {
        s->readable |= BIT_OF(AMV_SET_LOD);
        if (w && st->can_lod) s->writable |= BIT_OF(AMV_SET_LOD);
        s->lod = st->lod;
        s->n_lod = st->n_lods;
        for (int i = 0; i < st->n_lods; i++) s->lod_opts[i] = st->lods[i];
    }
    if (st->motion_sync >= 0) {
        s->readable |= BIT_OF(AMV_SET_MOTION_SYNC);
        if (w && st->can_motion_sync) s->writable |= BIT_OF(AMV_SET_MOTION_SYNC);
        s->motion_sync = (uint8_t)st->motion_sync;
    }
}

static mouse_t *mouse_of_session(int si) {
    for (auto &m : s_mice) if (m.used && m.sess == si) return &m;
    return nullptr;
}

static void set_tier(int si) {
    mouse_t *m = mouse_of_session(si);
    if (m && s_sess[si].probed) m->tier = AMV_TIER_KNOWN;
}

// Index order: mice, then ignored devices (as the ESP32's resolve()).
static int resolve(uint8_t index, int *count) {
    int n = 0, hit = INT32_MIN;
    for (int i = 0; i < MAX_MICE; i++) if (s_mice[i].used) { if (n == index) hit = i; n++; }
    for (int i = 0; i < MAX_IGNORED; i++) if (s_ign[i].used) { if (n == index) hit = -1 - i; n++; }
    *count = n;
    return hit;
}

uint8_t mice_count(void) { int n; resolve(0, &n); return (uint8_t)n; }

// ---------------------------------------------------------------------------------------------------------------
// lifecycle

static int find_free_session() { for (int i = 0; i < MAX_SESS; i++) if (!s_sess[i].used) return i; return -1; }

static void open_session(mouse_t *m) {
    host_if_t *mh = m->r.h;
    const amv_catalog_entry_t *e = amv_catalog_find(mh->vid, mh->pid);
    if (!s_engine || !e || !e->engine) return;
    int si = find_free_session();
    if (si < 0) return;
    session_t *s = &s_sess[si];
    *s = session_t{};
    s->used = true;
    s->dev = mh->dev;
    s->mouse = m->r;
    s->battery = 0xFF;
    js_iface_t ifs[1 + MAX_CFG];
    int n = 0;
    ifs[n++] = { -1, mh->desc, mh->desc_len };
    for (auto &h : host_ifs) {
        if (!h.used || h.gone || h.dev != mh->dev || &h == mh || s->n_cfg == MAX_CFG || !h.desc_len) continue;
        s->cfg[s->n_cfg] = { &h, h.dev_serial };
        ifs[n++] = { s->n_cfg, h.desc, h.desc_len };
        s->n_cfg++;
    }
    for (int k = -1; k < s->n_cfg; k++) { host_if_t *h = iface_of(s, k); h->listen = true; host_flush_input(h); }
    uint32_t id = ((uint32_t)mh->vid << 16) | mh->pid;
    for (auto v : VERIFIED) s->verified |= v == id;
    char brand[16];
    s->sid = js_open(&s_transport, s, mh->vid, mh->pid, mh->product, ifs, n, brand, sizeof brand);
    m->sess = (int8_t)si;
    if (!s->sid) {
        s->no_driver = true;
        amv_log("[mice] %04x:%04x: OpenMouse has no driver for these interfaces; generic\n", mh->vid, mh->pid);
        for (int k = -1; k < s->n_cfg; k++) if (host_if_t *h = iface_of(s, k)) h->listen = false;
        return;
    }
    s->next_ms = millis();          // probe as soon as the engine is free
    amv_log("[mice] %04x:%04x \"%s\": OpenMouse %s driver over %d interface(s)%s\n", mh->vid, mh->pid, mh->product, brand,
            n, s->verified ? "" : " (model not verified through a dongle: read-only)");
}

static void close_session(int si) {
    session_t *s = &s_sess[si];
    if (!s->used) return;
    if (s_job != JOB_NONE && s_job_sess == si) { s->dying = true; return; }   // after the running call
    if (s->sid) js_forget(s->sid);
    if (s_write.pending && s_write.sess == si && !s_write.done) { s_write.done = true; s_write.status = AMV_ST_NO_MOUSE; }
    *s = session_t{};
}

static void attach_device(void *dev) {
    bool any_mouse = false, any_abs = false;
    host_if_t *first = nullptr;
    for (auto &h : host_ifs) {
        if (!h.used || h.gone || h.dev != dev || h.announced) continue;
        h.announced = true;
        if (!first || h.ifnum < first->ifnum) first = &h;
        if (h.role == HOST_ROLE_ABSOLUTE) any_abs = true;
        if (h.role != HOST_ROLE_MOUSE && h.role != HOST_ROLE_BOOT) continue;
        any_mouse = true;
        int i = 0;
        while (i < MAX_MICE && s_mice[i].used) i++;
        if (i == MAX_MICE) {
            amv_log("[mice] %04x:%04x if %u: no slot for another mouse\n", h.vid, h.pid, h.ifnum);
            continue;
        }
        mouse_t *m = &s_mice[i];
        *m = mouse_t{};
        m->used = true;
        m->r = { &h, h.dev_serial };
        m->uid = ++s_next_uid;
        m->tier = h.role == HOST_ROLE_BOOT ? AMV_TIER_BOOT : AMV_TIER_GENERIC;
        m->sess = -1;
        m->sample_ms = millis();
        m->sample_reports = h.motion_reports;
        uint16_t cpi;
        if (amv_store_label_get(h.vid, h.pid, &cpi)) m->cpi_user = cpi;
        if (h.role == HOST_ROLE_BOOT) host_set_boot_protocol(&h);
        amv_log("[mice] mouse %04x:%04x if %u \"%s\" (%s speed, %u Hz interval): %s\n", h.vid, h.pid, h.ifnum, h.product,
                h.speed == 2 ? "high" : h.speed == 0 ? "full" : "low", h.nominal_hz,
                h.role == HOST_ROLE_BOOT ? "boot protocol fallback" : "vectored");
        emit(AMV_MEV_ATTACHED, m->uid);
        bool has_session = false;
        for (auto &s : s_sess) if (s.used && s.dev == dev) has_session = true;
        if (!has_session) open_session(m);
    }
    if (!any_mouse && first) {
        int i = 0;
        while (i < MAX_IGNORED && s_ign[i].used) i++;
        if (i == MAX_IGNORED) return;
        ignored_t *g = &s_ign[i];
        *g = ignored_t{};
        g->used = true;
        g->dev = dev;
        g->r = { first, first->dev_serial };
        g->uid = ++s_next_uid;
        g->vid = first->vid; g->pid = first->pid; g->speed = first->speed; g->iface = first->ifnum;
        g->reason = any_abs ? AMV_IGN_ABSOLUTE : AMV_IGN_NOT_MOUSE;
        snprintf(g->name, sizeof g->name, "%.29s", first->product);
        amv_log("[mice] %04x:%04x \"%s\": not used (%s)\n", g->vid, g->pid, g->name, any_abs ? "absolute pointer" : "not a mouse");
        emit(AMV_MEV_ATTACHED, g->uid);
    }
}

static void lifecycle() {
    // departures
    for (auto &m : s_mice) {
        if (!m.used || alive(m.r)) continue;
        int si = m.sess;
        uint16_t uid = m.uid;
        m.used = false;
        if (si >= 0) close_session(si);
        emit(AMV_MEV_REMOVED, uid);
    }
    for (auto &g : s_ign) {
        if (!g.used || alive(g.r)) continue;
        g.used = false;
        emit(AMV_MEV_REMOVED, g.uid);
    }
    for (int i = 0; i < MAX_SESS; i++)
        if (s_sess[i].used && !s_sess[i].dying && !alive(s_sess[i].mouse)) close_session(i);
    for (auto &h : host_ifs) {
        if (!h.used || !h.gone) continue;
        bool referenced = false;
        for (auto &s : s_sess) if (s.used) for (int k = -1; k < s.n_cfg; k++) {
            ifref_t &r = k < 0 ? s.mouse : s.cfg[k];
            if (r.h == &h) referenced = true;
        }
        if (!referenced) h.used = false;     // the record can take the next interface
    }
    // arrivals: once all interfaces of a device have been quiet for SETTLE_MS
    uint32_t now = millis();
    for (auto &h : host_ifs) {
        if (!h.used || h.gone || h.announced || !h.desc_len) continue;
        uint32_t last = 0;
        bool waiting = false;
        for (auto &o : host_ifs) if (o.used && !o.gone && o.dev == h.dev) {
            if (!o.desc_len) waiting = true;
            if (o.claim_ms > last) last = o.claim_ms;
        }
        if (!waiting && now - last >= SETTLE_MS) attach_device(h.dev);
    }
}

static void measure() {
    uint32_t now = millis();
    for (auto &m : s_mice) {
        if (!m.used || !alive(m.r) || now - m.sample_ms < 1000) continue;
        uint32_t reports = m.r.h->motion_reports;
        uint32_t hz = (reports - m.sample_reports) * 1000u / (now - m.sample_ms);
        m.hist[m.hslot++ % RATE_WINDOW] = (uint16_t)(hz > 65535 ? 65535 : hz);
        m.sample_ms = now;
        m.sample_reports = reports;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the engine's job queue: a requested write first, then probes and refreshes when they are due

static void finish_job(int r, const js_status_t *st) {
    int si = s_job_sess;
    int job = s_job;
    s_job = JOB_NONE;
    s_job_sess = -1;
    session_t *s = &s_sess[si];
    if (!s->used) return;
    mouse_t *m = mouse_of_session(si);
    if (job == JOB_CLOSE || s->dying) { s->dying = false; close_session(si); return; }
    bool was_known = s->probed;
    if (r > 0) {
        uint16_t old_cpi = s->cpi, old_hz = s->device_hz; uint8_t old_bat = s->battery, old_link = s->link;
        uint16_t old_lod = s->lod; uint8_t old_ms = s->motion_sync, old_chg = s->charging;
        bool old_err = s->error;
        apply(s, st);
        s->probed = true;
        s->fail_n = 0;
        set_tier(si);
        if (!was_known && m)
            amv_log("[mice] \"%s\": %u CPI, %u Hz, battery %d%%, firmware \"%s\"%s\n", s->name[0] ? s->name : m->r.h->product,
                    s->cpi, s->device_hz, st->battery, s->firmware, s->verified ? "" : " (read-only)");
        bool changed = !was_known || old_cpi != s->cpi || old_hz != s->device_hz || old_bat != s->battery || old_link != s->link ||
                       old_lod != s->lod || old_ms != s->motion_sync || old_chg != s->charging || old_err;
        if (changed && m) emit(AMV_MEV_UPDATED, m->uid);
    } else {
        bool was_err = s->error;
        uint8_t old_link = s->link;
        s->error = true;
        if (s->probed && s->link == AMV_LINK_CONNECTED) s->link = AMV_LINK_ASLEEP;   // a receiver whose mouse went quiet
        if (s->fail_n < 255) s->fail_n++;
        if (m) s->retry_reports = m->r.h->motion_reports + m->r.h->other_reports;
        if (m && (!was_err || old_link != s->link)) emit(AMV_MEV_UPDATED, m->uid);
    }
    // A mouse may apply a write after the driver's own read-back (the Finalmouse does, about a second later), so
    // look again shortly after a write instead of waiting a whole period.
    s->next_ms = millis() + (job == JOB_WRITE ? SETTLE_READ_MS : REFRESH_MS);
    if (job == JOB_WRITE) {
        s_write.done = true;
        s_write.status = r > 0 ? AMV_ST_OK : AMV_ST_DEVICE_ERR;
        if (r > 0 && m) emit(AMV_MEV_UPDATED, m->uid);
    }
}

static void schedule() {
    if (js_call_busy()) {
        js_status_t st;
        int r = js_call_poll(&st);
        if (r != 0) finish_job(r, &st);
        return;
    }
    if (s_job != JOB_NONE) return;
    if (s_write.pending && !s_write.started) {
        session_t *s = &s_sess[s_write.sess];
        s_write.started = true;
        if (!s->used || !s->sid || !js_call_start(s->sid, JS_OP_SET, s_write.setting, s_write.value)) {
            s_write.done = true; s_write.status = AMV_ST_DEVICE_ERR;
            return;
        }
        s_job = JOB_WRITE; s_job_sess = s_write.sess;
        return;
    }
    uint32_t now = millis();
    for (int i = 0; i < MAX_SESS; i++) {
        session_t *s = &s_sess[i];
        if (!s->used || !s->sid || s->dying || (int32_t)(now - s->next_ms) < 0) continue;
        mouse_t *m = mouse_of_session(i);
        if (!s->probed && s->fail_n > 0 && m) {
            // A failed first probe is retried when the mouse has produced reports since (it is awake and in use)
            // or, for a receiver with configuration interfaces, every period; after four failures only every sixth.
            uint32_t reports = m->r.h->motion_reports + m->r.h->other_reports;
            bool activity = reports != s->retry_reports;
            if (!activity && !s->n_cfg) { s->next_ms = now + REFRESH_MS; continue; }
            if (s->fail_n >= 4 && (s->fail_n % 6) != 0) { s->fail_n++; s->next_ms = now + REFRESH_MS; continue; }
        }
        if (js_call_start(s->sid, JS_OP_PROBE, 0, 0)) { s_job = JOB_PROBE; s_job_sess = i; }
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// protocol (same replies as amv_usb_host.c)

static uint8_t op_count(void) { return mice_count(); }

static void fill_info(int slot, uint8_t index, int count, amv_mouse_info_t *o) {
    memset(o, 0, sizeof *o);
    o->index = index;
    o->count = (uint8_t)count;
    o->battery = 0xFF;
    if (slot < 0) {
        const ignored_t *g = &s_ign[-1 - slot];
        o->tier = AMV_TIER_IGNORED;
        o->vid = g->vid; o->pid = g->pid; o->speed = g->speed; o->iface = g->iface; o->reason = g->reason; o->uid = g->uid;
        snprintf(o->name, sizeof o->name, "%.29s", g->name);
        return;
    }
    const mouse_t *m = &s_mice[slot];
    const host_if_t *h = m->r.h;
    const amv_hid_layout_t *L = &h->layout;
    o->tier = m->tier;
    o->flags = (L->uses_report_ids ? AMV_MF_REPORT_IDS : 0) | (L->wheel.present ? AMV_MF_WHEEL : 0) |
               (L->pan.present ? AMV_MF_PAN : 0) | (L->has_keyboard ? AMV_MF_ALSO_KEYBOARD : 0);
    o->vid = h->vid; o->pid = h->pid; o->speed = h->speed; o->iface = h->ifnum; o->uid = m->uid;
    o->nominal_hz = h->nominal_hz;
    for (int k = 0; k < RATE_WINDOW; k++) if (m->hist[k] > o->measured_hz) o->measured_hz = m->hist[k];
    o->num_buttons = L->num_buttons;
    o->x_bits = L->x.bit_size;
    const session_t *s = m->sess >= 0 ? &s_sess[m->sess] : nullptr;
    snprintf(o->name, sizeof o->name, "%.29s", m->tier == AMV_TIER_KNOWN && s && s->name[0] ? s->name : h->product);
    if (s && !s->no_driver) {
        o->driver = AMV_DRV_OPENMOUSE;
        if (s->error) o->flags |= AMV_MF_DRV_ERROR;
    }
    if (m->tier == AMV_TIER_KNOWN && s) {
        if (s->verified) o->flags |= AMV_MF_VERIFIED;
        if (s->can_set_cpi) o->flags |= AMV_MF_CAN_SET_CPI;
        if (s->can_set_rate) o->flags |= AMV_MF_CAN_SET_RATE;
        o->device_hz = s->device_hz;
        o->battery = s->battery;
        if (s->cpi) { o->cpi = s->cpi; o->cpi_source = AMV_CPI_DEVICE; }
    }
    if (!o->cpi && m->cpi_user) { o->cpi = m->cpi_user; o->cpi_source = AMV_CPI_USER; }
}

static uint8_t op_info(uint8_t index, amv_mouse_info_t *out) {
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) {
        memset(out, 0, sizeof *out);
        out->index = index;
        out->count = (uint8_t)count;
        return AMV_ST_NO_MOUSE;
    }
    fill_info(slot, index, count, out);
    return AMV_ST_OK;
}

static const session_t *known_session(int slot) {
    if (slot < 0 || slot == INT32_MIN) return nullptr;
    const mouse_t *m = &s_mice[slot];
    return m->tier == AMV_TIER_KNOWN && m->sess >= 0 ? &s_sess[m->sess] : nullptr;
}

static uint8_t op_caps(uint8_t index, amv_mouse_caps_t *out) {
    memset(out, 0, sizeof *out);
    out->index = index;
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    const session_t *s = known_session(slot);
    if (!s) return AMV_ST_UNSUPPORTED;
    out->driver = AMV_DRV_OPENMOUSE;
    out->n_rates = s->n_rates;
    memcpy(out->rates, s->rates, sizeof out->rates);
    out->cpi_min = s->cpi_min; out->cpi_max = s->cpi_max; out->cpi_step = s->cpi_step;
    snprintf(out->firmware, sizeof out->firmware, "%s", s->firmware);
    return AMV_ST_OK;
}

static uint8_t op_desc(uint8_t index, uint16_t offset, amv_mouse_desc_rep_t *out) {
    memset(out, 0, sizeof *out);
    out->index = index;
    out->offset = offset;
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    if (slot < 0) return AMV_ST_UNSUPPORTED;
    const host_if_t *h = s_mice[slot].r.h;
    out->total = h->desc_len;
    if (offset > h->desc_len) return AMV_ST_BAD_VALUE;
    int n = h->desc_len - offset;
    if (n > (int)sizeof out->data) n = sizeof out->data;
    out->len = (uint8_t)n;
    memcpy(out->data, h->desc + offset, n);
    return AMV_ST_OK;
}

static uint8_t op_model(uint8_t index, amv_mouse_model_t *out) {
    memset(out, 0, sizeof *out);
    out->index = index;
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    uint16_t vid, pid;
    bool driver = false;
    if (slot < 0) { vid = s_ign[-1 - slot].vid; pid = s_ign[-1 - slot].pid; }
    else {
        vid = s_mice[slot].r.h->vid; pid = s_mice[slot].r.h->pid;
        driver = s_mice[slot].sess >= 0 && !s_sess[s_mice[slot].sess].no_driver;
    }
    const amv_catalog_entry_t *e = amv_catalog_find(vid, pid);
    out->state = driver ? AMV_MODEL_DRIVER : e ? AMV_MODEL_KNOWN : AMV_MODEL_UNKNOWN;
    if (e) {
        snprintf(out->brand, sizeof out->brand, "%s", amv_catalog_brand(e));
        snprintf(out->model, sizeof out->model, "%s", e->model);
    }
    return AMV_ST_OK;
}

static void add_setting(amv_settings_rep_t *o, uint8_t id, uint8_t flags, uint16_t value) {
    if (o->count < AMV_SETTINGS_MAX) { o->s[o->count].id = id; o->s[o->count].flags = flags; o->s[o->count].value = value; o->count++; }
}

static uint8_t op_settings(uint8_t index, amv_settings_rep_t *o) {
    memset(o, 0, sizeof *o);
    o->index = index;
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    if (slot < 0) return AMV_ST_UNSUPPORTED;
    const session_t *c = known_session(slot);
    if (c) {
        o->link = c->link;
        if (c->cpi) add_setting(o, AMV_SET_CPI, AMV_SETF_READABLE | (c->can_set_cpi ? AMV_SETF_WRITABLE : 0), c->cpi);
        if (c->device_hz) add_setting(o, AMV_SET_POLLING, AMV_SETF_READABLE | (c->can_set_rate ? AMV_SETF_WRITABLE : 0), c->device_hz);
        if (c->readable & BIT_OF(AMV_SET_LOD))
            add_setting(o, AMV_SET_LOD, AMV_SETF_READABLE | ((c->writable & BIT_OF(AMV_SET_LOD)) ? AMV_SETF_WRITABLE : 0), c->lod);
        if (c->readable & BIT_OF(AMV_SET_MOTION_SYNC))
            add_setting(o, AMV_SET_MOTION_SYNC, AMV_SETF_READABLE | ((c->writable & BIT_OF(AMV_SET_MOTION_SYNC)) ? AMV_SETF_WRITABLE : 0), c->motion_sync);
        if (c->battery != 0xFF) add_setting(o, AMV_SET_BATTERY, AMV_SETF_READABLE, c->battery);
        if (c->readable & BIT_OF(AMV_SET_CHARGING)) add_setting(o, AMV_SET_CHARGING, AMV_SETF_READABLE, c->charging);
        add_setting(o, AMV_SET_LINK, AMV_SETF_READABLE, c->link);
    } else {
        // tiers 2 and 3: the sensor cannot be reached, the CPI is the user's label
        add_setting(o, AMV_SET_CPI, AMV_SETF_READABLE | AMV_SETF_WRITABLE, s_mice[slot].cpi_user);
    }
    return AMV_ST_OK;
}

static uint8_t op_options(uint8_t index, uint8_t setting, amv_options_rep_t *o) {
    memset(o, 0, sizeof *o);
    o->index = index;
    o->setting = setting;
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    if (slot < 0) return AMV_ST_UNSUPPORTED;
    const session_t *c = known_session(slot);
    if (setting == AMV_SET_CPI) {
        o->kind = AMV_OPT_RANGE; o->unit = AMV_UNIT_CPI; o->n = 3;
        o->values[0] = c && c->cpi_min ? c->cpi_min : 50;
        o->values[1] = c && c->cpi_max ? c->cpi_max : 64000;
        o->values[2] = c && c->cpi_step ? c->cpi_step : 50;
    } else if (c && setting == AMV_SET_POLLING && c->n_rates) {
        o->kind = AMV_OPT_LIST; o->unit = AMV_UNIT_HZ; o->n = c->n_rates;
        memcpy(o->values, c->rates, o->n * sizeof o->values[0]);
    } else if (c && setting == AMV_SET_LOD && (c->readable & BIT_OF(AMV_SET_LOD)) && c->n_lod) {
        o->kind = AMV_OPT_LIST; o->unit = AMV_UNIT_LEVEL; o->n = c->n_lod;
        memcpy(o->values, c->lod_opts, o->n * sizeof o->values[0]);
    } else if (c && setting == AMV_SET_MOTION_SYNC && (c->readable & BIT_OF(AMV_SET_MOTION_SYNC))) {
        o->kind = AMV_OPT_LIST; o->unit = AMV_UNIT_BOOL; o->n = 2; o->values[0] = 0; o->values[1] = 1;
    } else {
        return AMV_ST_UNSUPPORTED;
    }
    return AMV_ST_OK;
}

static uint8_t op_write(uint8_t index, uint8_t setting, uint16_t value) {
    int count, slot = resolve(index, &count);
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    if (slot < 0) return AMV_ST_UNSUPPORTED;
    mouse_t *m = &s_mice[slot];
    if (m->tier != AMV_TIER_KNOWN) {
        if (setting != AMV_SET_CPI) return AMV_ST_UNSUPPORTED;
        if (value != 0 && (value < 50 || value > 64000)) return AMV_ST_BAD_VALUE;
        // no protocol to reach the sensor: remember the user's number as a label (per VID:PID, like the ESP32)
        if (!amv_store_label_set(m->r.h->vid, m->r.h->pid, value)) return AMV_ST_STORE_ERR;
        for (auto &o : s_mice) if (o.used && o.r.h->vid == m->r.h->vid && o.r.h->pid == m->r.h->pid) o.cpi_user = value;
        emit(AMV_MEV_UPDATED, m->uid);
        return AMV_ST_OK;
    }
    session_t *s = &s_sess[m->sess];
    if (!s->verified) return AMV_ST_UNSUPPORTED;
    bool ok = false;
    switch (setting) {
    case AMV_SET_CPI:
        if (!s->can_set_cpi) return AMV_ST_UNSUPPORTED;
        ok = value >= (s->cpi_min ? s->cpi_min : 50) && value <= (s->cpi_max ? s->cpi_max : 64000);
        break;
    case AMV_SET_POLLING:
        if (!s->can_set_rate) return AMV_ST_UNSUPPORTED;
        for (int i = 0; i < s->n_rates; i++) ok |= s->rates[i] == value;
        break;
    case AMV_SET_LOD:
        if (!(s->writable & BIT_OF(AMV_SET_LOD))) return AMV_ST_UNSUPPORTED;
        for (int i = 0; i < s->n_lod; i++) ok |= s->lod_opts[i] == value;
        break;
    case AMV_SET_MOTION_SYNC:
        if (!(s->writable & BIT_OF(AMV_SET_MOTION_SYNC))) return AMV_ST_UNSUPPORTED;
        ok = value <= 1;
        break;
    default:
        return AMV_ST_UNSUPPORTED;
    }
    if (!ok) return AMV_ST_BAD_VALUE;
    if (s_write.pending) return AMV_ST_BUSY;
    s_write.pending = true; s_write.started = false; s_write.done = false;
    s_write.sess = m->sess; s_write.setting = setting; s_write.value = value;
    amv_log("[mice] \"%s\": set %s %u\n", s->name, setting == AMV_SET_CPI ? "CPI" : setting == AMV_SET_POLLING ? "polling" :
            setting == AMV_SET_LOD ? "LOD" : "Motion Sync", value);
    return AMV_ST_PENDING;
}

static bool op_write_done(uint8_t *status) {
    if (!s_write.pending || !s_write.done) return false;
    *status = s_write.status;
    s_write.pending = false;
    return true;
}

static const amv_mouse_ops_t s_ops = {
    op_count, op_info, op_caps, op_desc, op_model, op_settings, op_options, op_write, op_write_done,
};

const amv_mouse_ops_t *mice_ops(void) { return &s_ops; }

void mice_begin(bool engine) { s_engine = engine; }

void mice_poll(void) {
    host_poll();
    lifecycle();
    measure();
    if (s_engine) schedule();
}

void mice_print(void) {
    int count;
    resolve(0, &count);
    if (!count) amv_log("no devices on the host port\n");
    for (int i = 0; i < count; i++) {
        amv_mouse_info_t o;
        op_info((uint8_t)i, &o);
        static const char *tiers[] = {"?", "KNOWN", "GENERIC", "BOOT", "IGNORED"};
        amv_log("%d: %04x:%04x if %u \"%s\" %s, %s speed, interval %u Hz, measured %u Hz", i, o.vid, o.pid, o.iface, o.name,
                tiers[o.tier < 5 ? o.tier : 0], o.speed == 2 ? "high" : o.speed == 0 ? "full" : "low", o.nominal_hz, o.measured_hz);
        if (o.tier == AMV_TIER_KNOWN)
            amv_log(", %u CPI, %u Hz, battery %u%s%s", o.cpi, o.device_hz, o.battery, (o.flags & AMV_MF_VERIFIED) ? ", writable" : ", read-only",
                    (o.flags & AMV_MF_DRV_ERROR) ? ", last exchange failed" : "");
        else if (o.driver) amv_log(", OpenMouse driver%s", (o.flags & AMV_MF_DRV_ERROR) ? " (not answering yet)" : "");
        amv_log("\n");
    }
    js_stats_t js;
    js_get_stats(&js);
    if (js.ready)
        amv_log("OpenMouse engine: loaded in %lu ms, heap %u KB (peak %u KB) of %u KB, %d session(s), %lu calls, last %lu ms, max %lu ms\n",
                (unsigned long)js.load_ms, (unsigned)(js.heap_used / 1024), (unsigned)(js.heap_peak / 1024),
                (unsigned)(js.heap_capacity / 1024), js.sessions, (unsigned long)js.calls, (unsigned long)js.call_ms_last,
                (unsigned long)js.call_ms_max);
    else amv_log("OpenMouse engine: %s\n", js.failed ? "failed to load" : "not loaded");
}
