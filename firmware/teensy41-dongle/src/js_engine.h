#pragma once
/*
 * The OpenMouse protocol library's own WebHID drivers (TypeScript, AGPL-3.0), run unchanged on the Teensy in the
 * QuickJS-ng engine: the same bundle (firmware/esp32p4-dongle/components/amv_js/generated/openmouse.bc) and the
 * same bridge (js/bridge.js, js/hid_device.js) as the ESP32-P4 dongle. Port of amv_js.c without an RTOS:
 *
 *   - The engine's heap is in PSRAM (psram_heap.h); its code and constants stay in flash (linker script).
 *   - There is no engine task. A call (probe, write, close) is started and then advanced from loop() by
 *     js_call_poll() until its promise settles, so loop() keeps serving the vendor channel meanwhile. A driver's
 *     own transfers (send, recv) block inside the native for one USB transfer, a few milliseconds at most.
 *   - One call at a time, as on the ESP32: input reports wait in the host's queues between calls.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport for one attached mouse. `x` is -1 for the mouse's own interface, 0.. for its other HID interfaces.
 * Report data never includes the report ID; `id` 0 means no report IDs. Return 0 on success. */
typedef struct {
    int (*send)(void *h, int x, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len);   /* 2 output, 3 feature */
    int (*recv_feature)(void *h, int x, uint8_t id, uint8_t *data, uint16_t cap, int *actual);
    /* Next queued input report without waiting: raw bytes (report ID first when the interface uses IDs). */
    int (*poll_input)(void *h, int *x, uint8_t *raw, int cap);
} js_transport_t;

typedef struct {
    int            x;
    const uint8_t *desc;      /* only read during js_open */
    uint16_t       desc_len;
} js_iface_t;

/* OpenMouse's MouseStatus, flattened (js/bridge.js flatten()); the same fields as amv_js_status_t. */
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
    uint16_t cpi_min, cpi_max, cpi_step;
    uint8_t  lod;            /* 1 Low, 2 Medium, 3 High; 0 none */
    uint8_t  lods[3];
    uint8_t  n_lods;
    int8_t   motion_sync;    /* 0 / 1, -1 not offered */
    bool     can_cpi, can_rate, can_lod, can_motion_sync;
} js_status_t;

typedef struct {
    bool     ready, failed;
    uint32_t load_ms;
    size_t   bytecode_bytes;
    size_t   heap_used, heap_peak, heap_capacity;
    uint32_t calls, call_ms_last, call_ms_max;
    int      sessions;
} js_stats_t;

enum { JS_OP_PROBE = 1, JS_OP_SET = 2, JS_OP_CLOSE = 3 };

/* Loads the bundle (blocks for a second or so). false if there is no PSRAM or the bundle did not load. */
bool js_begin(void);
bool js_ready(void);

/* A mouse attached: OpenMouse's registry picks a driver from its interfaces. Session id > 0, or 0 when
 * OpenMouse has no driver for it. Synchronous (no I/O). */
int js_open(const js_transport_t *t, void *h, uint16_t vid, uint16_t pid, const char *name,
            const js_iface_t *ifs, int n, char *brand, size_t brand_cap);

/* Starts a call; false while another call is running. */
bool js_call_start(int sid, int op, uint8_t setting, uint16_t value);
bool js_call_busy(void);
/* Advances the running call: 0 still running, 1 finished (status in *st for probe and write), -1 failed. */
int  js_call_poll(js_status_t *st);
/* Drops a session without calling the driver (its device is already gone). */
void js_forget(int sid);

void js_get_stats(js_stats_t *out);
/* Console logging of the drivers' console.* output: 0 off, 1 warnings, 2 all. */
void js_set_log(int level);

#ifdef __cplusplus
}
#endif
