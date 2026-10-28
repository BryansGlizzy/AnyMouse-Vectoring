/*
 * Tier-1 driver that runs the OpenMouse protocol library's own TypeScript drivers, unchanged,
 * in the QuickJS engine (components/amv_js, AGPL-3.0 upstream). OpenMouse's registry picks the
 * driver from the mouse's interfaces exactly as its WebHID control panel does, so every family
 * OpenMouse supports works without a C port.
 *
 * Each HID interface of the mouse becomes a WebHID HIDDevice: the mouse's own interface through
 * SET/GET_REPORT and its vendor input queue (x = -1), every other HID interface through the
 * claimed-interface transport amv_hl_* (x = 0..). The write policy is unchanged (README, write
 * policy): reads for every model, writes only for models verified through the dongle.
 */
#include <string.h>
#include "esp_log.h"
#include "amv_catalog.h"
#include "amv_js.h"
#include "amv_mouse_drv.h"

static const char *TAG = "drv_openmouse";

#define MAX_SESS   4
#define NO_DRIVER  0xFF      /* ctx.priv[1]: OpenMouse has no driver for this device; don't ask again */

/* Writes allowed (VID << 16 | PID): Basilisk Ultimate wired, the G Pro's Lightspeed receiver. */
static const uint32_t VERIFIED[] = { 0x15320086, 0x046dc547 };

/* A stable copy of the transport part of the context: the host's copies come and go, the
 * engine keeps calling through this one until release. */
typedef struct {
    bool used;
    int sid;
    amv_drv_ctx_t c;
} om_sess_t;

static om_sess_t s_sess[MAX_SESS];

/* ---------- transport (amv_js.h) ---------- */

static esp_err_t om_send(void *h, int x, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len)
{
    amv_drv_ctx_t *c = h;
    if (x >= 0) return type == 3 ? amv_hl_send_feature(c, x, id, data, len) : amv_hl_send_report(c, x, id, data, len);
    uint8_t buf[128];
    uint16_t off = id ? 1 : 0;
    if (len + off > sizeof buf) return ESP_ERR_INVALID_SIZE;
    buf[0] = id;
    memcpy(buf + off, data, len);
    return amv_host_set_report(c, c->mouse_iface, type, id, buf, (uint16_t)(len + off));
}

static esp_err_t om_recv_feature(void *h, int x, uint8_t id, uint8_t *data, uint16_t cap, int *actual)
{
    amv_drv_ctx_t *c = h;
    if (x >= 0) return amv_hl_receive_feature(c, x, id, data, cap, actual);
    uint8_t buf[129];
    uint16_t off = id ? 1 : 0;
    if (cap + off > sizeof buf) return ESP_ERR_INVALID_SIZE;
    int got = 0;
    esp_err_t e = amv_host_get_report(c, c->mouse_iface, 3, id, buf, (uint16_t)(cap + off), &got);
    if (e != ESP_OK) return e;
    int n = got - off;
    if (n < 0) n = 0;
    memcpy(data, buf + off, (size_t)n);
    *actual = n;
    return ESP_OK;
}

static int om_poll_input(void *h, int *x, uint8_t *raw, int cap)
{
    amv_drv_ctx_t *c = h;
    int n = amv_host_read_input(c, raw, cap, 0);      /* the mouse interface: raw, report ID included */
    if (n > 0) { *x = -1; return n; }
    for (int k = 0; k < c->n_cfg; k++) {
        uint8_t id = 0;
        int off = c->cfg[k].report_ids ? 1 : 0;
        n = amv_hl_wait_input(c, k, &id, raw + off, cap - off, 0);
        if (n > 0) {
            if (off) raw[0] = id;
            *x = k;
            return n + off;
        }
    }
    return 0;
}

static const amv_js_transport_t s_transport = {
    .send = om_send, .recv_feature = om_recv_feature, .poll_input = om_poll_input,
};

/* ---------- driver ---------- */

static om_sess_t *sess_of(const amv_drv_ctx_t *c)
{
    int i = c->priv[0] - 1;
    return i >= 0 && i < MAX_SESS && s_sess[i].used ? &s_sess[i] : NULL;
}

static bool om_match(uint16_t vid, uint16_t pid)
{
    const amv_catalog_entry_t *e = amv_catalog_find(vid, pid);
    return e && e->engine != 0;
}

/* Every other HID interface may carry the configuration channel; OpenMouse decides which. */
static bool om_cfg_match(const uint8_t *desc, uint16_t len) { return true; }

#define BIT_OF(id) (1u << (id))

static void apply(amv_drv_ctx_t *c, const amv_js_status_t *st)
{
    bool w = c->verified;
    c->error = false;
    if (st->name[0]) snprintf(c->name, sizeof c->name, "%s", st->name);
    snprintf(c->firmware, sizeof c->firmware, "%s", st->firmware);
    c->cpi = st->cpi;
    c->device_hz = st->hz;
    c->battery = st->battery < 0 ? 0xFF : (uint8_t)(st->battery > 100 ? 100 : st->battery);
    c->n_rates = st->n_rates;
    memcpy(c->rates, st->rates, sizeof c->rates);
    c->cpi_min = st->cpi_min;
    c->cpi_max = st->cpi_max;
    c->cpi_step = st->cpi_step;
    c->charging = st->charging;
    c->link = st->link;
    c->can_set_cpi = w && st->can_cpi;
    c->can_set_rate = w && st->can_rate;
    c->readable = BIT_OF(AMV_SET_CPI) | BIT_OF(AMV_SET_POLLING) | BIT_OF(AMV_SET_LINK) |
                  (st->battery >= 0 ? BIT_OF(AMV_SET_BATTERY) : 0) | (st->charging ? BIT_OF(AMV_SET_CHARGING) : 0);
    c->writable = 0;
    if (st->lod && st->n_lods) {
        c->readable |= BIT_OF(AMV_SET_LOD);
        if (w && st->can_lod) c->writable |= BIT_OF(AMV_SET_LOD);
        c->lod = st->lod;
        c->lod_unit = AMV_UNIT_LEVEL;
        c->n_lod = st->n_lods;
        for (int i = 0; i < st->n_lods; i++) c->lod_opts[i] = st->lods[i];
    }
    if (st->motion_sync >= 0) {
        c->readable |= BIT_OF(AMV_SET_MOTION_SYNC);
        if (w && st->can_motion_sync) c->writable |= BIT_OF(AMV_SET_MOTION_SYNC);
        c->motion_sync = (uint8_t)st->motion_sync;
    }
}

static esp_err_t om_open(amv_drv_ctx_t *c)
{
    int i = 0;
    while (i < MAX_SESS && s_sess[i].used) i++;
    if (i == MAX_SESS) return ESP_ERR_NO_MEM;

    amv_js_iface_t ifs[AMV_DRV_MAX_HID_IFS];
    int n = 0;
    for (int k = 0; k < c->n_hid_ifs; k++) {
        int x = -2;
        if (c->hid_ifs[k].ifnum == c->mouse_iface) x = -1;
        for (int j = 0; j < c->n_cfg; j++) if (c->cfg[j].ifnum == c->hid_ifs[k].ifnum) x = j;
        if (x == -2) continue;              /* not claimed (no IN endpoint): unreachable, as in a browser */
        ifs[n++] = (amv_js_iface_t){ .x = x, .desc = c->hid_ifs[k].desc, .desc_len = c->hid_ifs[k].desc_len };
    }

    om_sess_t *s = &s_sess[i];
    *s = (om_sess_t){ .used = true, .c = *c };
    s->c.n_hid_ifs = 0;                     /* the descriptors are only valid during this call */
    char brand[16];
    int sid = amv_js_open(&s_transport, &s->c, c->vid, c->pid, c->product, ifs, n, brand, sizeof brand);
    if (!sid) {
        s->used = false;
        c->priv[1] = NO_DRIVER;
        ESP_LOGI(TAG, "%04x:%04x: OpenMouse has no driver for these interfaces; generic", c->vid, c->pid);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s->sid = sid;
    c->priv[0] = (uint8_t)(i + 1);
    ESP_LOGI(TAG, "%04x:%04x: OpenMouse %s driver, %d interface(s)", c->vid, c->pid, brand, n);
    return ESP_OK;
}

static esp_err_t om_probe(amv_drv_ctx_t *c)
{
    if (c->priv[1] == NO_DRIVER) return ESP_ERR_NOT_SUPPORTED;
    if (!sess_of(c)) {
        esp_err_t e = om_open(c);
        if (e != ESP_OK) return e;
    }
    uint32_t id = ((uint32_t)c->vid << 16) | c->pid;
    c->verified = false;
    for (size_t k = 0; k < sizeof VERIFIED / sizeof VERIFIED[0]; k++) c->verified |= VERIFIED[k] == id;
    amv_js_status_t st;
    esp_err_t e = amv_js_probe(sess_of(c)->sid, &st);
    if (e != ESP_OK) {
        c->error = true;
        return e;
    }
    apply(c, &st);
    ESP_LOGI(TAG, "\"%s\": %u CPI, %u Hz, battery %d%%, firmware \"%s\"%s", c->name, c->cpi, c->device_hz, st.battery,
             c->firmware, c->verified ? "" : " (model not verified through the dongle: read-only)");
    return ESP_OK;
}

static esp_err_t om_refresh(amv_drv_ctx_t *c)
{
    om_sess_t *s = sess_of(c);
    if (!s) return ESP_ERR_INVALID_STATE;
    amv_js_status_t st;
    esp_err_t e = amv_js_probe(s->sid, &st);
    if (e != ESP_OK) { c->error = true; return e; }
    apply(c, &st);
    return ESP_OK;
}

static esp_err_t om_write(amv_drv_ctx_t *c, uint8_t id, uint16_t value)
{
    om_sess_t *s = sess_of(c);
    if (!s) return ESP_ERR_INVALID_STATE;
    if (!c->verified) return ESP_ERR_NOT_SUPPORTED;
    amv_js_status_t st;
    esp_err_t e = amv_js_set(s->sid, id, value, &st);
    if (e != ESP_OK) { c->error = true; return e; }
    apply(c, &st);
    return ESP_OK;
}

static esp_err_t om_set_cpi(amv_drv_ctx_t *c, uint16_t cpi) { return om_write(c, AMV_SET_CPI, cpi); }
static esp_err_t om_set_rate(amv_drv_ctx_t *c, uint16_t hz) { return om_write(c, AMV_SET_POLLING, hz); }
static esp_err_t om_set_setting(amv_drv_ctx_t *c, uint8_t id, uint16_t value) { return om_write(c, id, value); }

static void om_release(amv_drv_ctx_t *c)
{
    om_sess_t *s = sess_of(c);
    if (!s) return;
    amv_js_close(s->sid);
    s->used = false;
    c->priv[0] = 0;
}

const amv_drv_t amv_drv_openmouse = {
    .id = AMV_DRV_OPENMOUSE,
    .name = "OpenMouse",
    .wants_input = true,
    .match = om_match,
    .probe = om_probe,
    .refresh = om_refresh,
    .set_cpi = om_set_cpi,
    .set_rate = om_set_rate,
    .set_setting = om_set_setting,
    .cfg_match = om_cfg_match,
    .release = om_release,
};
