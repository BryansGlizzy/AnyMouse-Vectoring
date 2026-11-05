/*
 * GearHub 2.4G relay (Attack Shark R3 receiver), ported from src/hid_driver.c (itself from
 * OpenMouse src/gearhub). 64-byte feature report 0x00 on the receiver's configuration interface
 * (usage page 0xFFFF, usage 2), relayed to the mouse with F6/F7/FC handshakes. The relay takes
 * seconds, so it is only read at attach and on request, not every refresh.
 *
 * Not yet verified through the dongle: reads only, writes refused. CPI is not implemented here
 * or in the desktop app.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_hid_parse.h"
#include "amv_mouse_drv.h"

static const char *TAG = "drv_gearhub";

#define GH_VID        0x3151
#define GH_PID_24G    0x402D
#define GH_LEN        64
#define GH_GET_OPT0   0xD3
#define GH_SEL        0xF6
#define GH_STATUS     0xF7
#define GH_NOTICE     0xFC
#define GH_MOUSE      0x05
#define GH_OPT0_RATE  9

static const struct { uint16_t hz; uint8_t code; } CODES[] = {
    { 8000, 129 }, { 4000, 130 }, { 2000, 132 }, { 1000, 1 }, { 500, 2 }, { 250, 4 }, { 125, 8 },
};

static bool match(uint16_t vid, uint16_t pid)
{
    return vid == GH_VID && pid == GH_PID_24G;
}

static bool send(amv_drv_ctx_t *c, const uint8_t *b, size_t n)
{
    uint8_t r[GH_LEN] = { 0 };
    memcpy(r, b, n > GH_LEN ? GH_LEN : n);
    return amv_host_set_report(c, (uint8_t)c->cfg_iface, 3, 0, r, GH_LEN) == ESP_OK;
}

static bool read(amv_drv_ctx_t *c, uint8_t out[GH_LEN])
{
    int got = 0;
    return amv_host_get_report(c, (uint8_t)c->cfg_iface, 3, 0, out, GH_LEN, &got) == ESP_OK && got == GH_LEN;
}

static void stamp(uint8_t *cmd)
{
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum = (uint8_t)(sum + cmd[i]);
    cmd[7] = (uint8_t)(255 - sum);
}

static bool wait_ready(amv_drv_ctx_t *c, bool for_send)
{
    for (int a = 0; a < 8; a++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const uint8_t st = GH_STATUS;
        uint8_t r[GH_LEN];
        if (!send(c, &st, 1)) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!read(c, r)) return false;
        if (for_send ? r[5] == 1 : r[0] == 1) return true;
    }
    return false;
}

static bool get(amv_drv_ctx_t *c, uint8_t id, uint8_t out[GH_LEN])
{
    uint8_t cmd[9] = { id };
    stamp(cmd);
    const uint8_t sel[2] = { GH_SEL, GH_MOUSE };
    if (!send(c, sel, 2)) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!wait_ready(c, true) || !send(c, cmd, sizeof cmd) || !wait_ready(c, false)) return false;
    const uint8_t notice = GH_NOTICE;
    if (!send(c, &notice, 1)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!read(c, out)) return false;
    bool zero = true;
    for (int i = 0; i < GH_LEN; i++) if (out[i]) zero = false;
    return !zero && out[0] == id;
}

static esp_err_t read_rate(amv_drv_ctx_t *c)
{
    uint8_t o[GH_LEN];
    if (!get(c, GH_GET_OPT0, o) && !get(c, GH_GET_OPT0, o)) return ESP_FAIL;
    for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; i++) {
        if (CODES[i].code == o[GH_OPT0_RATE]) { c->device_hz = CODES[i].hz; return ESP_OK; }
    }
    return ESP_FAIL;
}

static esp_err_t probe(amv_drv_ctx_t *c)
{
    static const uint16_t rates[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
    c->verified = false;
    c->cfg_iface = -1;
    for (int i = 0; i < c->n_hid_ifs; i++) {
        if (amv_hid_has_collection(c->hid_ifs[i].desc, c->hid_ifs[i].desc_len, 0xFFFF, 0x0002)) c->cfg_iface = c->hid_ifs[i].ifnum;
    }
    if (c->cfg_iface < 0) {
        ESP_LOGW(TAG, "no configuration interface (usage page 0xFFFF / 2)");
        c->error = true;
        return ESP_FAIL;
    }
    c->n_rates = 7;
    memcpy(c->rates, rates, sizeof rates);
    esp_err_t e = read_rate(c);
    c->error = e != ESP_OK;
    ESP_LOGI(TAG, "R3 receiver: config if %d, %u Hz%s (not verified through the dongle: read-only)", c->cfg_iface,
             c->device_hz, e == ESP_OK ? "" : " (no answer)");
    return e;
}

static esp_err_t refresh(amv_drv_ctx_t *c)
{
    (void)c;
    return ESP_OK;   /* the relay is too slow to poll; probe read it */
}

static esp_err_t set_cpi(amv_drv_ctx_t *c, uint16_t cpi)
{
    (void)c; (void)cpi;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t set_rate(amv_drv_ctx_t *c, uint16_t hz)
{
    (void)hz;
    (void)c;
    return ESP_ERR_NOT_SUPPORTED;   /* the read-modify-write is ported once a receiver is verified through the dongle */
}

const amv_drv_t amv_drv_gearhub = {
    .id = AMV_DRV_GEARHUB, .name = "GearHub", .wants_input = false,
    .match = match, .probe = probe, .refresh = refresh, .set_cpi = set_cpi, .set_rate = set_rate,
};
