#ifndef AMV_PROTO_TEENSY_H
#define AMV_PROTO_TEENSY_H
#include <stddef.h>
#include <stdint.h>
// The shared C wire header uses the C spelling of static_assert.
#pragma push_macro("_Static_assert")
#undef _Static_assert
#define _Static_assert static_assert
#include "../../../common/amv_dongle_proto.h"
#pragma pop_macro("_Static_assert")
#define AMV_TEENSY_FW_MAJOR 0
#define AMV_TEENSY_FW_MINOR 2
#define AMV_TEENSY_FW_PATCH 1

typedef struct { float w[8]; } amv_profile_t;
typedef struct {
    bool (*load)(uint8_t slot, amv_profile_t *p);
    bool (*save)(uint8_t slot, const amv_profile_t *p);
    uint8_t (*valid_mask)(void);
    bool (*factory_reset)(void);
    bool (*load_boot)(uint8_t *active_slot, amv_profile_t *p);
    bool (*save_boot)(uint8_t active_slot, const amv_profile_t *p, bool bypass);
} amv_store_ops_t;
// The attached mice (mice.cpp). Every call returns an AMV_ST_* status and fills its reply the way the ESP32-P4
// dongle does (amv_usb_host.c), so the two answer byte for byte alike.
#define AMV_ST_PENDING 0xFF     // write(): started, the result comes from write_done()
typedef struct amv_mouse_ops {
    uint8_t (*count)(void);
    uint8_t (*info)(uint8_t index, amv_mouse_info_t *out);
    uint8_t (*caps)(uint8_t index, amv_mouse_caps_t *out);
    uint8_t (*desc)(uint8_t index, uint16_t offset, amv_mouse_desc_rep_t *out);
    uint8_t (*model)(uint8_t index, amv_mouse_model_t *out);
    uint8_t (*settings)(uint8_t index, amv_settings_rep_t *out);
    uint8_t (*options)(uint8_t index, uint8_t setting, amv_options_rep_t *out);
    // A write (AMV_SET_*): a status when it finished at once, AMV_ST_PENDING when a driver call was started.
    uint8_t (*write)(uint8_t index, uint8_t setting, uint16_t value);
    // The started write finished: true once, with its status.
    bool (*write_done)(uint8_t *status);
} amv_mouse_ops_t;
void amv_proto_notify_mouse(uint8_t change, uint16_t uid);
typedef struct {
    void (*apply)(const float w[8], bool bypass);
    uint32_t (*uptime_ms)(void);
    void (*unit_id)(uint8_t mac[6]);
    void (*reboot)(void);
    uint8_t (*mouse_count)(void);
    void (*get_stats)(uint8_t *payload, uint8_t *len);
    const amv_store_ops_t *store;
    const amv_mouse_ops_t *mouse;
} amv_plat_t;
// Single loop-thread API; input/output buffers may alias. Platform is copied.
void amv_proto_init(const amv_plat_t *plat);
size_t amv_proto_handle(const uint8_t in[64], uint8_t out[64], uint8_t source = AMV_SRC_HOST);
// Loop-thread access; restore initializes boot state without bumping gen or events.
void amv_proto_snapshot(amv_state_payload_t *out);
void amv_proto_restore(uint8_t slot, const amv_profile_t *p, bool bypass);
bool amv_proto_take_event(uint8_t out[64]);
void amv_proto_notify_state(uint8_t source);
// Call only after the REBOOT reply has been transmitted.
bool amv_proto_reboot_pending();
void amv_proto_run_deferred();
// Loop-only: the reply to a deferred mouse write, once it finished (retry sending it if the transport is busy).
bool amv_proto_poll_mouse(uint8_t out[64]);
// Protocol counters for GET_STATS; the loop counts events sent and dropped.
void amv_proto_count_event(bool sent);
// TEST_SELF_CHANGE: applies a pending device-side change once its delay has passed (loop).
void amv_proto_tick(void);
#endif
