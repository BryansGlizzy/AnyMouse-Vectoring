#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Persistent profile slots in NVS (namespace "amv"). Each slot is a CRC-protected blob. */

esp_err_t amv_store_init(void);
/* ESP_OK, or ESP_ERR_NOT_FOUND for an empty slot, or ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_VERSION for a bad blob. */
esp_err_t amv_store_load_slot(uint8_t slot, uint32_t *gen, float w[8]);
esp_err_t amv_store_save_slot(uint8_t slot, uint32_t gen, const float w[8]);
esp_err_t amv_store_get_active(uint8_t *slot);   /* ESP_ERR_NOT_FOUND if never set */
esp_err_t amv_store_set_active(uint8_t slot);
esp_err_t amv_store_erase_all(void);

/* CPI the user entered for a mouse without a known protocol, keyed by VID:PID. Label only:
 * it feeds effective-DPI readouts, never the vectoring. ESP_ERR_NOT_FOUND if never set. */
esp_err_t amv_store_get_mouse_cpi(uint16_t vid, uint16_t pid, uint16_t *cpi);
esp_err_t amv_store_set_mouse_cpi(uint16_t vid, uint16_t pid, uint16_t cpi);   /* 0 forgets it */
