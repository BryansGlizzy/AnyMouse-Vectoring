#ifndef AMV_PROTO_TEENSY_H
#define AMV_PROTO_TEENSY_H
#include <stddef.h>
#include <stdint.h>
// The shared C wire header uses the C spelling of static_assert.
#define _Static_assert static_assert
#include "../../../common/amv_dongle_proto.h"
#undef _Static_assert
#define AMV_TEENSY_FW_MAJOR 0
#define AMV_TEENSY_FW_MINOR 1
#define AMV_TEENSY_FW_PATCH 0

typedef struct { float w[8]; } amv_profile_t;
typedef struct {
    bool (*load)(uint8_t slot, amv_profile_t *p);
    bool (*save)(uint8_t slot, const amv_profile_t *p);
    uint8_t (*valid_mask)(void);
    bool (*factory_reset)(void);
    bool (*load_boot)(uint8_t *active_slot, amv_profile_t *p);
    bool (*save_boot)(uint8_t active_slot, const amv_profile_t *p, bool bypass);
} amv_store_ops_t;
typedef struct amv_mouse_ops amv_mouse_ops_t;
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
size_t amv_proto_handle(const uint8_t in[64], uint8_t out[64]);
bool amv_proto_take_event(uint8_t out[64]);
void amv_proto_notify_state(uint8_t source);
// Call only after the REBOOT reply has been transmitted.
bool amv_proto_reboot_pending();
void amv_proto_run_deferred();
// Optional mouse-card implementation; returns status and writes payload/length.
uint8_t amv_proto_mouse_get_info(const amv_mouse_ops_t *, uint8_t index,
                                amv_mouse_info_t *out);
#endif
