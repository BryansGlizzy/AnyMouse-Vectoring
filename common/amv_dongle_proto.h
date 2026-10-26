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

/* USB identity of the dev build (PLAN.md section 6: replace the PID before sharing builds). */
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
#define AMV_CMD_EVENT_STATE      0x80   /* async: state changed (any source) */

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
    uint8_t  has_mouse;   /* 0 until the USB host side exists */
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
_Static_assert(sizeof(amv_test_scale_req_t) == 5, "test scale req size");
_Static_assert(sizeof(amv_test_scale_rep_t) == 17, "test scale rep size");
_Static_assert(sizeof(amv_test_self_change_req_t) == 7, "self change size");

#endif /* AMV_DONGLE_PROTO_H */
