/*
 * Razer control protocol, ported from OpenMouse src/razer/codec.ts and
 * src/drivers/razer/hid.ts (AGPL-3.0). 90-byte feature reports, report ID 0, on the interface
 * whose collection is Generic Desktop / Mouse.
 *
 *   [0] status  [1] transaction id  [2..4] remaining/protocol  [5] data size
 *   [6] command class  [7] command id  [8..87] arguments  [88] XOR of bytes 2..87  [89] 0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_mouse_drv.h"

static const char *TAG = "drv_razer";

#define RZ_VID       0x1532
#define RZ_LEN       90
#define RZ_ARGS      8
#define RZ_CRC       88
#define RZ_OK        0x02
#define RZ_BUSY      0x01
#define RZ_STORE     0x01      /* VARSTORE, as OpenMouse uses for reads and writes */
#define RZ_ATTEMPTS  6

/* Transaction ids per product (OpenMouse src/razer/devices.ts). A wrong id is silent. */
static const uint16_t TX_3F[] = {
    0x0050, 0x0059, 0x005a, 0x005c, 0x0060, 0x0064, 0x0065, 0x006f, 0x0070, 0x0072, 0x0073, 0x007c,
    0x007d, 0x0084, 0x008c, 0x007a, 0x007b, 0x006e, 0x0071, 0x0098,
};
static const uint16_t TX_1F[] = {
    0x0062, 0x006c, 0x0077, 0x0080, 0x0085, 0x0086, 0x0088, 0x008d, 0x008f, 0x0090, 0x0094, 0x0096,
    0x0099, 0x009a, 0x009c, 0x009e, 0x009f, 0x00a1, 0x00a4, 0x00a5, 0x00a6, 0x00a7, 0x00a8, 0x00aa,
    0x00ab, 0x00af, 0x00b0, 0x00b2, 0x00b3, 0x00b4, 0x00b6, 0x00b7, 0x00b8, 0x00b9, 0x00be, 0x00bf,
    0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c7, 0x00c8, 0x00cb, 0x00cc, 0x00cd, 0x00d0,
    0x00d1, 0x00d3, 0x00d4, 0x00d6, 0x00d7, 0x00de, 0x00df, 0x00ef, 0x00f0,
};

/* Models whose reads and writes have been confirmed through the dongle on real hardware */
static const uint16_t VERIFIED[] = { 0x0086 /* Basilisk Ultimate (wired) */ };

/* Max CPI where OpenMouse knows it (DPI_FOCUS = 20000 for the Focus+ sensor family) */
static uint16_t max_cpi(uint16_t pid)
{
    switch (pid) {
    case 0x0086: case 0x0088: return 20000;
    default: return 35000;
    }
}

static bool in_list(const uint16_t *l, size_t n, uint16_t v)
{
    for (size_t i = 0; i < n; i++) if (l[i] == v) return true;
    return false;
}

static bool match(uint16_t vid, uint16_t pid)
{
    (void)pid;
    return vid == RZ_VID;
}

static uint8_t checksum(const uint8_t *p)
{
    uint8_t c = 0;
    for (int i = 2; i < RZ_CRC; i++) c ^= p[i];
    return c;
}

/* One exchange. Returns ESP_OK with args copied, ESP_ERR_NOT_SUPPORTED, or ESP_FAIL. */
static esp_err_t exchange(amv_drv_ctx_t *c, uint8_t cls, uint8_t id, uint8_t size, const uint8_t *args, int nargs,
                          uint8_t out_args[80])
{
    uint8_t req[RZ_LEN] = { 0 };
    req[1] = c->txid;
    req[5] = size;
    req[6] = cls;
    req[7] = id;
    if (nargs) memcpy(req + RZ_ARGS, args, nargs);
    req[RZ_CRC] = checksum(req);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (amv_host_set_report(c, (uint8_t)c->cfg_iface, 3, 0, req, RZ_LEN) != ESP_OK) return ESP_FAIL;
        for (int poll = 0; poll < RZ_ATTEMPTS; poll++) {
            vTaskDelay(pdMS_TO_TICKS(poll == 0 ? 40 : 60));
            uint8_t rep[RZ_LEN];
            int got = 0;
            if (amv_host_get_report(c, (uint8_t)c->cfg_iface, 3, 0, rep, RZ_LEN, &got) != ESP_OK || got != RZ_LEN) break;
            if (rep[RZ_CRC] != checksum(rep)) break;              /* corrupt: resend */
            if (rep[0] == RZ_BUSY) continue;
            if (rep[0] == 0x05) return ESP_ERR_NOT_SUPPORTED;
            if (rep[0] != RZ_OK) return ESP_FAIL;
            if (rep[6] != cls || rep[7] != id) {
                if (id & 0x80) break;                            /* stale reply to a getter: resend */
                return ESP_FAIL;                                 /* never resend a setter blind */
            }
            if (out_args) memcpy(out_args, rep + RZ_ARGS, 80);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t read_dpi(amv_drv_ctx_t *c)
{
    uint8_t a[80];
    const uint8_t arg = RZ_STORE;
    esp_err_t e = exchange(c, 0x04, 0x85, 0x07, &arg, 1, a);
    if (e == ESP_OK) c->cpi = (uint16_t)((a[1] << 8) | a[2]);     /* X; Y is a[3..4] */
    return e;
}

static esp_err_t read_polling(amv_drv_ctx_t *c)
{
    uint8_t a[80];
    if (!c->extended_polling && exchange(c, 0x00, 0x85, 0x01, NULL, 0, a) == ESP_OK && a[0]) {
        c->device_hz = (uint16_t)(1000 / a[0]);
        return ESP_OK;
    }
    const uint8_t arg = 0x00;
    if (exchange(c, 0x00, 0xC0, 0x02, &arg, 1, a) == ESP_OK && a[1]) {
        c->extended_polling = true;
        c->device_hz = (uint16_t)(8000 / a[1]);
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void read_battery(amv_drv_ctx_t *c)
{
    uint8_t a[80];
    c->battery = exchange(c, 0x07, 0x80, 0x02, NULL, 0, a) == ESP_OK ? (uint8_t)((a[1] * 100 + 127) / 255) : 0xFF;
}

static esp_err_t probe(amv_drv_ctx_t *c)
{
    c->cfg_iface = c->mouse_iface;   /* the Generic Desktop / Mouse interface carries the control channel */
    c->txid = in_list(TX_3F, sizeof TX_3F / 2, c->pid) ? 0x3F : in_list(TX_1F, sizeof TX_1F / 2, c->pid) ? 0x1F : 0xFF;
    c->verified = in_list(VERIFIED, sizeof VERIFIED / 2, c->pid);
    c->cpi_min = 100;
    c->cpi_max = max_cpi(c->pid);
    c->cpi_step = 50;

    uint8_t a[80];
    if (exchange(c, 0x00, 0x81, 0x02, NULL, 0, a) != ESP_OK) {
        ESP_LOGW(TAG, "%04x: no answer on transaction id 0x%02x", c->pid, c->txid);
        c->error = true;
        return ESP_FAIL;
    }
    snprintf(c->firmware, sizeof c->firmware, "%u.%u", a[0], a[1]);
    esp_err_t e = read_dpi(c);
    if (read_polling(c) == ESP_OK) {
        static const uint16_t r1k[] = { 125, 500, 1000 }, r8k[] = { 125, 500, 1000, 2000, 4000, 8000 };
        const uint16_t *r = c->extended_polling ? r8k : r1k;
        c->n_rates = c->extended_polling ? 6 : 3;
        memcpy(c->rates, r, c->n_rates * sizeof r[0]);
    }
    read_battery(c);
    c->can_set_cpi = c->verified && e == ESP_OK;
    c->can_set_rate = c->verified && c->n_rates > 0;
    c->error = e != ESP_OK;
    ESP_LOGI(TAG, "%04x: firmware %s, %u CPI, %u Hz%s, battery %s%s", c->pid, c->firmware, c->cpi, c->device_hz,
             c->extended_polling ? " (extended)" : "", c->battery == 0xFF ? "n/a" : "read",
             c->verified ? "" : " (model not verified through the dongle: read-only)");
    return e;
}

static esp_err_t refresh(amv_drv_ctx_t *c)
{
    esp_err_t e = read_dpi(c);        /* the mouse's own DPI button changes it */
    c->error = e != ESP_OK;
    return e;
}

static esp_err_t set_cpi(amv_drv_ctx_t *c, uint16_t cpi)
{
    if (!c->can_set_cpi) return ESP_ERR_NOT_SUPPORTED;
    if (cpi < c->cpi_min || cpi > c->cpi_max) return ESP_ERR_INVALID_ARG;
    const uint8_t args[7] = { RZ_STORE, (uint8_t)(cpi >> 8), (uint8_t)cpi, (uint8_t)(cpi >> 8), (uint8_t)cpi, 0, 0 };
    esp_err_t e = exchange(c, 0x04, 0x05, 0x07, args, 7, NULL);
    if (e == ESP_OK) e = read_dpi(c);
    c->error = e != ESP_OK;
    return e == ESP_OK && c->cpi != cpi ? ESP_FAIL : e;
}

static esp_err_t set_rate(amv_drv_ctx_t *c, uint16_t hz)
{
    if (!c->can_set_rate) return ESP_ERR_NOT_SUPPORTED;
    bool ok = false;
    for (int i = 0; i < c->n_rates; i++) if (c->rates[i] == hz) ok = true;
    if (!ok) return ESP_ERR_INVALID_ARG;
    esp_err_t e;
    if (c->extended_polling) {
        const uint8_t args[2] = { 0x00, (uint8_t)(8000 / hz) };
        e = exchange(c, 0x00, 0x40, 0x02, args, 2, NULL);
    } else {
        const uint8_t arg = (uint8_t)(1000 / hz);
        e = exchange(c, 0x00, 0x05, 0x01, &arg, 1, NULL);
    }
    if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(150));   /* OpenMouse POLLING_LINK_SETTLE_MS */
        e = read_polling(c);
    }
    c->error = e != ESP_OK;
    return e == ESP_OK && c->device_hz != hz ? ESP_FAIL : e;
}

const amv_drv_t amv_drv_razer = {
    .id = AMV_DRV_RAZER, .name = "Razer", .wants_input = false,
    .match = match, .probe = probe, .refresh = refresh, .set_cpi = set_cpi, .set_rate = set_rate,
};
