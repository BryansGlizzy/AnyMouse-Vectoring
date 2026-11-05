/*
 * Finalmouse UltralightX protocol, ported from src/hid_driver.c (itself from OpenMouse
 * src/finalmouse). Output report 0x04 carries a command; replies arrive as input report 0x05 on
 * the same (combined motion + vendor) interface the dongle already reads.
 *
 * Not yet verified through the dongle: reads only, writes refused.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "amv_mouse_drv.h"

static const char *TAG = "drv_fm";

#define FM_VID        0x361D
#define FM_PID_ULX    0x0100
#define FM_OUT        0x04
#define FM_IN         0x05
#define FM_CMD_DPI    16
#define FM_CMD_RATE   17
#define FM_CMD_WAKE   96

static bool match(uint16_t vid, uint16_t pid)
{
    return vid == FM_VID && pid == FM_PID_ULX;
}

static esp_err_t send_cmd(amv_drv_ctx_t *c, uint8_t cmd, const uint8_t *pl, uint8_t n)
{
    uint8_t r[64] = { FM_OUT, (uint8_t)(2 + n), (uint8_t)(0x80 | (cmd & 0x7F)), n };
    if (n) memcpy(r + 4, pl, n);
    return amv_host_set_report(c, c->mouse_iface, 2, FM_OUT, r, sizeof r);
}

/* wakeAll makes the mouse report everything; collect replies for 350 ms */
static esp_err_t read_all(amv_drv_ctx_t *c)
{
    uint8_t b[64];
    while (amv_host_read_input(c, b, sizeof b, 0) > 0) { }   /* drop stale replies */
    if (send_cmd(c, FM_CMD_WAKE, NULL, 0) != ESP_OK) return ESP_FAIL;
    int64_t end = esp_timer_get_time() + 350000;
    int packets = 0;
    while (esp_timer_get_time() < end) {
        int n = amv_host_read_input(c, b, sizeof b, 40);
        if (n < 4 || b[0] != FM_IN) continue;
        packets++;
        uint8_t cmd = b[2], plen = b[3];
        if (plen > n - 4) plen = (uint8_t)(n - 4);
        const uint8_t *p = b + 4;
        if ((cmd == 3 || cmd == 16) && plen >= 2) c->cpi = (uint16_t)(p[0] | (p[1] << 8));
        else if ((cmd == 4 || cmd == 17) && plen >= 2) c->device_hz = (uint16_t)(p[0] | (p[1] << 8));
        else if (cmd == 5 && plen >= 2) {
            int mv = p[0] | (p[1] << 8), pct = (mv - 3400) * 100 / (4200 - 3400);
            c->battery = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
        } else if (cmd == 12 && plen > 0) {
            size_t k = plen < sizeof c->firmware - 1 ? plen : sizeof c->firmware - 1;
            memcpy(c->firmware, p, k);
            c->firmware[k] = '\0';
        }
    }
    return packets ? ESP_OK : ESP_FAIL;
}

static esp_err_t probe(amv_drv_ctx_t *c)
{
    static const uint16_t rates[] = { 500, 1000, 2000, 4000, 8000 };
    c->cfg_iface = c->mouse_iface;
    c->verified = false;
    c->n_rates = 5;
    memcpy(c->rates, rates, sizeof rates);
    c->cpi_min = 50;
    c->cpi_max = 26000;
    c->cpi_step = 50;
    esp_err_t e = read_all(c);
    c->error = e != ESP_OK;
    ESP_LOGI(TAG, "ULX: %s, %u CPI, %u Hz (not verified through the dongle: read-only)",
             e == ESP_OK ? "answered" : "no answer", c->cpi, c->device_hz);
    return e;
}

static esp_err_t refresh(amv_drv_ctx_t *c)
{
    esp_err_t e = read_all(c);
    c->error = e != ESP_OK;
    return e;
}

static esp_err_t set_cpi(amv_drv_ctx_t *c, uint16_t cpi)
{
    if (!c->can_set_cpi) return ESP_ERR_NOT_SUPPORTED;
    const uint8_t p[2] = { (uint8_t)cpi, (uint8_t)(cpi >> 8) };
    esp_err_t e = send_cmd(c, FM_CMD_DPI, p, 2);
    return e == ESP_OK ? read_all(c) : e;
}

static esp_err_t set_rate(amv_drv_ctx_t *c, uint16_t hz)
{
    if (!c->can_set_rate) return ESP_ERR_NOT_SUPPORTED;
    const uint8_t p[2] = { (uint8_t)hz, (uint8_t)(hz >> 8) };
    esp_err_t e = send_cmd(c, FM_CMD_RATE, p, 2);
    return e == ESP_OK ? read_all(c) : e;
}

const amv_drv_t amv_drv_finalmouse = {
    .id = AMV_DRV_FINALMOUSE, .name = "Finalmouse", .wants_input = true,
    .match = match, .probe = probe, .refresh = refresh, .set_cpi = set_cpi, .set_rate = set_rate,
};
