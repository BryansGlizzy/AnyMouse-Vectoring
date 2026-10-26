#pragma once
#include <stdint.h>
#include "esp_err.h"

/*
 * Mouse side of the dongle: the High-Speed OTG controller in host mode (USB-A port 1 with the
 * HOST jumper; a hub in that port serves more mice).
 *
 * Every device that attaches is enumerated; each HID interface whose report descriptor has a
 * relative-motion Generic Desktop / Mouse collection is claimed and read at its native rate
 * with several interrupt IN transfers kept in flight. Keyboards, consumer controls, vendor
 * configuration interfaces and absolute pointers are left alone. Each decoded report goes to
 * amv_vector. Unplugging releases the mouse's buttons; the PC-side device is never touched.
 */

/* GPIO that enables the USB-A VBUS switch, or -1 when it is on without help. */
#ifndef AMV_USB_VBUS_EN_GPIO
#define AMV_USB_VBUS_EN_GPIO (-1)
#endif

typedef struct {
    uint16_t vid, pid;
    uint8_t  speed;            /* usb_speed_t: 0 low, 1 full, 2 high */
    uint8_t  iface;
    uint8_t  interval;         /* bInterval of the IN endpoint */
    uint16_t mps;
    uint32_t reports;          /* native reports received */
    uint32_t errors;
    char     name[40];
} amv_usb_host_mouse_t;

esp_err_t amv_usb_host_start(void);

/* Snapshot of the claimed mouse interfaces; returns how many were written. */
int amv_usb_host_mice(amv_usb_host_mouse_t *out, int max);
