#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * The OpenMouse protocol library's own WebHID drivers (TypeScript, AGPL-3.0), run unchanged on
 * the dongle in the QuickJS-ng engine. The bundle (generated/openmouse.bc) is built by
 * tools/openmouse/gen_js_bundle.sh from OpenMouse and the bridge in js/.
 *
 * One task on core 1 owns the engine; its heap lives in PSRAM. Every call below hands a request
 * to that task and waits for it, so callers keep the same blocking, one-exchange-at-a-time
 * discipline as the C drivers (amv_usb_host's s_drv). Nothing runs in JS between calls: input
 * reports that arrive meanwhile wait in the host's queues and are delivered at the next call.
 */

/* Transport for one attached mouse. `x` is -1 for the mouse's own interface, 0.. for a claimed
 * vendor interface. Report data never includes the report ID; `id` 0 means no report IDs. */
typedef struct {
    esp_err_t (*send)(void *h, int x, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len);   /* type 2 output, 3 feature */
    esp_err_t (*recv_feature)(void *h, int x, uint8_t id, uint8_t *data, uint16_t cap, int *actual);
    /* Next queued input report without waiting: raw bytes (report ID first when the interface
     * uses IDs), its length, or 0 when nothing is queued. *x tells which interface. */
    int       (*poll_input)(void *h, int *x, uint8_t *raw, int cap);
} amv_js_transport_t;

typedef struct {
    int            x;
    const uint8_t *desc;      /* report descriptor; only read during amv_js_open */
    uint16_t       desc_len;
} amv_js_iface_t;

/* OpenMouse's MouseStatus, flattened (js/bridge.js flatten()). */
typedef struct {
    char     brand[16];
    char     name[30];
    char     firmware[16];
    uint16_t cpi, hz;
    uint16_t rates[8];
    uint8_t  n_rates;
    int16_t  battery;        /* percent, -1 unknown */
    uint8_t  charging;       /* AMV_CHG_* */
    uint8_t  link;           /* AMV_LINK_* */
    uint16_t cpi_min, cpi_max, cpi_step;   /* 0 when the driver publishes no range */
    uint8_t  lod;            /* 1 Low, 2 Medium, 3 High; 0 none */
    uint8_t  lods[3];
    uint8_t  n_lods;
    int8_t   motion_sync;    /* 0 / 1, -1 not offered */
    bool     can_cpi, can_rate, can_lod, can_motion_sync;
} amv_js_status_t;

typedef struct {
    bool     ready;
    uint32_t load_ms;        /* bytecode read + module evaluation at boot */
    size_t   bytecode_bytes;
    size_t   heap_used;      /* engine allocations now (PSRAM) */
    size_t   heap_peak;
    uint32_t stack_free;     /* bytes of the engine task's stack never used */
    int      sessions;
} amv_js_stats_t;

/* Starts the engine task and loads the bundle (takes a moment; calls below wait for it). */
esp_err_t amv_js_start(void);

/* A mouse attached: OpenMouse's registry picks a driver from its interfaces. Returns a session
 * id > 0, or 0 when OpenMouse has no driver for it. `brand` receives the driver's brand. */
int amv_js_open(const amv_js_transport_t *t, void *h, uint16_t vid, uint16_t pid, const char *name,
                const amv_js_iface_t *ifs, int n, char *brand, size_t brand_cap);

/* readStatus() through the driver. */
esp_err_t amv_js_probe(int sid, amv_js_status_t *out);

/* Write one setting (AMV_SET_CPI, _POLLING, _LOD, _MOTION_SYNC), then read the status back. */
esp_err_t amv_js_set(int sid, uint8_t id, uint16_t value, amv_js_status_t *out);

/* The mouse left: closes the driver. The transport must stay valid until this returns. */
void amv_js_close(int sid);

void amv_js_get_stats(amv_js_stats_t *out);
