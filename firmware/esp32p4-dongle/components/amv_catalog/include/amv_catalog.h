#pragma once
/*
 * Catalog of every device OpenMouse knows (see firmware/esp32p4-dongle/README.md, Mouse discovery and information), generated from a pinned
 * OpenMouse checkout by tools/openmouse/gen_catalog.sh. Pure C with no ESP-IDF dependency, so it
 * is also built by the host tests.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t vid, pid;
    uint16_t page;       /* vendor interface usage page the engine talks to, 0 = none recorded */
    uint16_t engine;     /* generated AMV_ENG_* id, 0 = none */
    uint16_t usage;
    uint8_t  brand;      /* index, see amv_catalog_brand() */
    uint8_t  flags;      /* bit 0: vendor-wide row (any product of the vendor) */
    const char *model;   /* "" when OpenMouse has no name for this id */
} amv_catalog_entry_t;

/* The row for this exact id, else the vendor-wide row, else NULL. */
const amv_catalog_entry_t *amv_catalog_find(uint16_t vid, uint16_t pid);
const char *amv_catalog_brand(const amv_catalog_entry_t *e);
/* The OpenMouse client class this entry's engine is (e.g. "LogitechHidppClient"), or "". */
const char *amv_catalog_engine(const amv_catalog_entry_t *e);
size_t      amv_catalog_count(void);
const amv_catalog_entry_t *amv_catalog_at(size_t i);
const char *amv_catalog_commit(void);
