#include "amv_hid_parse.h"
#include <string.h>

#define MAX_USAGES   32
#define MAX_GSTACK   4
#define MAX_CSTACK   8
#define MAX_REPORTS  16

#define PAGE_DESKTOP  0x01
#define PAGE_BUTTON   0x09
#define PAGE_CONSUMER 0x0C
#define USAGE(p, u)   (((uint32_t)(p) << 16) | (uint32_t)(u))

typedef struct {
    uint32_t usage_page;
    int32_t  lmin, lmax;
    uint32_t rsize, rcount, rid;
} globals_t;

typedef struct {
    uint32_t usage[MAX_USAGES];
    int      nusage;
    bool     has_range;
    uint32_t umin, umax;       /* full usages (page << 16 | id) */
} locals_t;

typedef struct { uint32_t page, usage; bool application; } coll_t;

typedef struct { uint8_t id; uint32_t bits; } rpos_t;

static int32_t item_value(const uint8_t *p, unsigned size, bool is_signed)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    if (is_signed && size && size < 4 && (v >> (8 * size - 1)) & 1) v |= ~0u << (8 * size);
    return (int32_t)v;
}

static rpos_t *report_pos(rpos_t *tbl, int *n, uint32_t id)
{
    for (int i = 0; i < *n; i++) if (tbl[i].id == id) return &tbl[i];
    if (*n >= MAX_REPORTS) return NULL;
    tbl[*n] = (rpos_t){ .id = (uint8_t)id, .bits = 0 };
    return &tbl[(*n)++];
}

static void set_field(amv_hid_field_t *f, const globals_t *g, uint32_t bit_off)
{
    if (f->present || g->rsize == 0 || g->rsize > 32 || bit_off > 0xFFFF) return;
    f->present = true;
    f->report_id = (uint8_t)g->rid;
    f->bit_off = (uint16_t)bit_off;
    f->bit_size = (uint8_t)g->rsize;
    f->is_signed = g->lmin < 0;
}

int amv_hid_parse(const uint8_t *d, size_t n, amv_hid_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    globals_t g = { 0 }, gstack[MAX_GSTACK];
    int gdepth = 0;
    locals_t l = { 0 };
    coll_t cstack[MAX_CSTACK];
    int cdepth = 0;
    rpos_t rpos[MAX_REPORTS];
    int nrpos = 0;
    bool abs_seen = false;

    size_t i = 0;
    while (i < n) {
        uint8_t b = d[i];
        size_t size;
        uint8_t type, tag;
        if (b == 0xFE) {                       /* long item: skip */
            if (i + 2 >= n) return -1;
            i += 3 + (size_t)d[i + 1];
            continue;
        }
        size = b & 3;
        if (size == 3) size = 4;
        type = (b >> 2) & 3;
        tag = b >> 4;
        if (i + 1 + size > n) return -1;
        const uint8_t *p = d + i + 1;
        i += 1 + size;

        if (type == 1) {                       /* global */
            switch (tag) {
            case 0x0: g.usage_page = (uint32_t)item_value(p, (unsigned)size, false); break;
            case 0x1: g.lmin = item_value(p, (unsigned)size, true); break;
            case 0x2: g.lmax = item_value(p, (unsigned)size, g.lmin < 0); break;
            case 0x7: g.rsize = (uint32_t)item_value(p, (unsigned)size, false); break;
            case 0x8:
                g.rid = (uint32_t)item_value(p, (unsigned)size, false);
                if (g.rid == 0 || g.rid > 255) return -2;
                out->uses_report_ids = true;
                break;
            case 0x9: g.rcount = (uint32_t)item_value(p, (unsigned)size, false); break;
            case 0xA:
                if (gdepth >= MAX_GSTACK) return -3;
                gstack[gdepth++] = g;
                break;
            case 0xB:
                if (gdepth <= 0) return -3;
                g = gstack[--gdepth];
                break;
            default: break;
            }
        } else if (type == 2) {                /* local */
            uint32_t v = (uint32_t)item_value(p, (unsigned)size, false);
            uint32_t full = size == 4 ? v : ((g.usage_page << 16) | (v & 0xFFFF));
            switch (tag) {
            case 0x0: if (l.nusage < MAX_USAGES) l.usage[l.nusage++] = full; break;
            case 0x1: l.umin = full; l.has_range = true; break;
            case 0x2: l.umax = full; l.has_range = true; break;
            default: break;
            }
        } else if (type == 0) {                /* main */
            uint32_t flags = (uint32_t)item_value(p, (unsigned)size, false);
            switch (tag) {
            case 0xA: {                        /* collection */
                if (cdepth >= MAX_CSTACK) return -4;
                uint32_t u = l.nusage ? l.usage[0] : (l.has_range ? l.umin : 0);
                coll_t c = { .page = u >> 16, .usage = u & 0xFFFF, .application = (flags & 0xFF) == 1 };
                if (c.application && c.page == PAGE_DESKTOP && c.usage == 2) out->has_mouse = true;
                if (c.application && c.page == PAGE_DESKTOP && c.usage == 6) out->has_keyboard = true;
                cstack[cdepth++] = c;
                break;
            }
            case 0xC:
                if (cdepth <= 0) return -4;
                cdepth--;
                break;
            case 0x8: {                        /* input */
                rpos_t *rp = report_pos(rpos, &nrpos, g.rid);
                if (!rp) return -5;
                bool in_mouse = false;
                for (int k = 0; k < cdepth; k++) {
                    if (cstack[k].application && cstack[k].page == PAGE_DESKTOP && cstack[k].usage == 2) in_mouse = true;
                }
                bool variable = flags & 0x02, relative = flags & 0x04, constant = flags & 0x01;
                if (in_mouse && !constant && variable) {
                    for (uint32_t k = 0; k < g.rcount; k++) {
                        uint32_t u = 0;
                        if (l.has_range) {
                            u = l.umin + k;
                            if (l.umax >= l.umin && u > l.umax) u = l.umax;
                        } else if (l.nusage) {
                            u = l.usage[k < (uint32_t)l.nusage ? k : (uint32_t)l.nusage - 1];
                        }
                        uint32_t off = rp->bits + k * g.rsize;
                        if (u == USAGE(PAGE_DESKTOP, 0x30) || u == USAGE(PAGE_DESKTOP, 0x31)) {
                            if (!relative) { abs_seen = true; continue; }
                            set_field(u == USAGE(PAGE_DESKTOP, 0x30) ? &out->x : &out->y, &g, off);
                        } else if (u == USAGE(PAGE_DESKTOP, 0x38)) {
                            set_field(&out->wheel, &g, off);
                        } else if (u == USAGE(PAGE_CONSUMER, 0x238)) {
                            set_field(&out->pan, &g, off);
                        } else if ((u >> 16) == PAGE_BUTTON && (u & 0xFFFF) >= 1 && (u & 0xFFFF) <= AMV_HID_MAX_BUTTONS) {
                            unsigned bn = (u & 0xFFFF) - 1;
                            set_field(&out->button[bn], &g, off);
                            if (out->button[bn].present && bn + 1 > out->num_buttons) out->num_buttons = (uint8_t)(bn + 1);
                        }
                    }
                }
                rp->bits += g.rsize * g.rcount;
                break;
            }
            default: break;                    /* output / feature do not occupy input bits */
            }
            memset(&l, 0, sizeof(l));          /* locals live until the next main item */
        }
    }
    if (cdepth != 0) return -4;
    out->absolute = abs_seen && !(out->x.present && out->y.present);
    out->valid = out->has_mouse && out->x.present && out->y.present && out->x.report_id == out->y.report_id;
    return 0;
}

static int32_t get_bits(const uint8_t *p, size_t plen, const amv_hid_field_t *f, bool *ok)
{
    if (((size_t)f->bit_off + f->bit_size + 7) / 8 > plen) { *ok = false; return 0; }
    uint32_t v = 0;
    for (unsigned i = 0; i < f->bit_size; i++) {
        unsigned bit = f->bit_off + i;
        if ((p[bit >> 3] >> (bit & 7)) & 1) v |= 1u << i;
    }
    if (f->is_signed && f->bit_size < 32 && ((v >> (f->bit_size - 1)) & 1)) v |= ~0u << f->bit_size;
    return (int32_t)v;
}

bool amv_hid_decode(const amv_hid_layout_t *l, const uint8_t *data, size_t len, amv_hid_report_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!l->valid) return false;
    uint8_t id = 0;
    if (l->uses_report_ids) {
        if (len < 1) return false;
        id = data[0];
        data++;
        len--;
    }
    bool ok = true;
    if (l->x.present && l->x.report_id == id) { out->dx = get_bits(data, len, &l->x, &ok); out->have |= AMV_HID_HAVE_MOTION; }
    if (l->y.present && l->y.report_id == id) { out->dy = get_bits(data, len, &l->y, &ok); out->have |= AMV_HID_HAVE_MOTION; }
    if (l->wheel.present && l->wheel.report_id == id) { out->wheel = get_bits(data, len, &l->wheel, &ok); out->have |= AMV_HID_HAVE_WHEEL; }
    if (l->pan.present && l->pan.report_id == id) { out->pan = get_bits(data, len, &l->pan, &ok); out->have |= AMV_HID_HAVE_PAN; }
    for (int k = 0; k < AMV_HID_MAX_BUTTONS; k++) {
        const amv_hid_field_t *f = &l->button[k];
        if (f->present && f->report_id == id) {
            if (get_bits(data, len, f, &ok) & 1) out->buttons |= 1u << k;
            out->have |= AMV_HID_HAVE_BUTTONS;
        }
    }
    return ok && out->have != 0;
}

bool amv_hid_has_collection(const uint8_t *d, size_t n, uint16_t want_page, uint16_t want_usage)
{
    uint32_t page = 0, usage = 0;
    bool have = false;
    size_t i = 0;
    while (i < n) {
        uint8_t b = d[i];
        if (b == 0xFE) {
            if (i + 2 >= n) return false;
            i += 3 + (size_t)d[i + 1];
            continue;
        }
        size_t size = b & 3;
        if (size == 3) size = 4;
        if (i + 1 + size > n) return false;
        uint32_t v = (uint32_t)item_value(d + i + 1, (unsigned)size, false);
        i += 1 + size;
        switch (b & 0xFC) {
        case 0x04: page = v; break;                                  /* Usage Page */
        case 0x08:                                                   /* Usage */
            if (!have) { usage = size == 4 ? v : ((page << 16) | (v & 0xFFFF)); have = true; }
            break;
        case 0xA0:                                                   /* Collection */
            if (have && (usage >> 16) == want_page && (usage & 0xFFFF) == want_usage) return true;
            have = false;
            break;
        case 0x80: case 0x90: case 0xB0: case 0xC0:
            have = false;
            break;
        default: break;
        }
    }
    return false;
}

const uint8_t amv_hid_boot_mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0,
};
const size_t amv_hid_boot_mouse_desc_len = sizeof(amv_hid_boot_mouse_desc);
