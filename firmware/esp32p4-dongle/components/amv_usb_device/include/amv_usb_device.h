#pragma once
#include "esp_err.h"

/* Moves the Full-Speed OTG controller onto the second USB-C (GPIO24/25) and starts the
 * composite HID device: IF0 mouse (no motion yet), IF1 vendor channel (usage page 0xFF4D). */
esp_err_t amv_usb_device_start(void);
