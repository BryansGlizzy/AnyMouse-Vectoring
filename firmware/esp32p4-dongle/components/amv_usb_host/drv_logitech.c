/*
 * Logitech HID++ 2.0 driver, ported from OpenMouse src/drivers/logitech/hidpp.ts and
 * src/logitech/index.ts (AGPL-3.0). Reads only: DPI (0x2201 / 0x2202), report rate
 * (0x8060 / 0x8061), battery (0x1004 / 0x1000), the mouse's own name (0x0005), and whether a
 * receiver's mouse is awake.
 *
 * Writes (DPI and report rate, the live features only): a receiver's mouse is first switched to host
 * control (hidpp.ts ensureHostControl, feature 0x8100), then the value is written and read back.
 * Enabled only for models in VERIFIED_PIDS (the G Pro receiver, enabled by the user on 2026-10-26).
 * Not ported: lift-off distance (its level mapping depends on the onboard profile format), and the
 * onboard-profile write that a direct-connect mouse falls back to when it rejects the live rate.
 *
 * Transport: report 0x10 (short, 6 data bytes) or 0x11 (long, 19) on the vendor interface through
 * the amv_hl_* shim, which keeps WebHID's convention: the report ID is separate from the data, so
 * every buffer below starts at the HID++ "device index" byte exactly like OpenMouse's `report`.
 *
 *   [0] device index  [1] feature index  [2] function<<4 | software id  [3..] parameters
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_hid_parse.h"
#include "amv_mouse_drv.h"

static const char *TAG = "drv_logi";

/* Set while a write is in progress: failures are logged at INFO instead of staying silent (reads of a
 * sleeping mouse fail every few seconds and would flood the log). */
static bool s_verbose;
#define WLOG(...) do { if (s_verbose) ESP_LOGI(TAG, __VA_ARGS__); } while (0)

#define LG_VID          0x046D
#define SW_ID           0x05
#define SHORT_ID        0x10
#define LONG_ID         0x11
#define IDX_RECEIVER    0x01
#define IDX_DIRECT      0xFF

#define F_FIRMWARE      0x0003
#define F_NAME          0x0005
#define F_BATTERY_1004  0x1004
#define F_BATTERY_1000  0x1000
#define F_DPI_EXT       0x2202
#define F_DPI           0x2201
#define F_RATE_EXT      0x8061
#define F_RATE          0x8060
#define F_ONBOARD       0x8100

/* driver-private state lives in ctx->priv */
enum { P_DEV, P_DPI, P_DPI_LEGACY, P_RATE, P_RATE_LEGACY, P_BATT, P_BATT_KIND, P_NAME, P_BOLT, P_INIT, P_ONBOARD };

/* Direct-connect product ids (OpenMouse LOGITECH_DIRECT_PRODUCT_IDS): the mouse is the endpoint */
static const uint16_t DIRECT[] = {
    0xc084, 0xc089, 0xc092, 0xc07d, 0xc07e, 0xc07f, 0xc080, 0xc097, 0xc332, 0xc081, 0xc082, 0xc083, 0xc085,
    0xc086, 0xc087, 0xc088, 0xc08b, 0xc08c, 0xc08d, 0xc08e, 0xc08f, 0xc090, 0xc091, 0xc096, 0xc094, 0xc09b,
    0xc0a0, 0xc09a, 0xc095, 0xc098, 0xc099,
};

static bool is_direct(uint16_t pid)
{
    for (size_t i = 0; i < sizeof DIRECT / sizeof DIRECT[0]; i++) if (DIRECT[i] == pid) return true;
    return false;
}

/* Products whose writes are enabled. 0xc547: Lightspeed receiver (PRO X Wireless family). */
static const uint16_t VERIFIED_PIDS[] = { 0xc547 };

static bool is_verified(uint16_t pid)
{
    for (size_t i = 0; i < sizeof VERIFIED_PIDS / sizeof VERIFIED_PIDS[0]; i++) if (VERIFIED_PIDS[i] == pid) return true;
    return false;
}

static bool match(uint16_t vid, uint16_t pid)
{
    (void)pid;
    return vid == LG_VID;
}

/* HID++ over USB: vendor page 0xFF00, usage 1 (short reports) and 2 (long); over Bluetooth 0xFF43 */
static bool cfg_match(const uint8_t *desc, uint16_t len)
{
    return amv_hid_has_collection(desc, len, 0xFF00, 1) || amv_hid_has_collection(desc, len, 0xFF00, 2) ||
           amv_hid_has_collection(desc, len, 0xFF43, 0x0202);
}

typedef struct {
    uint8_t data[20];
    int     len;
} reply_t;

/* Why the last request failed: lets the probe tell "no HID++ 2.0 here" from "silence" */
typedef enum { R_OK, R_TIMEOUT, R_ERR20, R_ERR10, R_SEND } outcome_t;

static bool has_report(const amv_drv_ctx_t *c, uint8_t id)
{
    for (int i = 0; i < c->cfg[0].n_ids; i++) if (c->cfg[0].ids[i] == id) return true;
    return false;
}

/* One HID++ 2.0 request on feature `feat`, function `fn` (already shifted, low nibble 0). */
static outcome_t request_x(amv_drv_ctx_t *c, uint8_t feat, uint8_t fn, const uint8_t *params, int np, reply_t *r,
                           int timeout_ms, bool force_long)
{
    uint8_t dev = c->priv[P_DEV];
    uint8_t out[19] = { dev, feat, (uint8_t)((fn & 0xF0) | SW_ID) };
    memcpy(out + 3, params, np);
    bool use_short = !force_long && np <= 3 && !c->priv[P_BOLT] && has_report(c, SHORT_ID);
    amv_hl_flush_input(c, 0);
    if (amv_hl_send_report(c, 0, use_short ? SHORT_ID : LONG_ID, out, use_short ? 6 : 19) != ESP_OK) return R_SEND;

    int64_t deadline = (int64_t)xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    outcome_t last = R_TIMEOUT;
    for (;;) {
        int64_t left = deadline - (int64_t)xTaskGetTickCount();
        if (left <= 0) return last;
        uint8_t id, d[20];
        int n = amv_hl_wait_input(c, 0, &id, d, sizeof d, (int)(left * portTICK_PERIOD_MS));
        if (n <= 0) { WLOG("feature %02x fn %02x: no answer within %d ms", feat, fn, timeout_ms); return last; }
        if ((id != SHORT_ID && id != LONG_ID) || n < 4 || d[0] != dev) continue;
        if (d[1] == feat && d[2] == ((fn & 0xF0) | SW_ID)) {
            if (r) { memcpy(r->data, d, (size_t)n); memset(r->data + n, 0, sizeof r->data - (size_t)n); r->len = n; }
            return R_OK;
        }
        if (d[1] == 0xFF && n >= 5 && d[2] == feat && d[3] == ((fn & 0xF0) | SW_ID)) {
            ESP_LOGD(TAG, "feature %02x fn %02x: HID++ 2.0 error %02x", feat, fn, d[4]); WLOG("feature %02x fn %02x: HID++ 2.0 error %02x", feat, fn, d[4]);
            return R_ERR20;
        }
        if (d[1] == 0x8F && n >= 5 && d[2] == feat) { last = R_ERR10; ESP_LOGD(TAG, "HID++ 1.0 error %02x", d[4]); WLOG("HID++ 1.0 error %02x", d[4]); return R_ERR10; }
        /* anything else is a notification (wake, battery, buttons): keep waiting */
    }
}

static outcome_t request(amv_drv_ctx_t *c, uint8_t feat, uint8_t fn, const uint8_t *params, int np, reply_t *r,
                         int timeout_ms)
{
    return request_x(c, feat, fn, params, np, r, timeout_ms, false);
}

/* IRoot.getFeature: index of a feature, 0 when the mouse lacks it. */
static int get_feature(amv_drv_ctx_t *c, uint16_t id, int timeout_ms)
{
    reply_t r;
    const uint8_t p[2] = { (uint8_t)(id >> 8), (uint8_t)id };
    if (request(c, 0x00, 0x00, p, 2, &r, timeout_ms) != R_OK) return -1;
    return r.data[3];
}

static bool has_dpi(amv_drv_ctx_t *c)
{
    int e = get_feature(c, F_DPI_EXT, 700);
    if (e > 0) return true;
    return get_feature(c, F_DPI, 700) > 0;
}

/* hidppDeviceIndexCandidates: a receiver's pairing slots first, a direct mouse answers as itself */
static bool resolve_index(amv_drv_ctx_t *c)
{
    static const uint8_t RECV[] = { IDX_RECEIVER, 2, 3, 4, 5, 6, IDX_DIRECT };
    static const uint8_t DIRECT_IDX[] = { IDX_DIRECT, IDX_RECEIVER };
    bool receiver = !is_direct(c->pid);
    const uint8_t *cand = receiver ? RECV : DIRECT_IDX;
    int n = receiver ? (int)sizeof RECV : (int)sizeof DIRECT_IDX;
    const uint8_t p[2] = { F_FIRMWARE >> 8, F_FIRMWARE & 0xFF };
    for (int i = 0; i < n; i++) {
        c->priv[P_DEV] = cand[i];
        /* The mouse is on slot 1 nearly always; the others are probed quickly (an empty slot answers
         * at once with a HID++ 1.0 error, only a sleeping mouse is silent) */
        outcome_t o = request(c, 0x00, 0x00, p, 2, NULL, i == 0 ? 900 : 250);
        ESP_LOGD(TAG, "probe index %u: %s", cand[i], o == R_OK ? "answered" : o == R_TIMEOUT ? "silent" : o == R_ERR20 ? "HID++ 2.0 error" : o == R_ERR10 ? "HID++ 1.0 error" : "send failed");
        if (o == R_SEND) break;                         /* the request never left: every index would fail */
        if (o != R_OK && o != R_ERR20) continue;        /* only a HID++ 2.0 reply counts */
        if (!receiver || has_dpi(c)) return true;       /* a keyboard on a merged receiver has no sensor */
    }
    c->priv[P_DEV] = 0;
    return false;
}

static void read_name(amv_drv_ctx_t *c)
{
    reply_t r;
    int f = get_feature(c, F_NAME, 800);
    if (f <= 0 || request(c, (uint8_t)f, 0x00, NULL, 0, &r, 800) != R_OK) return;
    int total = r.data[3];
    if (total > (int)sizeof c->name - 1) total = (int)sizeof c->name - 1;
    int got = 0;
    for (int off = 0; off < total; off += 16) {
        const uint8_t p = (uint8_t)off;
        if (request(c, (uint8_t)f, 0x10, &p, 1, &r, 800) != R_OK) break;
        int k = total - off < 16 ? total - off : 16;
        memcpy(c->name + got, r.data + 3, (size_t)k);
        got += k;
    }
    c->name[got] = '\0';
}

static int rate_code_to_hz(int code)
{
    static const uint16_t RATES_HZ[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
    return code >= 0 && code < 7 ? RATES_HZ[code] : 0;
}

static uint8_t connection_arg(const amv_drv_ctx_t *c) { return c->priv[P_DEV] == IDX_DIRECT ? 0x00 : 0x01; }

static void read_rates(amv_drv_ctx_t *c)
{
    reply_t r;
    c->n_rates = 0;
    if (!c->priv[P_RATE]) return;
    if (c->priv[P_RATE_LEGACY]) {
        if (request(c, c->priv[P_RATE], 0x00, NULL, 0, &r, 800) != R_OK) return;
        for (int bit = 7; bit >= 0; bit--) {            /* decodeReportRateBitmap, ascending Hz */
            if (r.data[3] & (1 << bit)) c->rates[c->n_rates++] = (uint16_t)(1000 / (bit + 1));
        }
        return;
    }
    const uint8_t conn = connection_arg(c);
    if (request(c, c->priv[P_RATE], 0x10, &conn, 1, &r, 800) != R_OK) return;
    uint16_t flags = (uint16_t)((r.data[3] << 8) | r.data[4]);
    for (int code = 0; code < 7; code++) if (flags & (1 << code)) c->rates[c->n_rates++] = (uint16_t)rate_code_to_hz(code);
    if (c->n_rates && c->rates[c->n_rates - 1] <= 1000 && conn == 0x01 &&
        request(c, c->priv[P_RATE], 0x00, &conn, 1, &r, 800) == R_OK) {
        /* some firmware keeps the wireless capability mask on function 0 */
        uint16_t f2 = (uint16_t)((r.data[3] << 8) | r.data[4]);
        uint8_t n2 = 0;
        uint16_t tmp[8];
        for (int code = 0; code < 7; code++) if (f2 & (1 << code)) tmp[n2++] = (uint16_t)rate_code_to_hz(code);
        if (n2 && tmp[n2 - 1] > 1000) { memcpy(c->rates, tmp, n2 * sizeof tmp[0]); c->n_rates = n2; }
    }
}

static esp_err_t read_state(amv_drv_ctx_t *c)
{
    reply_t r;
    esp_err_t res = ESP_OK;
    if (c->priv[P_DPI]) {
        if (c->priv[P_DPI_LEGACY]) {
            const uint8_t sensor = 0;
            if (request(c, c->priv[P_DPI], 0x20, &sensor, 1, &r, 900) == R_OK) c->cpi = (uint16_t)((r.data[4] << 8) | r.data[5]);
            else res = ESP_FAIL;
        } else if (request(c, c->priv[P_DPI], 0x50, NULL, 0, &r, 900) == R_OK) {
            c->cpi = (uint16_t)((r.data[4] << 8) | r.data[5]);
        } else res = ESP_FAIL;
    }
    if (res == ESP_OK && c->priv[P_RATE]) {
        if (c->priv[P_RATE_LEGACY]) {
            if (request(c, c->priv[P_RATE], 0x10, NULL, 0, &r, 800) == R_OK && r.data[3]) c->device_hz = (uint16_t)(1000 / r.data[3]);
        } else {
            const uint8_t conn = connection_arg(c);
            if (request(c, c->priv[P_RATE], 0x20, &conn, 1, &r, 800) == R_OK) c->device_hz = (uint16_t)rate_code_to_hz(r.data[3]);
        }
    }
    if (res == ESP_OK && c->priv[P_BATT]) {
        if (c->priv[P_BATT_KIND] == 1 && request(c, c->priv[P_BATT], 0x10, NULL, 0, &r, 800) == R_OK) {
            /* 0x1004 unified battery: [state of charge, level, charging status, external power] */
            c->battery = r.data[3] <= 100 ? r.data[3] : 0xFF;
            c->charging = r.data[5] == 0 ? AMV_CHG_DISCHARGING : (r.data[5] == 1 || r.data[5] == 2) ? AMV_CHG_CHARGING :
                          r.data[5] == 3 ? AMV_CHG_FULL : AMV_CHG_UNKNOWN;
        } else if (c->priv[P_BATT_KIND] == 2 && request(c, c->priv[P_BATT], 0x00, NULL, 0, &r, 800) == R_OK) {
            /* 0x1000 battery level status: [discharge level, next level, status] */
            c->battery = r.data[3] <= 100 ? r.data[3] : 0xFF;
            c->charging = r.data[5] == 0 ? AMV_CHG_DISCHARGING : (r.data[5] == 1 || r.data[5] == 2 || r.data[5] == 4) ? AMV_CHG_CHARGING :
                          r.data[5] == 3 ? AMV_CHG_FULL : AMV_CHG_UNKNOWN;
        }
    }
    return res;
}

/* ---- writes ---- */

/* Decode a DPI list (OpenMouse getDpiOptions / readLegacyDpiList): uint16 values, 0 ends it, and a value
 * with the top three bits set is a range (0xE000 | step) from the previous value to the next one.
 * Sets cpi_min / cpi_max / cpi_step and, when `want` is nonzero, reports if that value is offered. */
typedef struct { uint16_t min, max, step; bool found; } dpi_scan_t;

static void parse_dpi_list(const uint8_t *b, int n, uint16_t want, dpi_scan_t *s)
{
    uint16_t last = 0;
    for (int i = 0; i + 1 < n;) {
        uint16_t v = (uint16_t)((b[i] << 8) | b[i + 1]);
        if (v == 0) break;
        if ((v >> 13) == 7) {
            uint16_t step = v & 0x1FFF, to = i + 3 < n ? (uint16_t)((b[i + 2] << 8) | b[i + 3]) : 0;
            if (!last || !to || to <= last || !step) break;
            if (want >= last && want <= to && (want - last) % step == 0) s->found = true;
            if (!s->step || step < s->step) s->step = step;
            if (to > s->max) s->max = to;
            last = to;
            i += 4;
        } else {
            if (v == want) s->found = true;
            if (!s->min || v < s->min) s->min = v;
            if (v > s->max) s->max = v;
            last = v;
            i += 2;
        }
    }
}

/* Reads the mouse's DPI list. Fills cpi_min/max/step when `c` is writable memory; returns whether `want` is offered. */
static bool scan_dpi(amv_drv_ctx_t *c, uint16_t want, bool *ok)
{
    dpi_scan_t s = { 0 };
    uint8_t buf[14 * 6];
    int n = 0;
    reply_t r;
    bool got = false;
    if (!c->priv[P_DPI]) { if (ok) *ok = false; return false; }
    if (c->priv[P_DPI_LEGACY]) {
        const uint8_t sensor = 0;
        if (request(c, c->priv[P_DPI], 0x10, &sensor, 1, &r, 900) == R_OK) {
            n = r.len > 4 ? r.len - 4 : 0;
            memcpy(buf, r.data + 4, (size_t)(n > (int)sizeof buf ? (int)sizeof buf : n));
            got = true;
        }
    } else {
        for (int page = 0; page < 6; page++) {
            const uint8_t p[2] = { 0x00, (uint8_t)page };
            if (request(c, c->priv[P_DPI], 0x20, p, 2, &r, 900) != R_OK) break;
            got = true;
            int k = r.len > 6 ? r.len - 6 : 0;
            if (n + k > (int)sizeof buf) k = (int)sizeof buf - n;
            memcpy(buf + n, r.data + 6, (size_t)k);
            n += k;
            bool end = false;
            for (int i = 1; i < n; i++) if (buf[i - 1] == 0 && buf[i] == 0) end = true;
            if (end) break;
        }
    }
    if (got) parse_dpi_list(buf, n, want, &s);
    if (ok) *ok = got;
    if (!want && got) { c->cpi_min = s.min; c->cpi_max = s.max; c->cpi_step = s.step ? s.step : 50; }
    return s.found;
}

/* hidpp.ts ensureHostControl: a receiver's mouse must be in host mode (0x02) before a live write sticks.
 * Direct-connect mice accept live writes in onboard mode and are left as they are. */
static esp_err_t ensure_host_control(amv_drv_ctx_t *c)
{
    if (is_direct(c->pid) || !c->priv[P_ONBOARD]) return ESP_OK;
    reply_t r;
    if (request(c, c->priv[P_ONBOARD], 0x20, NULL, 0, &r, 1000) != R_OK) return ESP_FAIL;
    if (r.data[3] == 0x02) return ESP_OK;
    const uint8_t host = 0x02;
    if (request(c, c->priv[P_ONBOARD], 0x10, &host, 1, &r, 1000) != R_OK) return ESP_FAIL;   /* refused: onboard only */
    if (request(c, c->priv[P_ONBOARD], 0x20, NULL, 0, &r, 1000) != R_OK || r.data[3] != 0x02) return ESP_FAIL;
    ESP_LOGI(TAG, "mouse switched to host control");
    return ESP_OK;
}

static esp_err_t set_cpi_impl(amv_drv_ctx_t *c, uint16_t cpi)
{
    if (!c->can_set_cpi) return ESP_ERR_NOT_SUPPORTED;
    bool ok;
    if (!scan_dpi(c, cpi, &ok)) return ok ? ESP_ERR_INVALID_ARG : ESP_FAIL;   /* not advertised by the mouse */
    if (ensure_host_control(c) != ESP_OK) return ESP_FAIL;
    reply_t r;
    if (c->priv[P_DPI_LEGACY]) {
        const uint8_t p[3] = { 0x00, (uint8_t)(cpi >> 8), (uint8_t)cpi };
        if (request_x(c, c->priv[P_DPI], 0x30, p, 3, &r, 1500, !is_direct(c->pid)) != R_OK) return ESP_FAIL;
        const uint8_t sensor = 0;
        if (request(c, c->priv[P_DPI], 0x20, &sensor, 1, &r, 1500) != R_OK) return ESP_FAIL;
        c->cpi = (uint16_t)((r.data[4] << 8) | r.data[5]);
    } else {
        if (request(c, c->priv[P_DPI], 0x50, NULL, 0, &r, 1500) != R_OK) return ESP_FAIL;
        const uint8_t lod = r.data[12];
        const uint8_t p[6] = { 0x00, (uint8_t)(cpi >> 8), (uint8_t)cpi, (uint8_t)(cpi >> 8), (uint8_t)cpi, lod };
        if (request_x(c, c->priv[P_DPI], 0x60, p, 6, &r, 1500, true) != R_OK) return ESP_FAIL;
        if (request(c, c->priv[P_DPI], 0x50, NULL, 0, &r, 1500) != R_OK) return ESP_FAIL;
        c->cpi = (uint16_t)((r.data[4] << 8) | r.data[5]);
        if ((uint16_t)((r.data[8] << 8) | r.data[9]) != cpi) return ESP_FAIL;
    }
    return c->cpi == cpi ? ESP_OK : ESP_FAIL;       /* "The mouse kept X instead of Y" */
}

/* Extended report rate write. The mouse re-links its radio when the rate changes, so the write's own
 * answer can be late or missing; OpenMouse treats the mouse's "rate changed" notification (feature
 * event 0x01 carrying the new rate code) as the confirmation and waits up to 6 s for it. Nothing else is
 * sent while waiting. */
static esp_err_t write_rate_ext(amv_drv_ctx_t *c, uint16_t hz, uint8_t code)
{
    const uint8_t feat = c->priv[P_RATE], dev = c->priv[P_DEV];
    uint8_t out[19] = { dev, feat, (uint8_t)(0x30 | SW_ID), code };
    bool use_short = !c->priv[P_BOLT] && has_report(c, SHORT_ID);
    amv_hl_flush_input(c, 0);
    if (amv_hl_send_report(c, 0, use_short ? SHORT_ID : LONG_ID, out, use_short ? 6 : 19) != ESP_OK) { WLOG("rate write: send failed"); return ESP_FAIL; }
    int64_t end = (int64_t)xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    bool acked = false;
    for (;;) {
        int64_t left = end - (int64_t)xTaskGetTickCount();
        if (left <= 0) break;
        uint8_t id, d[20];
        int n = amv_hl_wait_input(c, 0, &id, d, sizeof d, (int)(left * portTICK_PERIOD_MS));
        if (n <= 0) break;
        if ((id != SHORT_ID && id != LONG_ID) || n < 5 || d[0] != dev) continue;
        if (d[1] == feat && d[2] == (0x30 | SW_ID)) { acked = true; WLOG("rate write acknowledged"); continue; }
        if (d[1] == 0xFF && d[2] == feat && d[3] == (0x30 | SW_ID)) { WLOG("rate write refused by the mouse: error %02x", d[4]); return ESP_FAIL; }
        if (d[1] == 0x8F) { WLOG("rate write: HID++ 1.0 error %02x", n >= 5 ? d[4] : 0); return ESP_FAIL; }
        if (d[1] == feat && d[2] == 0x00 && d[3] == 0x01 && d[4] == code) { WLOG("rate change confirmed by the mouse"); c->device_hz = hz; return ESP_OK; }
        WLOG("rate write: ignoring report %02x %02x %02x %02x", d[1], d[2], d[3], d[4]);
    }
    WLOG("rate write: %s, no confirmation within 6 s", acked ? "acknowledged" : "no answer");
    return ESP_FAIL;
}

static esp_err_t set_rate_impl(amv_drv_ctx_t *c, uint16_t hz)
{
    if (!c->can_set_rate) return ESP_ERR_NOT_SUPPORTED;
    bool listed = false;
    for (int i = 0; i < c->n_rates; i++) listed |= c->rates[i] == hz;
    if (!listed) return ESP_ERR_INVALID_ARG;
    if (ensure_host_control(c) != ESP_OK) return ESP_FAIL;
    reply_t r;
    if (c->priv[P_RATE_LEGACY]) {
        const uint8_t ms = (uint8_t)((1000 + hz / 2) / hz);
        if (ms < 1 || ms > 8) return ESP_ERR_INVALID_ARG;
        outcome_t o = request(c, c->priv[P_RATE], 0x20, &ms, 1, &r, 1500);
        if (o != R_OK) {
            /* diagnostics for the lockup: show everything the receiver sends for 10 s, send nothing */
            WLOG("rate write (%u ms) failed with outcome %d; listening for 10 s", ms, (int)o);
            int64_t end = (int64_t)xTaskGetTickCount() + pdMS_TO_TICKS(10000);
            while ((int64_t)xTaskGetTickCount() < end) {
                uint8_t id, d[20];
                int n = amv_hl_wait_input(c, 0, &id, d, sizeof d, 500);
                if (n > 0) WLOG("  rx id %02x len %d: %02x %02x %02x %02x %02x %02x", id, n, d[0], d[1], d[2], d[3], d[4], d[5]);
            }
            return ESP_FAIL;
        }
        if (request(c, c->priv[P_RATE], 0x10, NULL, 0, &r, 1500) != R_OK || r.data[3] != ms) return ESP_FAIL;
        c->device_hz = hz;
        return ESP_OK;
    }
    uint8_t code = 0xFF;
    for (uint8_t i = 0; i < 7; i++) if (rate_code_to_hz(i) == hz) code = i;
    if (code == 0xFF) return ESP_ERR_INVALID_ARG;
    return write_rate_ext(c, hz, code);
}

static esp_err_t set_cpi(amv_drv_ctx_t *c, uint16_t cpi)
{
    s_verbose = true;
    esp_err_t e = set_cpi_impl(c, cpi);
    WLOG("set CPI %u -> %s", cpi, esp_err_to_name(e));
    s_verbose = false;
    return e;
}

static esp_err_t set_rate(amv_drv_ctx_t *c, uint16_t hz)
{
    s_verbose = true;
    esp_err_t e = set_rate_impl(c, hz);
    WLOG("set rate %u -> %s", hz, esp_err_to_name(e));
    s_verbose = false;
    return e;
}

static esp_err_t probe(amv_drv_ctx_t *c)
{
    c->cfg_iface = c->n_cfg ? c->cfg[0].ifnum : -1;
    c->verified = false;                          /* reads only */
    c->readable = c->writable = 0;
    if (!c->n_cfg) return ESP_FAIL;
    bool receiver = !is_direct(c->pid);

    if (!c->priv[P_DEV] && !resolve_index(c)) {
        /* nothing awake behind the receiver (or a keyboard-only receiver): retried on the next refresh */
        c->link = receiver ? AMV_LINK_ASLEEP : AMV_LINK_WIRED;
        ESP_LOGI(TAG, "%04x: no mouse answered on the HID++ channel (%s)", c->pid,
                 receiver ? "asleep or out of range; will retry" : "not HID++ 2.0");
        return ESP_FAIL;
    }
    c->link = receiver ? AMV_LINK_CONNECTED : AMV_LINK_WIRED;

    int f;
    if ((f = get_feature(c, F_DPI_EXT, 900)) > 0) { c->priv[P_DPI] = (uint8_t)f; c->priv[P_DPI_LEGACY] = 0; }
    else if ((f = get_feature(c, F_DPI, 900)) > 0) { c->priv[P_DPI] = (uint8_t)f; c->priv[P_DPI_LEGACY] = 1; }
    if ((f = get_feature(c, F_RATE_EXT, 900)) > 0) { c->priv[P_RATE] = (uint8_t)f; c->priv[P_RATE_LEGACY] = 0; }
    else if ((f = get_feature(c, F_RATE, 900)) > 0) { c->priv[P_RATE] = (uint8_t)f; c->priv[P_RATE_LEGACY] = 1; }
    if ((f = get_feature(c, F_ONBOARD, 900)) > 0) c->priv[P_ONBOARD] = (uint8_t)f;
    if ((f = get_feature(c, F_BATTERY_1004, 900)) > 0) { c->priv[P_BATT] = (uint8_t)f; c->priv[P_BATT_KIND] = 1; }
    else if ((f = get_feature(c, F_BATTERY_1000, 900)) > 0) { c->priv[P_BATT] = (uint8_t)f; c->priv[P_BATT_KIND] = 2; }
    if (!c->priv[P_DPI]) {
        ESP_LOGI(TAG, "%04x: HID++ device without a DPI feature (not a mouse)", c->pid);
        c->priv[P_DEV] = 0;
        return ESP_FAIL;
    }

    read_name(c);
    read_rates(c);
    esp_err_t e = read_state(c);
    scan_dpi(c, 0, NULL);                               /* CPI range for the options reply and write validation */
    c->verified = is_verified(c->pid);
    c->can_set_cpi = c->verified && e == ESP_OK && c->cpi_max > 0;
    /* A rate write once seemed to silence the receiver until it was replugged; it was the mouse asleep (PROGRESS.md, 2026-10-26). */
    c->can_set_rate = c->verified && e == ESP_OK && c->n_rates > 0;
    ESP_LOGI(TAG, "features: dpi %02x%s rate %02x%s onboard %02x; CPI range %u..%u step %u; %u rates", c->priv[P_DPI],
             c->priv[P_DPI_LEGACY] ? " legacy" : "", c->priv[P_RATE], c->priv[P_RATE_LEGACY] ? " legacy" : "", c->priv[P_ONBOARD],
             c->cpi_min, c->cpi_max, c->cpi_step, c->n_rates);
    c->error = e != ESP_OK;
    ESP_LOGI(TAG, "%04x: \"%s\" on index %u (%s): %u CPI (%s), %u Hz, battery %u%%, %s", c->pid, c->name,
             c->priv[P_DEV], c->priv[P_DEV] == IDX_DIRECT ? "direct" : "via receiver", c->cpi,
             c->priv[P_DPI_LEGACY] ? "0x2201" : "0x2202", c->device_hz, c->battery == 0xFF ? 0 : c->battery,
             c->verified ? "writes enabled" : "read-only");
    return e;
}

static esp_err_t refresh(amv_drv_ctx_t *c)
{
    esp_err_t e = read_state(c);
    if (e == ESP_OK) {
        c->link = is_direct(c->pid) ? AMV_LINK_WIRED : AMV_LINK_CONNECTED;
        c->error = false;
    } else if (!is_direct(c->pid)) {
        c->link = AMV_LINK_ASLEEP;                /* keep the last values; they are shown as stale */
        c->error = false;
    } else {
        c->error = true;
    }
    return e;
}

const amv_drv_t amv_drv_logitech = {
    .id = AMV_DRV_LOGITECH, .name = "Logitech", .wants_input = false,
    .match = match, .probe = probe, .refresh = refresh, .set_cpi = set_cpi, .set_rate = set_rate,
    .cfg_match = cfg_match,
};
