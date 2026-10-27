#pragma once
/*
 * Tier-1 mouse drivers: the dongle speaks a mouse's own vendor protocol (ported from the
 * OpenMouse protocol library, AGPL-3.0, github.com/OpenMouse-Project/mouse-protocol) so the
 * Studio and ./anymouse can show and change its real CPI and polling rate even though the PC
 * only sees the dongle.
 *
 * Safety rule (firmware/esp32p4-dongle/README.md, write policy; PLAN.md section 4.2): probe and refresh are read-only; a write happens only when a
 * client asks, and only for models listed as verified through the dongle on real hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "amv_dongle_proto.h"

#define AMV_DRV_MAX_HID_IFS 6
#define AMV_DRV_MAX_CFG     2     /* extra vendor interfaces a driver can talk to besides the mouse's */

/* A vendor/configuration interface the host claimed for a driver (firmware/esp32p4-dongle/README.md, vendor interfaces). `h` is the
 * host's private handle for the transport calls below. */
typedef struct {
    uint8_t ifnum;
    bool    report_ids;   /* the descriptor uses report IDs: input reports start with the ID byte */
    bool    has_out_ep;   /* output reports go to an interrupt OUT endpoint, else SET_REPORT */
    uint8_t n_ids;        /* report IDs the descriptor declares (WebHID's per-collection reportId lists, merged) */
    uint8_t ids[8];
    void   *h;
} amv_drv_cfg_if_t;

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
    amv_drv_cfg_if_t cfg[AMV_DRV_MAX_CFG];   /* claimed vendor interfaces, see amv_drv_t.cfg_match */
    uint8_t        n_cfg;

    /* filled by the driver */
    int            cfg_iface; /* interface the driver talks to, -1 none */
    uint8_t        txid;      /* driver private (Razer transaction id) */
    uint8_t        priv[16];  /* driver private (Logitech: device index, feature indexes) */
    char           name[30];  /* the mouse's own name when it reports one (receivers), else "" */
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

    /* Settings beyond CPI and polling (section 10.2 item 4). Masks are over AMV_SET_* bits. */
    uint16_t       readable, writable;
    uint16_t       lod;           /* AMV_SET_LOD value, in lod_unit */
    uint8_t        lod_unit;      /* AMV_UNIT_* */
    uint16_t       lod_opts[6];
    uint8_t        n_lod;
    uint8_t        motion_sync;   /* 0 / 1 */
    uint8_t        charging;      /* AMV_CHG_* */
    uint8_t        link;          /* AMV_LINK_* */
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
    /* Optional. AMV_SET_LOD / AMV_SET_MOTION_SYNC writes; only called when ctx.writable has the bit. */
    esp_err_t (*set_setting)(amv_drv_ctx_t *c, uint8_t id, uint16_t value);
    /* Optional: does this HID interface (report descriptor) carry the driver's configuration
     * channel? The host claims every such interface besides the mouse's own, before probe. */
    bool      (*cfg_match)(const uint8_t *desc, uint16_t len);
} amv_drv_t;

extern const amv_drv_t amv_drv_razer, amv_drv_finalmouse, amv_drv_gearhub, amv_drv_logitech;

/* Transport provided by amv_usb_host.c. Types: 1 input, 2 output, 3 feature. */
esp_err_t amv_host_set_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len);
esp_err_t amv_host_get_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, uint8_t *data, uint16_t len, int *actual);
/*
 * WebHID-shaped transport over a claimed vendor interface (ctx->cfg[n]). The shape is deliberate:
 * a port reads like the OpenMouse TypeScript it comes from. `report_id` is separate from the data
 * and 0 means "no report IDs", exactly as in WebHID; input reports are queued with the interface's
 * traffic from the mouse and must be matched by the caller (receivers also send unrelated
 * notifications).
 */
esp_err_t amv_hl_send_report(amv_drv_ctx_t *c, int n, uint8_t report_id, const uint8_t *data, uint16_t len);
esp_err_t amv_hl_send_feature(amv_drv_ctx_t *c, int n, uint8_t report_id, const uint8_t *data, uint16_t len);
esp_err_t amv_hl_receive_feature(amv_drv_ctx_t *c, int n, uint8_t report_id, uint8_t *data, uint16_t cap, int *actual);
/* Next input report: returns the data length (0 on timeout), *report_id set. */
int       amv_hl_wait_input(amv_drv_ctx_t *c, int n, uint8_t *report_id, uint8_t *data, int cap, int timeout_ms);
void      amv_hl_flush_input(amv_drv_ctx_t *c, int n);

/* Next vendor input report from the mouse interface (drivers with wants_input), or 0 on timeout. */
int       amv_host_read_input(amv_drv_ctx_t *c, uint8_t *buf, int cap, int timeout_ms);
