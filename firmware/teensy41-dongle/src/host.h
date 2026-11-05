#pragma once
/*
 * Mouse side of the Teensy bridge: every HID interface of every device on the USB host port (hubs included),
 * on top of USBHost_t36.
 *
 * One driver claims every top-level collection of every HID interface, so it receives the raw input reports of
 * all of them (USBHost_t36 only hands raw reports to the first collection's driver). Per interface it keeps
 * the report descriptor, the parsed mouse layout (amv_hid_parse, shared with the ESP32-P4), the endpoints and
 * a queue of input reports for the OpenMouse drivers.
 *
 * Mouse interfaces are decoded, scaled and sent to the PC inside the host interrupt (see pc_out.h). Everything
 * else here runs in loop(): the WebHID-shaped transport (output reports, SET/GET_REPORT) blocks until the
 * transfer completes or times out, which only delays loop(), never the motion.
 */
#include <stdint.h>
#include <stdbool.h>
extern "C" {
#include "amv_hid_parse.h"
}

#define HOST_MAX_IF     12          // interface records (more than parsers: a record outlives its parser briefly)
#define HOST_RING       16          // queued input reports per interface for the drivers
#define HOST_REPORT_MAX 64

enum { HOST_ROLE_OTHER = 0, HOST_ROLE_MOUSE, HOST_ROLE_BOOT, HOST_ROLE_ABSOLUTE };

struct host_if_t {
    volatile bool used;
    volatile bool gone;             // the device left; the loop tears the record down
    bool     announced;             // the mouse manager has seen it
    void    *dev;                   // Device_t *, identifies the physical device
    uint32_t dev_serial;            // changes when a Device_t is reused
    uint8_t  ifnum, subclass, protocol, speed;
    uint16_t vid, pid;
    uint16_t nominal_hz;            // from bInterval and bus speed
    uint16_t in_mps, out_mps;
    bool     has_out;               // interrupt OUT endpoint (else output reports go through SET_REPORT)
    uint16_t desc_len;
    uint8_t  desc[928];
    amv_hid_layout_t layout;
    uint8_t  role;                  // HOST_ROLE_*
    int8_t   src;                   // pc_out source (mice), -1 none
    uint32_t claim_ms;
    char     product[32];
    char     manufacturer[24];
    volatile uint32_t motion_reports;   // reports that carried mouse fields (measured rate)
    volatile uint32_t other_reports;
    volatile bool listen;           // queue non-motion input reports for a driver
    volatile uint8_t r_head, r_count;
    volatile uint32_t r_drops;
    uint8_t  r_len[HOST_RING];
    uint8_t  r_data[HOST_RING][HOST_REPORT_MAX];
};

extern host_if_t host_ifs[HOST_MAX_IF];

void host_begin(void);
void host_task(void);               // USBHost_t36's task; safe to call from inside a blocking transfer wait

// Device_t identity of an interface's device, and the serial number string (for logging).
bool host_if_alive(const host_if_t *h);

// Transport (loop only). Report data excludes the report ID; `id` 0 means no report IDs. 0 on success.
int host_send_output(host_if_t *h, uint8_t id, const uint8_t *data, uint16_t len);
int host_set_report(host_if_t *h, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len);
int host_get_report(host_if_t *h, uint8_t type, uint8_t id, uint8_t *data, uint16_t cap, int *actual);
// Next queued input report (raw: report ID first when the interface uses IDs), 0 if none.
int host_pop_input(host_if_t *h, uint8_t *raw, int cap);
void host_flush_input(host_if_t *h);
int host_set_boot_protocol(host_if_t *h);

// Called by the loop: tears down departed interfaces (returns true if anything changed).
bool host_poll(void);

// Statistics for the console.
struct host_stats_t {
    uint32_t rx, isr_n, isr_busy, isr_min, isr_max, isr_sum;
    uint32_t dt_min, dt_max, hist[8];
    uint64_t dt_sum;
    uint32_t isr_per[4];
    uint32_t scale_n, scale_sum, scale_max;     // cycles in amv_octant_scale
    uint32_t path_n, path_sum, path_max;        // cycles from raw report to frame queued
};
void host_take_stats(host_stats_t *out);
void host_set_weights(const float w[8], bool bypass);
void host_set_itc(unsigned microframes);
unsigned host_get_itc(void);
