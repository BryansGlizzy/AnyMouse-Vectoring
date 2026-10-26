#pragma once
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/* Moves the Full-Speed OTG controller onto the second USB-C (GPIO24/25) and starts the
 * composite HID device: IF0 mouse, IF1 vendor channel (usage page 0xFF4D). */
esp_err_t amv_usb_device_start(void);

/* IF0 mouse output. Only one task may call _send (amv_vector's sender). */
bool amv_usb_device_mouse_ready(void);
int  amv_usb_device_mouse_max_delta(void);    /* 32767 in report protocol, 127 in BIOS boot protocol */
bool amv_usb_device_mouse_send(uint16_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan);
void amv_usb_device_set_mouse_done_cb(void (*cb)(void));   /* called when a mouse report finished sending */
