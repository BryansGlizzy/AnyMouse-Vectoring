#include "amv_catalog.h"
#include <string.h>
#include "amv_catalog.generated.h"

_Static_assert(sizeof(amv_cat_t) == sizeof(amv_catalog_entry_t), "catalog entry layout");

static const amv_catalog_entry_t *tbl(void) { return (const amv_catalog_entry_t *)amv_catalog; }

size_t amv_catalog_count(void) { return AMV_CATALOG_COUNT; }
const amv_catalog_entry_t *amv_catalog_at(size_t i) { return i < AMV_CATALOG_COUNT ? &tbl()[i] : NULL; }
const char *amv_catalog_commit(void) { return AMV_CATALOG_OPENMOUSE_COMMIT; }

const char *amv_catalog_brand(const amv_catalog_entry_t *e)
{
    return e && e->brand < sizeof amv_cat_brands / sizeof amv_cat_brands[0] ? amv_cat_brands[e->brand] : "";
}

const char *amv_catalog_engine(const amv_catalog_entry_t *e)
{
    unsigned i = e && e->engine >= 1000 ? e->engine - 1000u : 0xFFFF;
    return i < sizeof amv_cat_engines / sizeof amv_cat_engines[0] ? amv_cat_engines[i] : "";
}

const amv_catalog_entry_t *amv_catalog_find(uint16_t vid, uint16_t pid)
{
    const amv_catalog_entry_t *t = tbl();
    size_t lo = 0, hi = AMV_CATALOG_COUNT;
    const amv_catalog_entry_t *wide = NULL;
    while (lo < hi) {                                  /* lower bound on (vid, pid) */
        size_t mid = (lo + hi) / 2;
        uint32_t k = ((uint32_t)t[mid].vid << 16) | t[mid].pid, want = ((uint32_t)vid << 16) | pid;
        if (k < want) lo = mid + 1; else hi = mid;
    }
    if (lo < AMV_CATALOG_COUNT && t[lo].vid == vid && t[lo].pid == pid) return &t[lo];
    for (size_t i = 0; i < AMV_CATALOG_COUNT; i++) {   /* vendor-wide rows are few */
        if (t[i].vid == vid && (t[i].flags & AMV_CAT_VID_ONLY)) { wide = &t[i]; break; }
    }
    return wide;
}
