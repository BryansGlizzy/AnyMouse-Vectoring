#include "amv_store.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "amv_dongle_proto.h"

#define NS            "amv"
#define BLOB_MAGIC    0x50564D41u   /* 'AMVP' little-endian */
#define BLOB_SCHEMA   1

static const char *TAG = "amv_store";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint32_t gen;
    float    w[8];
    uint32_t crc;      /* CRC32 over every preceding byte */
} slot_blob_t;

static void slot_key(uint8_t slot, char key[8])
{
    snprintf(key, 8, "slot%u", (unsigned)slot);
}

esp_err_t amv_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s); previous contents are discarded", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t amv_store_load_slot(uint8_t slot, uint32_t *gen, float w[8])
{
    if (slot >= AMV_NUM_SLOTS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;

    char key[8];
    slot_key(slot, key);
    slot_blob_t b;
    size_t len = sizeof(b);
    err = nvs_get_blob(h, key, &b, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (len != sizeof(b) || b.magic != BLOB_MAGIC) return ESP_ERR_INVALID_VERSION;
    if (b.schema != BLOB_SCHEMA) return ESP_ERR_INVALID_VERSION;
    if (esp_rom_crc32_le(0, (const uint8_t *)&b, sizeof(b) - sizeof(b.crc)) != b.crc) return ESP_ERR_INVALID_CRC;

    *gen = b.gen;
    memcpy(w, b.w, sizeof(b.w));
    return ESP_OK;
}

esp_err_t amv_store_save_slot(uint8_t slot, uint32_t gen, const float w[8])
{
    if (slot >= AMV_NUM_SLOTS) return ESP_ERR_INVALID_ARG;
    slot_blob_t b = { .magic = BLOB_MAGIC, .schema = BLOB_SCHEMA, .gen = gen };
    memcpy(b.w, w, sizeof(b.w));
    b.crc = esp_rom_crc32_le(0, (const uint8_t *)&b, sizeof(b) - sizeof(b.crc));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[8];
    slot_key(slot, key);
    err = nvs_set_blob(h, key, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t amv_store_get_active(uint8_t *slot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, "active", slot);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err == ESP_OK && *slot >= AMV_NUM_SLOTS) return ESP_ERR_INVALID_STATE;
    return err;
}

esp_err_t amv_store_set_active(uint8_t slot)
{
    if (slot >= AMV_NUM_SLOTS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "active", slot);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t amv_store_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void mouse_key(uint16_t vid, uint16_t pid, char key[16])
{
    snprintf(key, 16, "c%04x%04x", vid, pid);
}

esp_err_t amv_store_get_mouse_cpi(uint16_t vid, uint16_t pid, uint16_t *cpi)
{
    char key[16];
    mouse_key(vid, pid, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    err = nvs_get_u16(h, key, cpi);
    nvs_close(h);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t amv_store_set_mouse_cpi(uint16_t vid, uint16_t pid, uint16_t cpi)
{
    char key[16];
    mouse_key(vid, pid, key);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = cpi ? nvs_set_u16(h, key, cpi) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
