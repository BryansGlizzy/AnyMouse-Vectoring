#include "amv_store_teensy.h"
#include <string.h>
#include <cmath>

uint32_t amv_store_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
        }
    }
    return ~crc;
}

static bool validate_weights(const float w[8]) {
    for (int i = 0; i < 8; i++) {
        float val = w[i];
        if (!std::isfinite(val) || val < (float)AMV_WEIGHT_MIN || val > (float)AMV_WEIGHT_MAX) {
            return false;
        }
    }
    return true;
}

#ifdef ARDUINO
#include <EEPROM.h>

static bool eeprom_read(uint16_t addr, void *buf, size_t n) {
    if ((size_t)addr + n > AMV_STORE_EEPROM_SIZE) return false;
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        p[i] = EEPROM.read(addr + (uint16_t)i);
    }
    return true;
}

static bool eeprom_write(uint16_t addr, const void *buf, size_t n) {
    if ((size_t)addr + n > AMV_STORE_EEPROM_SIZE) return false;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        EEPROM.update(addr + (uint16_t)i, p[i]);
    }
    return true;
}

static const amv_store_backend_t s_default_backend = {
    eeprom_read,
    eeprom_write,
};
#else
static uint8_t s_ram_eeprom[AMV_STORE_EEPROM_SIZE];

static bool ram_default_read(uint16_t addr, void *buf, size_t n) {
    if ((size_t)addr + n > AMV_STORE_EEPROM_SIZE) return false;
    memcpy(buf, &s_ram_eeprom[addr], n);
    return true;
}

static bool ram_default_write(uint16_t addr, const void *buf, size_t n) {
    if ((size_t)addr + n > AMV_STORE_EEPROM_SIZE) return false;
    memcpy(&s_ram_eeprom[addr], buf, n);
    return true;
}

static const amv_store_backend_t s_default_backend = {
    ram_default_read,
    ram_default_write,
};
#endif

static const amv_store_backend_t *s_active_backend = &s_default_backend;

void amv_store_set_backend(const amv_store_backend_t *backend) {
    if (backend && backend->read && backend->write) {
        s_active_backend = backend;
    } else {
        s_active_backend = &s_default_backend;
    }
}

const amv_store_backend_t *amv_store_get_backend(void) {
    return s_active_backend;
}

static bool backend_read(uint16_t addr, void *buf, size_t n) {
    if (!s_active_backend || !s_active_backend->read) return false;
    return s_active_backend->read(addr, buf, n);
}

static bool backend_write(uint16_t addr, const void *buf, size_t n) {
    if (!s_active_backend || !s_active_backend->write) return false;
    return s_active_backend->write(addr, buf, n);
}

static bool store_load(uint8_t slot, amv_profile_t *p) {
    if (slot >= AMV_NUM_SLOTS || !p) return false;
    uint16_t addr = (uint16_t)(offsetof(amv_store_layout_t, slots) + slot * sizeof(amv_slot_record_t));
    amv_slot_record_t rec;
    if (!backend_read(addr, &rec, sizeof(rec))) return false;
    if (rec.magic != AMV_STORE_SLOT_MAGIC || rec.schema != AMV_STORE_SCHEMA) return false;
    uint32_t expected_crc = amv_store_crc32(&rec, sizeof(rec) - sizeof(rec.crc));
    if (rec.crc != expected_crc) return false;
    for (int i = 0; i < 8; i++) {
        float val = rec.w[i];
        if (!std::isfinite(val) || val < (float)AMV_WEIGHT_MIN || val > (float)AMV_WEIGHT_MAX) {
            return false;
        }
    }
    memcpy(p->w, rec.w, sizeof(p->w));
    return true;
}

static bool store_save(uint8_t slot, const amv_profile_t *p) {
    if (slot >= AMV_NUM_SLOTS || !p) return false;
    if (!validate_weights(p->w)) return false;
    uint16_t addr = (uint16_t)(offsetof(amv_store_layout_t, slots) + slot * sizeof(amv_slot_record_t));
    amv_slot_record_t rec;
    rec.magic = AMV_STORE_SLOT_MAGIC;
    rec.schema = AMV_STORE_SCHEMA;
    rec.reserved = 0;
    memcpy(rec.w, p->w, sizeof(rec.w));
    rec.crc = amv_store_crc32(&rec, sizeof(rec) - sizeof(rec.crc));
    return backend_write(addr, &rec, sizeof(rec));
}

static uint8_t store_valid_mask(void) {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < AMV_NUM_SLOTS; i++) {
        amv_profile_t dummy;
        if (store_load(i, &dummy)) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}

static bool store_factory_reset(void) {
    uint8_t zeros[sizeof(amv_store_layout_t)];
    memset(zeros, 0, sizeof(zeros));
    return backend_write(0, zeros, sizeof(zeros));
}

static bool s_last_boot_bypass = false;

bool amv_store_boot_bypass(void) {
    return s_last_boot_bypass;
}

bool amv_store_load_boot_full(uint8_t *active_slot, amv_profile_t *p, bool *bypass) {
    if (!active_slot || !p) return false;
    uint16_t addr = (uint16_t)offsetof(amv_store_layout_t, boot);
    amv_boot_record_t rec;
    if (!backend_read(addr, &rec, sizeof(rec))) return false;
    if (rec.magic != AMV_STORE_BOOT_MAGIC || rec.schema != AMV_STORE_SCHEMA) return false;
    uint32_t expected_crc = amv_store_crc32(&rec, sizeof(rec) - sizeof(rec.crc));
    if (rec.crc != expected_crc) return false;
    for (int i = 0; i < 8; i++) {
        float val = rec.w[i];
        if (!std::isfinite(val) || val < (float)AMV_WEIGHT_MIN || val > (float)AMV_WEIGHT_MAX) {
            return false;
        }
    }
    if (rec.active_slot >= AMV_NUM_SLOTS) return false;

    *active_slot = rec.active_slot;
    memcpy(p->w, rec.w, sizeof(p->w));
    s_last_boot_bypass = (rec.bypass != 0);
    if (bypass) *bypass = s_last_boot_bypass;
    return true;
}

static bool store_load_boot(uint8_t *active_slot, amv_profile_t *p) {
    return amv_store_load_boot_full(active_slot, p, nullptr);
}

static bool store_save_boot(uint8_t active_slot, const amv_profile_t *p, bool bypass) {
    if (!p || active_slot >= AMV_NUM_SLOTS) return false;
    if (!validate_weights(p->w)) return false;
    uint16_t addr = (uint16_t)offsetof(amv_store_layout_t, boot);
    amv_boot_record_t rec;
    rec.magic = AMV_STORE_BOOT_MAGIC;
    rec.schema = AMV_STORE_SCHEMA;
    rec.active_slot = active_slot;
    rec.bypass = bypass ? 1 : 0;
    memcpy(rec.w, p->w, sizeof(rec.w));
    rec.crc = amv_store_crc32(&rec, sizeof(rec) - sizeof(rec.crc));
    bool ok = backend_write(addr, &rec, sizeof(rec));
    if (ok) {
        s_last_boot_bypass = bypass;
    }
    return ok;
}

static const amv_store_ops_t s_store_ops = {
    store_load,
    store_save,
    store_valid_mask,
    store_factory_reset,
    store_load_boot,
    store_save_boot,
};

const amv_store_ops_t *amv_store_teensy_ops(void) {
    return &s_store_ops;
}
