#ifndef AMV_STORE_TEENSY_H
#define AMV_STORE_TEENSY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "amv_proto_teensy.h"

#define AMV_STORE_EEPROM_SIZE 4284
#define AMV_STORE_SLOT_MAGIC  0x50564D41u /* 'AMVP' little-endian */
#define AMV_STORE_BOOT_MAGIC  0x42564D41u /* 'AMVB' little-endian */
#define AMV_STORE_SCHEMA      1

#ifndef AMV_NUM_SLOTS
#define AMV_NUM_SLOTS 4
#endif

#ifndef AMV_WEIGHT_MIN
#define AMV_WEIGHT_MIN 0.01
#endif
#ifndef AMV_WEIGHT_MAX
#define AMV_WEIGHT_MAX 5.0
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;    /* AMV_STORE_SLOT_MAGIC */
    uint16_t schema;   /* AMV_STORE_SCHEMA */
    uint16_t reserved; /* 0 */
    float    w[8];     /* Profile weights */
    uint32_t crc;      /* CRC32 over all preceding bytes in record */
} amv_slot_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* AMV_STORE_BOOT_MAGIC */
    uint16_t schema;      /* AMV_STORE_SCHEMA */
    uint8_t  active_slot; /* Active slot (0..AMV_NUM_SLOTS-1) */
    uint8_t  bypass;      /* 0 or 1 */
    float    w[8];        /* Boot weights */
    uint32_t crc;         /* CRC32 over all preceding bytes in record */
} amv_boot_record_t;

#define AMV_STORE_LABEL_MAGIC 0x4C564D41u /* 'AMVL' little-endian */
#define AMV_STORE_LABELS      16

/* CPI the user entered for a mouse with no known protocol (labels the effective-DPI readouts only). */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* AMV_STORE_LABEL_MAGIC */
    uint16_t vid, pid;
    uint16_t cpi;         /* 0 = free */
    uint16_t reserved;
    uint32_t crc;
} amv_label_record_t;

typedef struct __attribute__((packed)) {
    amv_slot_record_t slots[AMV_NUM_SLOTS];
    amv_boot_record_t boot;
    amv_label_record_t labels[AMV_STORE_LABELS];   /* appended in firmware 0.2.0; older images never wrote here */
} amv_store_layout_t;

#ifdef __cplusplus
static_assert(sizeof(amv_store_layout_t) <= AMV_STORE_EEPROM_SIZE, "Record layout exceeds 4284 bytes");
#else
_Static_assert(sizeof(amv_store_layout_t) <= AMV_STORE_EEPROM_SIZE, "Record layout exceeds 4284 bytes");
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Backend byte I/O interface */
typedef bool (*amv_store_read_fn)(uint16_t addr, void *buf, size_t n);
typedef bool (*amv_store_write_fn)(uint16_t addr, const void *buf, size_t n);

typedef struct {
    amv_store_read_fn read;
    amv_store_write_fn write;
} amv_store_backend_t;

/* Returns store ops implementation matching the contract's amv_store_ops_t */
const amv_store_ops_t *amv_store_teensy_ops(void);

/* Backend configuration (pass NULL to reset to default backend) */
void amv_store_set_backend(const amv_store_backend_t *backend);
const amv_store_backend_t *amv_store_get_backend(void);

/* Helper to get the bypass state restored by the last successful load_boot() */
bool amv_store_boot_bypass(void);

/* Helper for full boot state load including bypass */
bool amv_store_load_boot_full(uint8_t *active_slot, amv_profile_t *p, bool *bypass);

/* Per-VID:PID CPI labels: get returns false when none is stored; set with cpi 0 removes it. */
bool amv_store_label_get(uint16_t vid, uint16_t pid, uint16_t *cpi);
bool amv_store_label_set(uint16_t vid, uint16_t pid, uint16_t cpi);

/* Utility function to compute standard CRC32 (exposed for tests / backend inspection) */
uint32_t amv_store_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AMV_STORE_TEENSY_H */
