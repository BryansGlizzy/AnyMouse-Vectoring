#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "amv_dongle_proto.h"

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

/* Devices the host library has enumerated (any class, hubs included), or -1. */
int amv_usb_host_device_count(void);

/* Human-readable snapshot of the High-Speed host port register (bring-up diagnostics). */
void amv_usb_host_port_status(char *out, size_t cap);

/* Snapshot of the claimed mouse interfaces; returns how many were written. */
int amv_usb_host_mice(amv_usb_host_mouse_t *out, int max);

/* ---- attached-device information, backing the GET_MOUSE* / SET_MOUSE_* protocol commands ----
 * Entries: claimed mice (tiers KNOWN, GENERIC, BOOT) first, then ignored devices. Each call
 * returns an AMV_ST_* code. Safe from any task; the setters block while the mouse answers. */
typedef void (*amv_mouse_event_cb_t)(const amv_mouse_event_t *ev);
void    amv_mouse_set_event_cb(amv_mouse_event_cb_t cb);
uint8_t amv_mouse_get_info(uint8_t index, amv_mouse_info_t *out);
uint8_t amv_mouse_get_caps(uint8_t index, amv_mouse_caps_t *out);
uint8_t amv_mouse_get_desc(uint8_t index, uint16_t offset, amv_mouse_desc_rep_t *out);
uint8_t amv_mouse_set_cpi(uint8_t index, uint16_t cpi, amv_mouse_info_t *out);    /* KNOWN: to the mouse; else a stored label (0 forgets) */
uint8_t amv_mouse_set_rate(uint8_t index, uint16_t hz, amv_mouse_info_t *out);
/* Catalog names and the brand-neutral settings list (PLAN.md section 10.2) */
uint8_t amv_mouse_get_model(uint8_t index, amv_mouse_model_t *out);
uint8_t amv_mouse_get_settings(uint8_t index, amv_settings_rep_t *out);
uint8_t amv_mouse_get_options(uint8_t index, uint8_t setting, amv_options_rep_t *out);
uint8_t amv_mouse_set_setting(uint8_t index, uint8_t setting, uint16_t value, amv_settings_rep_t *out);
