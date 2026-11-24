#pragma once
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define AMV_GHOST_MAX_IFS   2
#define AMV_GHOST_DESC_MAX  1024
#define AMV_GHOST_STR_MAX   48

typedef struct {
    uint8_t  desc[AMV_GHOST_DESC_MAX];
    uint16_t desc_len;
    uint16_t mps_in;
    uint16_t mps_out;
    uint8_t  interval_in;
    uint8_t  interval_out;
} amv_ghost_if_cfg_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    char     manufacturer[AMV_GHOST_STR_MAX];
    char     product[AMV_GHOST_STR_MAX];
    uint8_t  n_if;
    amv_ghost_if_cfg_t ifs[AMV_GHOST_MAX_IFS];
} amv_ghost_cfg_t;

typedef struct {
    int  (*get_report)(uint8_t slot, uint8_t report_type, uint8_t report_id, uint8_t *buf, uint16_t cap);
    void (*set_report)(uint8_t slot, uint8_t report_type, uint8_t report_id, const uint8_t *buf, uint16_t len);
} amv_ghost_ops_t;

/* Moves the Full-Speed OTG controller onto the second USB-C (GPIO24/25) and starts the
 * composite HID device: IF0 mouse, IF1 vendor channel (usage page 0xFF4D), IF2/IF3 Ghost HID. */
esp_err_t amv_usb_device_start(void);

void amv_usb_device_set_ghost_ops(const amv_ghost_ops_t *ops);
void amv_usb_device_set_ghost(const amv_ghost_cfg_t *cfg);
bool amv_usb_device_ghost_send(uint8_t slot, const uint8_t *data, uint16_t len);

/* IF0 mouse output. Only one task may call _send (amv_vector's sender). */
bool amv_usb_device_mouse_ready(void);
int  amv_usb_device_mouse_max_delta(void);    /* 32767 in report protocol, 127 in BIOS boot protocol */
bool amv_usb_device_mouse_send(uint16_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan);
void amv_usb_device_set_mouse_done_cb(void (*cb)(void));   /* called when a mouse report finished sending */
