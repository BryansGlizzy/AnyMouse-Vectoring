#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AMV_GHOST_MAX_IFS   2
#define AMV_GHOST_DESC_MAX  928
#define AMV_GHOST_STR_MAX   48

typedef struct {
    uint8_t  desc[AMV_GHOST_DESC_MAX];
    uint16_t desc_len;
    uint16_t mps_in;
    uint16_t mps_out;
    uint8_t  interval_in;
    uint8_t  interval_out;
} amv_ghost_if_cfg_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    char     manufacturer[AMV_GHOST_STR_MAX];
    char     product[AMV_GHOST_STR_MAX];
    uint8_t  n_if;
    amv_ghost_if_cfg_t ifs[AMV_GHOST_MAX_IFS];
} amv_ghost_cfg_t;

typedef struct {
    uint8_t  slot;
    uint8_t  report_type;
    uint8_t  report_id;
    uint16_t len;
    uint8_t  data[256];
} amv_ghost_ep0_set_t;

typedef struct {
    uint8_t  slot;
    uint8_t  report_type;
    uint8_t  report_id;
    uint16_t len;
    uint32_t seq;
} amv_ghost_ep0_get_t;

void usb_ghost_configure_identity(const amv_ghost_cfg_t *cfg);
void usb_ghost_clear(void);
bool usb_ghost_is_active(void);

bool usb_ghost_tx_ready(int slot);
bool usb_ghost_send(int slot, const uint8_t *data, uint16_t len);
int  usb_ghost_recv(int slot, uint8_t *out, uint16_t cap);

bool usb_ghost_pop_set_report(amv_ghost_ep0_set_t *out);
bool usb_ghost_take_get_report(amv_ghost_ep0_get_t *out);
void usb_ghost_finish_get_report(uint32_t seq, const uint8_t *data, int len);

extern "C" {
void usb_soft_disconnect(void);
void usb_soft_connect(void);
}
