#pragma once
#include <stdint.h>

/* HID instance index of the vendor interface in the composite device (IF0 = mouse, IF1 = vendor). */
#define AMV_HID_INSTANCE_VENDOR 1

#define AMV_FW_MAJOR 0
#define AMV_FW_MINOR 3
#define AMV_FW_PATCH 0

/* Starts the protocol task and subscribes to state-change events. Call after amv_ctrl_init(). */
void amv_proto_init(void);

/* Feed one received 64-byte output report. Safe to call from the TinyUSB task; it only enqueues. */
void amv_proto_on_rx(const uint8_t *buf, uint16_t len);
