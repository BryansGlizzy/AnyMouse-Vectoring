#pragma once
/*
 * Tier-1 mouse drivers: the dongle speaks a mouse's own vendor protocol (ported from the
 * OpenMouse protocol library, AGPL-3.0, github.com/OpenMouse-Project/mouse-protocol) so the
 * Studio and ./anymouse can show and change its real CPI and polling rate even though the PC
 * only sees the dongle.
 *
 * Safety rule (PLAN.md 9.1a): probe and refresh are read-only; a write happens only when a
 * client asks, and only for models listed as verified through the dongle on real hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "amv_dongle_proto.h"

#define AMV_DRV_MAX_HID_IFS 6

typedef struct {
    uint8_t        ifnum;
    const uint8_t *desc;      /* report descriptor (owned by the host component) */
    uint16_t       desc_len;
} amv_drv_hid_if_t;

typedef struct amv_drv_ctx_s {
    /* set by the host before probe */
    void          *dev;       /* usb_device_handle_t */
    uint16_t       vid, pid;
    uint8_t        mouse_iface;
    amv_drv_hid_if_t hid_ifs[AMV_DRV_MAX_HID_IFS];
    uint8_t        n_hid_ifs;
    QueueHandle_t  in_q;      /* vendor input reports (drivers with wants_input) */

    /* filled by the driver */
    int            cfg_iface; /* interface the driver talks to, -1 none */
    uint8_t        txid;      /* driver private (Razer transaction id) */
    bool           extended_polling;
    bool           verified;  /* writes allowed */
    bool           can_set_cpi, can_set_rate;
    bool           error;     /* last exchange failed */
    uint16_t       cpi, device_hz;
    uint8_t        battery;   /* percent, 0xFF unknown */
    uint16_t       rates[8];
    uint8_t        n_rates;
    uint16_t       cpi_min, cpi_max, cpi_step;
    char           firmware[16];
} amv_drv_ctx_t;

typedef struct {
    uint8_t     id;           /* AMV_DRV_* */
    const char *name;
    bool        wants_input;  /* receives the mouse interface's non-motion input reports */
    bool      (*match)(uint16_t vid, uint16_t pid);
    esp_err_t (*probe)(amv_drv_ctx_t *c);     /* read-only */
    esp_err_t (*refresh)(amv_drv_ctx_t *c);   /* read-only, every few seconds */
    esp_err_t (*set_cpi)(amv_drv_ctx_t *c, uint16_t cpi);
    esp_err_t (*set_rate)(amv_drv_ctx_t *c, uint16_t hz);
} amv_drv_t;

extern const amv_drv_t amv_drv_razer, amv_drv_finalmouse, amv_drv_gearhub;

/* Transport provided by amv_usb_host.c. Types: 1 input, 2 output, 3 feature. */
esp_err_t amv_host_set_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len);
esp_err_t amv_host_get_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, uint8_t *data, uint16_t len, int *actual);
/* Next vendor input report from the mouse interface (drivers with wants_input), or 0 on timeout. */
int       amv_host_read_input(amv_drv_ctx_t *c, uint8_t *buf, int cap, int timeout_ms);
