/*
 * AnyMouse dongle vendor-HID protocol (version 1).
 *
 * Carried on the dongle's vendor HID interface (usage page 0xFF4D, no report IDs):
 * 64-byte input and output reports. All multi-byte fields are little-endian; floats
 * are IEEE-754 binary32.
 *
 *   byte 0  magic0 'A' (0x41)      byte 4  seq   (echoed in the reply)
 *   byte 1  magic1 'M' (0x4D)      byte 5  status (requests: 0; replies: AMV_ST_*)
 *   byte 2  protocol version       byte 6  payload length (0..56)
 *   byte 3  command                byte 7  reserved (0)
 *   bytes 8..63  payload
 *
 * Replies echo cmd and seq. Asynchronous events use cmd AMV_CMD_EVENT_STATE, seq 0.
 *
 * Identity: a host decides that a device is an AnyMouse dongle from the HELLO handshake,
 * never from its USB VID:PID (those change between development and production builds).
 * The host sends a random nonce; the dongle answers with AMV_HELLO_IDENT, the same nonce
 * and its capabilities. Only a live implementation of this protocol can echo the nonce, so
 * an unrelated device that happens to use the same usage page is not mistaken for a dongle.
 * AMV_VENDOR_USAGE_PAGE is the only thing a host matches before asking.
 *
 * Only plain "#define NAME number" lines are parsed by tools/dongle_test.py, so keep
 * constants in that form.
 */
#ifndef AMV_DONGLE_PROTO_H
#define AMV_DONGLE_PROTO_H

#include <stdint.h>

#define AMV_PROTO_VERSION     1
#define AMV_FRAME_SIZE        64
#define AMV_HEADER_SIZE       8
#define AMV_PAYLOAD_MAX       56
#define AMV_MAGIC0            0x41
#define AMV_MAGIC1            0x4D
#define AMV_VENDOR_USAGE_PAGE 0xFF4D
#define AMV_VENDOR_USAGE      0x01   /* application collection usage on that page */
#define AMV_NUM_SLOTS         4

/* USB identity of the dev build (PLAN.md section 8: replace the PID before sharing builds). */
#define AMV_USB_VID           0x303A
#define AMV_USB_PID           0x4D56

/* Weight validity: a value outside this range, or NaN/Inf, is rejected (never clamped). */
#define AMV_WEIGHT_MIN        0.01
#define AMV_WEIGHT_MAX        5.0

/* SET_WEIGHTS base_gen value meaning "apply regardless of the current generation". */
#define AMV_GEN_ANY           0xFFFFFFFF

/* Commands */
#define AMV_CMD_GET_INFO         0x01
#define AMV_CMD_GET_WEIGHTS      0x02
#define AMV_CMD_SET_WEIGHTS      0x03
#define AMV_CMD_SAVE_PROFILE     0x04
#define AMV_CMD_LOAD_PROFILE     0x05
#define AMV_CMD_SET_BYPASS       0x06
#define AMV_CMD_GET_STATS        0x07
#define AMV_CMD_TEST_SCALE       0x08   /* debug: run the real scaling on injected deltas */
#define AMV_CMD_TEST_SELF_CHANGE 0x09   /* debug: change a weight from the device side */
#define AMV_CMD_REBOOT           0x0A
#define AMV_CMD_FACTORY_RESET    0x0B
#define AMV_CMD_GET_SLOTS        0x0C   /* reply: 1 byte, bit n set if slot n holds a valid saved profile */
#define AMV_CMD_HELLO            0x0D   /* identity handshake (see top of file); firmware 0.4.0+ */
/* Attached-device information (firmware 0.5.0+). Index n of GET_MOUSE runs over the claimed mice
 * (tiers KNOWN, GENERIC, BOOT) and then the ignored devices; out of range answers NO_MOUSE with
 * {index, count}. Indexes change on hotplug: refetch after EVENT_MOUSE and match on uid. */
#define AMV_CMD_GET_MOUSE        0x0E   /* {index} -> amv_mouse_info_t */
#define AMV_CMD_GET_MOUSE_CAPS   0x0F   /* {index} -> amv_mouse_caps_t (tier KNOWN only) */
#define AMV_CMD_GET_MOUSE_DESC   0x10   /* amv_mouse_desc_req_t -> amv_mouse_desc_rep_t (raw report descriptor) */
#define AMV_CMD_SET_MOUSE_CPI    0x11   /* amv_mouse_set_req_t -> amv_mouse_info_t; KNOWN: written to the mouse, else stored as a label */
#define AMV_CMD_SET_MOUSE_POLLING 0x12  /* amv_mouse_set_req_t -> amv_mouse_info_t; KNOWN with AMV_MF_CAN_SET_RATE only */
/* Full OpenMouse coverage (firmware 0.6.0+, see firmware/esp32p4-dongle/README.md). A brand-neutral settings list: a
 * client renders whatever the mouse offers (CPI, polling, lift-off, Motion Sync, battery) without
 * knowing the brand. SET_MOUSE_CPI / SET_MOUSE_POLLING remain as aliases of SET_MOUSE_SETTING. */
#define AMV_CMD_GET_MOUSE_MODEL  0x13   /* {index} -> amv_mouse_model_t: the catalog's name for this id */
#define AMV_CMD_GET_MOUSE_SETTINGS 0x14 /* {index} -> amv_settings_rep_t */
#define AMV_CMD_GET_MOUSE_OPTIONS  0x15 /* amv_options_req_t -> amv_options_rep_t */
#define AMV_CMD_SET_MOUSE_SETTING  0x16 /* amv_setting_set_req_t -> amv_settings_rep_t */
#define AMV_CMD_EVENT_STATE      0x80   /* async: state changed (any source) */
#define AMV_CMD_EVENT_MOUSE      0x81   /* async: amv_mouse_event_t; a mouse was attached, removed or re-read */

/* Status codes in replies */
#define AMV_ST_OK          0
#define AMV_ST_BAD_CMD     1
#define AMV_ST_BAD_LEN     2
#define AMV_ST_BAD_VALUE   3
#define AMV_ST_STALE       4   /* base_gen did not match; payload carries the current state */
#define AMV_ST_BAD_SLOT    5
#define AMV_ST_STORE_ERR   6
#define AMV_ST_BUSY        7
#define AMV_ST_EMPTY       8   /* LOAD_PROFILE on a slot that was never saved */
#define AMV_ST_BAD_MAGIC   9
#define AMV_ST_NO_MOUSE    10  /* index out of range; payload {index, count} */
#define AMV_ST_UNSUPPORTED 11  /* this mouse or its driver cannot do that */
#define AMV_ST_DEVICE_ERR  12  /* the mouse did not answer its vendor protocol */

/* Where a state change came from (EVENT payload and bookkeeping) */
#define AMV_SRC_HOST       0
#define AMV_SRC_DEVICE     1
#define AMV_SRC_LOAD       2
#define AMV_SRC_BOOT       3

/* HELLO reply identity word: bytes 'A' 'M' 'V' 'D' (AnyMouse Vectoring Dongle) little-endian */
#define AMV_HELLO_IDENT    0x44564D41
#define AMV_HELLO_NONCE_LEN 8

/* HELLO capability bits */
#define AMV_CAP_VECTORED_MOUSE 0x0001   /* the HID mouse on this USB device already carries vectored motion */
#define AMV_CAP_MOUSE_HOST     0x0002   /* a downstream mouse can be attached (USB host side present) */

/* Device tiers (firmware/esp32p4-dongle/README.md, Mouse discovery and information) */
#define AMV_TIER_KNOWN     1   /* vendor protocol known (OpenMouse driver on the dongle): real CPI / polling */
#define AMV_TIER_GENERIC   2   /* report descriptor parsed: vectored, info from USB descriptors */
#define AMV_TIER_BOOT      3   /* descriptor unusable, read in boot protocol (8-bit motion, 3 buttons) */
#define AMV_TIER_IGNORED   4   /* attached but not used (see AMV_IGN_*) */

/* amv_mouse_info_t.flags */
#define AMV_MF_REPORT_IDS    0x01
#define AMV_MF_WHEEL         0x02
#define AMV_MF_PAN           0x04
#define AMV_MF_ALSO_KEYBOARD 0x08
#define AMV_MF_VERIFIED      0x10  /* driver tested on this model through the dongle; writes allowed */
#define AMV_MF_CAN_SET_CPI   0x20
#define AMV_MF_CAN_SET_RATE  0x40
#define AMV_MF_DRV_ERROR     0x80  /* the driver's last exchange with the mouse failed */

/* amv_mouse_info_t.cpi_source */
#define AMV_CPI_NONE       0
#define AMV_CPI_USER       1   /* entered by the user, stored per VID:PID, label only */
#define AMV_CPI_DEVICE     2   /* read from the mouse */

/* amv_mouse_info_t.driver */
#define AMV_DRV_NONE       0
#define AMV_DRV_RAZER      1
#define AMV_DRV_FINALMOUSE 2
#define AMV_DRV_GEARHUB    3
#define AMV_DRV_LOGITECH   4

/* Settings (GET_MOUSE_SETTINGS entries). Values are u16 in the setting's own unit. */
#define AMV_SET_CPI          1   /* counts per inch */
#define AMV_SET_POLLING      2   /* Hz */
#define AMV_SET_LOD          3   /* lift-off distance, see the options unit */
#define AMV_SET_MOTION_SYNC  4   /* 0 off, 1 on */
#define AMV_SET_BATTERY      5   /* percent; read only */
#define AMV_SET_CHARGING     6   /* AMV_CHG_*; read only */
#define AMV_SET_LINK         7   /* AMV_LINK_*; read only */

#define AMV_SETF_READABLE    0x01
#define AMV_SETF_WRITABLE    0x02   /* only for models verified through the dongle */

/* AMV_SET_CHARGING values */
#define AMV_CHG_UNKNOWN      0
#define AMV_CHG_DISCHARGING  1
#define AMV_CHG_CHARGING     2
#define AMV_CHG_FULL         3

/* AMV_SET_LINK values: how the mouse is reached (receivers sleep, wired mice do not) */
#define AMV_LINK_WIRED       0   /* also: no radio involved */
#define AMV_LINK_CONNECTED   1   /* via a receiver, the mouse answered */
#define AMV_LINK_ASLEEP      2   /* via a receiver, the mouse is asleep or out of range: wakes on motion */

/* GET_MOUSE_OPTIONS reply kinds and units */
#define AMV_OPT_LIST         0   /* values[n] are the allowed values */
#define AMV_OPT_RANGE        1   /* values = {min, max, step} */
#define AMV_UNIT_NONE        0
#define AMV_UNIT_HZ          1
#define AMV_UNIT_CPI         2
#define AMV_UNIT_MM10        3   /* tenths of a millimetre */
#define AMV_UNIT_LEVEL       4   /* ordinal 1..n, lowest first (Low, Medium, High when n is 3) */
#define AMV_UNIT_BOOL        5

/* amv_mouse_model_t.state: what the dongle knows about this exact product id */
#define AMV_MODEL_UNKNOWN    0   /* not in OpenMouse's list */
#define AMV_MODEL_KNOWN      1   /* in OpenMouse's list, no driver ported to the dongle yet */
#define AMV_MODEL_DRIVER     2   /* a ported driver talks to it */

/* amv_mouse_info_t.reason (tier IGNORED) */
#define AMV_IGN_NOT_MOUSE  1   /* no HID mouse collection (keyboard, audio, storage, ...) */
#define AMV_IGN_ABSOLUTE   2   /* absolute pointer (tablet, touch screen) */
#define AMV_IGN_UNPARSED   3   /* descriptor unusable and no boot protocol to fall back on */
#define AMV_IGN_NO_SLOT    4   /* too many mice attached */
#define AMV_IGN_CLAIM      5   /* the interface could not be claimed or read */

/* amv_mouse_event_t.change */
#define AMV_MEV_ATTACHED   1
#define AMV_MEV_REMOVED    2
#define AMV_MEV_UPDATED    3

/* TEST_SCALE request flags */
#define AMV_TEST_RESET_REM 0x01

typedef struct __attribute__((packed)) {
    uint8_t magic0, magic1, ver, cmd, seq, status, len, rsvd;
} amv_hdr_t;

/* GET_WEIGHTS reply; SET_WEIGHTS/SET_BYPASS/LOAD_PROFILE OK and STALE replies; base of EVENT. 38 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t gen;
    float    w[8];        /* raw weights (bypass is reported separately) */
    uint8_t  bypass;
    uint8_t  active_slot;
} amv_state_payload_t;

/* EVENT_STATE payload: state + source. 39 bytes. */
typedef struct __attribute__((packed)) {
    amv_state_payload_t st;
    uint8_t             source;
} amv_event_payload_t;

/* SET_WEIGHTS request. 36 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t base_gen;
    float    w[8];
} amv_set_weights_req_t;

/* GET_INFO reply. 18 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    uint8_t  fw_major, fw_minor, fw_patch;
    uint16_t chip_rev;    /* e.g. 103 = v1.03 */
    uint8_t  num_slots;
    uint8_t  has_mouse;   /* number of downstream mice currently attached and read */
    uint8_t  mac[6];
    uint32_t uptime_ms;
} amv_info_payload_t;

/* HELLO request: 8 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  nonce[AMV_HELLO_NONCE_LEN];
} amv_hello_req_t;

/* HELLO reply. 24 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t ident;       /* AMV_HELLO_IDENT */
    uint8_t  nonce[AMV_HELLO_NONCE_LEN];   /* copied from the request */
    uint8_t  proto_ver;
    uint8_t  fw_major, fw_minor, fw_patch;
    uint16_t caps;        /* AMV_CAP_* */
    uint8_t  mac[6];      /* stable per-unit id */
} amv_hello_rep_t;

/* GET_MOUSE reply. 56 bytes. Rates in Hz, 0 = unknown. */
typedef struct __attribute__((packed)) {
    uint8_t  index, count;    /* this entry, entries in the list */
    uint8_t  tier;            /* AMV_TIER_* */
    uint8_t  flags;           /* AMV_MF_* */
    uint16_t vid, pid;
    uint8_t  speed;           /* 0 low, 1 full, 2 high */
    uint8_t  iface;           /* claimed interface number */
    uint16_t nominal_hz;      /* from bInterval and bus speed */
    uint16_t measured_hz;     /* highest reports/s seen in the last 10 s (rises only while moving) */
    uint16_t device_hz;       /* polling rate the mouse reports (KNOWN), 0 = unknown */
    uint16_t cpi;             /* 0 = unknown */
    uint8_t  cpi_source;      /* AMV_CPI_* */
    uint8_t  battery;         /* percent, 0xFF = none or unknown */
    uint8_t  num_buttons;
    uint8_t  x_bits;          /* width of the X field */
    uint8_t  driver;          /* AMV_DRV_* */
    uint8_t  reason;          /* AMV_IGN_* for tier IGNORED, else 0 */
    uint16_t uid;             /* stable while attached */
    char     name[30];        /* product string, NUL-terminated */
} amv_mouse_info_t;

/* GET_MOUSE_MODEL reply. 56 bytes. Names come from the catalog generated out of OpenMouse. */
typedef struct __attribute__((packed)) {
    uint8_t  index, state, rsvd[2];   /* state: AMV_MODEL_* */
    char     brand[16];
    char     model[36];               /* "" when OpenMouse has no name for this id */
} amv_mouse_model_t;

typedef struct __attribute__((packed)) {
    uint8_t  id;                      /* AMV_SET_* */
    uint8_t  flags;                   /* AMV_SETF_* */
    uint16_t value;
} amv_setting_t;

#define AMV_SETTINGS_MAX 13

/* GET_MOUSE_SETTINGS and SET_MOUSE_SETTING reply. 4 + 4n bytes (at most 56). */
typedef struct __attribute__((packed)) {
    uint8_t  index, count, link, rsvd;
    amv_setting_t s[AMV_SETTINGS_MAX];
} amv_settings_rep_t;

typedef struct __attribute__((packed)) {
    uint8_t  index, setting;
} amv_options_req_t;

#define AMV_OPTIONS_MAX 24

/* GET_MOUSE_OPTIONS reply. 6 + 2n bytes (at most 54). */
typedef struct __attribute__((packed)) {
    uint8_t  index, setting, kind, unit, n, rsvd;
    uint16_t values[AMV_OPTIONS_MAX];
} amv_options_rep_t;

typedef struct __attribute__((packed)) {
    uint8_t  index, setting;
    uint16_t value;
} amv_setting_set_req_t;

/* GET_MOUSE_CAPS reply. 42 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  index, driver, n_rates, rsvd;
    uint16_t rates[8];        /* polling rates the mouse accepts */
    uint16_t cpi_min, cpi_max, cpi_step;
    char     firmware[16];    /* mouse firmware version, "" if unknown */
} amv_mouse_caps_t;

typedef struct __attribute__((packed)) {
    uint8_t  index, rsvd;
    uint16_t offset;
} amv_mouse_desc_req_t;

/* GET_MOUSE_DESC reply. 56 bytes; ask again at offset + len until offset + len == total. */
typedef struct __attribute__((packed)) {
    uint8_t  index, rsvd;
    uint16_t total, offset;
    uint8_t  len, rsvd2;
    uint8_t  data[48];
} amv_mouse_desc_rep_t;

/* SET_MOUSE_CPI / SET_MOUSE_POLLING request. 4 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  index, rsvd;
    uint16_t value;           /* CPI, or Hz */
} amv_mouse_set_req_t;

/* EVENT_MOUSE payload. 4 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  count;           /* entries now in the list */
    uint8_t  change;          /* AMV_MEV_* */
    uint16_t uid;             /* the entry concerned */
} amv_mouse_event_t;

/* GET_STATS reply. 32 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t sets_ok, stale_rejects, bad_value_rejects, saves;
    uint32_t loads, events_sent, events_dropped, test_scales;
} amv_stats_payload_t;

/* TEST_SCALE request: 5 bytes; reply: 17 bytes. */
typedef struct __attribute__((packed)) {
    int16_t dx, dy;
    uint8_t flags;
} amv_test_scale_req_t;

typedef struct __attribute__((packed)) {
    int32_t out_dx, out_dy;
    float   rem_x, rem_y;
    uint8_t octant;
} amv_test_scale_rep_t;

/* TEST_SELF_CHANGE request: 7 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  octant;
    float    value;
    uint16_t delay_ms;
} amv_test_self_change_req_t;

_Static_assert(sizeof(amv_hdr_t) == AMV_HEADER_SIZE, "header size");
_Static_assert(sizeof(amv_state_payload_t) == 38, "state payload size");
_Static_assert(sizeof(amv_event_payload_t) == 39, "event payload size");
_Static_assert(sizeof(amv_set_weights_req_t) == 36, "set weights size");
_Static_assert(sizeof(amv_info_payload_t) == 18, "info size");
_Static_assert(sizeof(amv_stats_payload_t) == 32, "stats size");
_Static_assert(sizeof(amv_hello_req_t) == 8, "hello req size");
_Static_assert(sizeof(amv_hello_rep_t) == 24, "hello rep size");
_Static_assert(sizeof(amv_mouse_info_t) == 56, "mouse info size");
_Static_assert(sizeof(amv_mouse_caps_t) == 42, "mouse caps size");
_Static_assert(sizeof(amv_mouse_model_t) == 56, "mouse model size");
_Static_assert(sizeof(amv_setting_t) == 4, "setting size");
_Static_assert(sizeof(amv_settings_rep_t) == 4 + 4 * AMV_SETTINGS_MAX, "settings rep size");
_Static_assert(sizeof(amv_settings_rep_t) <= AMV_PAYLOAD_MAX, "settings rep fits a frame");
_Static_assert(sizeof(amv_options_rep_t) == 6 + 2 * AMV_OPTIONS_MAX, "options rep size");
_Static_assert(sizeof(amv_options_rep_t) <= AMV_PAYLOAD_MAX, "options rep fits a frame");
_Static_assert(sizeof(amv_setting_set_req_t) == 4, "setting set size");
_Static_assert(sizeof(amv_mouse_desc_rep_t) == 56, "mouse desc size");
_Static_assert(sizeof(amv_mouse_set_req_t) == 4, "mouse set size");
_Static_assert(sizeof(amv_mouse_event_t) == 4, "mouse event size");
_Static_assert(sizeof(amv_test_scale_req_t) == 5, "test scale req size");
_Static_assert(sizeof(amv_test_scale_rep_t) == 17, "test scale rep size");
_Static_assert(sizeof(amv_test_self_change_req_t) == 7, "self change size");

#endif /* AMV_DONGLE_PROTO_H */
