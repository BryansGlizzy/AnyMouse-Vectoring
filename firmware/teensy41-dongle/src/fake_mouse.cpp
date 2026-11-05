/*
 * Console self-test `fake`: OpenMouse's real Finalmouse driver, in the engine on the Teensy, against a simulated
 * Finalmouse UltralightX dongle (361d:0100). The same fake as firmware/esp32p4-dongle/tests/js_bridge/
 * finalmouse_fake.test.mjs: the descriptor the ESP32 captured from the real dongle, output report 4 commands, and
 * the dongle's answers as input reports 5 (and 3) a few milliseconds later. It exercises everything a real mouse
 * would except the USB host transfers: driver choice, readStatus, writes, timers, promises, the input path.
 */
#include <Arduino.h>
#include <string.h>
#include "js_engine.h"

extern "C" void amv_log(const char *fmt, ...);
extern "C" void amv_js_idle_hook(void);

// tests/descriptors/finalmouse_ulx_dongle_361d_0100_if0.hex
static const uint8_t ULX_DESC[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x85,0x01,0x05,0x09,0x19,0x01,
    0x29,0x05,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x05,0x81,0x02,0x75,0x03,0x95,0x01,
    0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x00,0x80,0x26,0xff,0x7f,0x75,0x10,
    0x95,0x02,0x81,0x06,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,
    0xc0,0xc0,0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,0x02,0x19,0x01,0x29,0x01,0x15,
    0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x91,0x02,0x85,0x03,0x19,0x01,0x29,0x01,
    0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x81,0x02,0x85,0x04,0x19,0x01,0x29,
    0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x91,0x02,0x85,0x05,0x19,0x01,
    0x29,0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x81,0x02,0xc0,
};

struct fake_t {
    uint16_t cpi = 3200, hz = 2000;
    uint8_t q[24][64];
    uint8_t q_len[24];
    uint32_t q_due[24];
    unsigned n = 0;
    unsigned sent = 0, sets = 0;
};
static fake_t *F;

static void push(uint8_t id, uint8_t cmd, const uint8_t *payload, uint8_t len) {
    if (F->n == 24) return;
    uint8_t *r = F->q[F->n];
    memset(r, 0, 64);
    r[0] = id; r[1] = (uint8_t)(2 + len); r[2] = cmd; r[3] = len;
    memcpy(r + 4, payload, len);
    F->q_len[F->n] = 64;
    F->q_due[F->n] = millis() + 3;
    F->n++;
}

static void burst() {
    uint8_t v[2];
    v[0] = F->cpi & 255; v[1] = F->cpi >> 8; push(5, 3, v, 2);
    v[0] = F->hz & 255; v[1] = F->hz >> 8; push(5, 4, v, 2);
    v[0] = 3880 & 255; v[1] = 3880 >> 8; push(5, 5, v, 2);
    push(5, 12, (const uint8_t *)"1.2.3", 6);
    uint8_t rssi = 0xed; push(5, 13, &rssi, 1);
    uint8_t ms = 1; push(5, 18, &ms, 1);
    push(3, 0x0b, (const uint8_t *)"dongle", 7);
}

static int f_send(void *, int x, uint8_t type, uint8_t id, const uint8_t *d, uint16_t len) {
    F->sent++;
    if (x != -1 || type != 2) return -1;
    if (id == 4 && len >= 4) {
        uint8_t cmd = d[1] & 0x7F;
        if (cmd == 0x60) burst();                                   // wakeAll: the settings burst
        else if (cmd == 16 && d[2] >= 2) { F->cpi = d[3] | (d[4] << 8); F->sets++; }   // DPI
        else if (cmd == 17 && d[2] >= 2) { F->hz = d[3] | (d[4] << 8); F->sets++; }    // polling rate
    }
    return 0;
}
static int f_recv(void *, int, uint8_t, uint8_t *, uint16_t, int *actual) { *actual = 0; return -1; }
static int f_poll(void *, int *x, uint8_t *raw, int cap) {
    if (!F->n || (int32_t)(millis() - F->q_due[0]) < 0) return 0;
    int n = F->q_len[0] < cap ? F->q_len[0] : cap;
    memcpy(raw, F->q[0], n);
    for (unsigned i = 1; i < F->n; i++) { memcpy(F->q[i - 1], F->q[i], 64); F->q_len[i - 1] = F->q_len[i]; F->q_due[i - 1] = F->q_due[i]; }
    F->n--;
    *x = -1;
    return n;
}
static const js_transport_t T = { f_send, f_recv, f_poll };

// Runs one call to the end (blocking the console; motion keeps flowing in interrupts).
static int run(int sid, int op, uint8_t id, uint16_t value, js_status_t *st, uint32_t *ms) {

    uint32_t t0 = millis();
    if (!js_call_start(sid, op, id, value)) return -2;
    int r;
    while ((r = js_call_poll(st)) == 0) amv_js_idle_hook();
    *ms = millis() - t0;
    return r;
}

static void show(const char *what, int r, const js_status_t *st, uint32_t ms) {
    if (r <= 0) { amv_log("  %-22s FAILED (%d) after %lu ms\n", what, r, (unsigned long)ms); return; }
    amv_log("  %-22s %lu ms: \"%s\" %u CPI, %u Hz, battery %d%%, Motion Sync %d, firmware \"%s\", rates", what, (unsigned long)ms,
            st->name, st->cpi, st->hz, st->battery, st->motion_sync, st->firmware);
    for (int i = 0; i < st->n_rates; i++) amv_log(" %u", st->rates[i]);
    amv_log(", CPI %u..%u step %u, can set CPI %d rate %d\n", st->cpi_min, st->cpi_max, st->cpi_step, st->can_cpi, st->can_rate);
}

void fake_mouse_test(void) {
    if (!js_ready()) { amv_log("OpenMouse engine not loaded\n"); return; }
    if (js_call_busy()) { amv_log("engine busy with a real mouse; try again\n"); return; }
    static fake_t f;
    f = fake_t();
    F = &f;
    js_iface_t ifs[1] = { { -1, ULX_DESC, sizeof ULX_DESC } };
    char brand[16];
    uint32_t t0 = millis();
    int sid = js_open(&T, nullptr, 0x361d, 0x0100, "UltralightX dongle", ifs, 1, brand, sizeof brand);
    amv_log("fake Finalmouse ULX dongle: open %s (%s) in %lu ms\n", sid ? "ok" : "FAILED", brand, (unsigned long)(millis() - t0));
    if (!sid) return;
    js_status_t st;
    uint32_t ms;
    int r = run(sid, JS_OP_PROBE, 0, 0, &st, &ms);
    show("readStatus", r, &st, ms);
    bool ok = r > 0 && st.cpi == 3200 && st.hz == 2000 && st.motion_sync == 1 && st.battery > 0;
    r = run(sid, JS_OP_SET, 2, 8000, &st, &ms);
    show("set polling 8000", r, &st, ms);
    ok = ok && r > 0 && f.hz == 8000 && st.hz == 8000;
    r = run(sid, JS_OP_SET, 1, 1600, &st, &ms);
    show("set CPI 1600", r, &st, ms);
    ok = ok && r > 0 && f.cpi == 1600 && st.cpi == 1600;
    r = run(sid, JS_OP_CLOSE, 0, 0, &st, &ms);
    amv_log("  close                  %s in %lu ms; %u reports sent to the fake, %u writes\n", r > 0 ? "ok" : "FAILED",
            (unsigned long)ms, f.sent, f.sets);
    js_stats_t js;
    js_get_stats(&js);
    amv_log("fake mouse test %s (engine heap %u KB, peak during calls %u KB)\n", ok ? "PASSED" : "FAILED",
            (unsigned)(js.heap_used / 1024), (unsigned)(js.heap_peak / 1024));
    F = nullptr;
}
