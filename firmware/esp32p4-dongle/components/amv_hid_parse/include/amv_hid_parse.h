#pragma once
/*
 * HID report-descriptor parser for the mouse side of the dongle.
 *
 * Pure C with no ESP-IDF dependency, so it is unit-tested on the build host
 * (tests/test_hid_parse.c). It walks a report descriptor and records where X, Y, wheel,
 * AC Pan and buttons 1..16 live inside the mouse application collection (Generic Desktop /
 * Mouse), and amv_hid_decode() pulls them out of an input report. Handles report IDs,
 * arbitrary field widths (8/12/16 bit, signed or not), Push/Pop and usage ranges.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AMV_HID_MAX_BUTTONS 16

typedef struct {
    bool     present;
    uint8_t  report_id;    /* 0 when the descriptor has no report IDs */
    uint16_t bit_off;      /* from the first bit after the report ID byte */
    uint8_t  bit_size;     /* 1..32 */
    bool     is_signed;
} amv_hid_field_t;

typedef struct {
    bool valid;             /* mouse application collection with relative X and Y */
    bool has_mouse;         /* a Generic Desktop / Mouse application collection exists */
    bool absolute;          /* its X/Y are absolute (tablet, touch): not vectored */
    bool has_keyboard;      /* the descriptor also has a keyboard collection (macro pad / combo device) */
    bool uses_report_ids;   /* every report starts with its ID byte */
    amv_hid_field_t x, y, wheel, pan;
    amv_hid_field_t button[AMV_HID_MAX_BUTTONS];
    uint8_t num_buttons;    /* highest button number present */
} amv_hid_layout_t;

#define AMV_HID_HAVE_MOTION  0x01
#define AMV_HID_HAVE_WHEEL   0x02
#define AMV_HID_HAVE_PAN     0x04
#define AMV_HID_HAVE_BUTTONS 0x08

typedef struct {
    int32_t  dx, dy, wheel, pan;
    uint32_t buttons;       /* bit n = button n+1 */
    uint8_t  have;          /* AMV_HID_HAVE_*: which of the above this report carried */
} amv_hid_report_t;

/* 0 on success (check layout->valid), negative if the descriptor is malformed. */
int  amv_hid_parse(const uint8_t *desc, size_t len, amv_hid_layout_t *out);

/* true if the descriptor declares an application collection with this usage page and usage
 * (used to find a vendor configuration interface). */
bool amv_hid_has_collection(const uint8_t *desc, size_t len, uint16_t page, uint16_t usage);

/* Report IDs declared by the descriptor, in order of first appearance (duplicates dropped);
 * returns how many were stored (at most `max`). 0 when the descriptor has no report IDs. */
int amv_hid_report_ids(const uint8_t *desc, size_t len, uint8_t *ids, int max);

/* The HID 1.11 appendix B boot-protocol mouse descriptor (3 buttons, 8-bit X/Y): the layout used
 * when a mouse's own descriptor cannot be parsed and it is switched to boot protocol. */
extern const uint8_t amv_hid_boot_mouse_desc[];
extern const size_t amv_hid_boot_mouse_desc_len;

/* true if the report carried at least one mouse field. */
bool amv_hid_decode(const amv_hid_layout_t *l, const uint8_t *data, size_t len, amv_hid_report_t *out);
