#include "usb_ghost.h"
#include <Arduino.h>
#include <string.h>
#include "usb_desc.h"
#include "usb_dev.h"

extern "C" {
extern const uint8_t usb_config_descriptor_480[];
extern const uint8_t usb_config_descriptor_12[];
extern volatile uint8_t usb_configuration;
const uint8_t *usb_base_device_descriptor(void);
uint16_t usb_base_config_desc_len(void);
void usb_ep0_ghost_receive(void *data, uint32_t len, uint16_t wRequestAndType, uint16_t wValue, uint16_t wIndex, uint16_t wLength);
void usb_ep0_ghost_transmit(const void *data, uint32_t len, uint32_t seq);
}

#define TX_NUM 4
#define RX_NUM 4
#define SET_Q_LEN 6

static struct {
    bool     active;
    uint16_t vid, pid;
    uint8_t  n_if;
    amv_ghost_if_cfg_t ifs[AMV_GHOST_MAX_IFS];

    uint8_t  dev_desc[18];
    uint8_t  cfg_480[256];
    uint8_t  cfg_12[256];
    uint8_t  other_cfg[256];
    uint16_t cfg_len;
    uint16_t hid_off[AMV_GHOST_MAX_IFS];

    struct { uint8_t bLength; uint8_t bDescriptorType; uint16_t wString[AMV_GHOST_STR_MAX]; } mfg_str, prod_str;
    bool has_mfg, has_prod;
} s_g;

// Per-slot interrupt transfers (EP6 = slot 0, EP7 = slot 1)
static transfer_t s_tx_xfer[AMV_GHOST_MAX_IFS][TX_NUM] __attribute__((used, aligned(32)));
static transfer_t s_rx_xfer[AMV_GHOST_MAX_IFS][RX_NUM] __attribute__((used, aligned(32)));
DMAMEM static uint8_t s_tx_buf[AMV_GHOST_MAX_IFS][TX_NUM][64] __attribute__((aligned(32)));
DMAMEM static uint8_t s_rx_buf[AMV_GHOST_MAX_IFS][RX_NUM][64] __attribute__((aligned(32)));
static uint8_t  s_tx_head[AMV_GHOST_MAX_IFS];
static volatile uint16_t s_rx_len[AMV_GHOST_MAX_IFS][RX_NUM];
static volatile uint8_t  s_rx_qhead[AMV_GHOST_MAX_IFS], s_rx_qtail[AMV_GHOST_MAX_IFS];
static uint8_t  s_rx_list[AMV_GHOST_MAX_IFS][RX_NUM + 1];

// EP0 control report forwarding state
DMAMEM static uint8_t s_ep0_buf[256] __attribute__((aligned(32)));
static amv_ghost_ep0_set_t s_set_q[SET_Q_LEN];
static volatile uint8_t s_set_head, s_set_tail;
static volatile bool s_get_pending;
static amv_ghost_ep0_get_t s_get_req;

static void fill_utf16(const char *ascii, uint8_t *bLength, uint8_t *bType, uint16_t *wStr) {
    size_t n = 0;
    while (ascii[n] && n < AMV_GHOST_STR_MAX) {
        wStr[n] = (uint8_t)ascii[n];
        n++;
    }
    *bLength = (uint8_t)(2 + n * 2);
    *bType = 3;
}

static uint16_t append_if_desc(uint8_t *dst, uint8_t ifnum, uint8_t ep_num, uint16_t rlen,
                               uint16_t mps_in, uint16_t mps_out, uint8_t iv_in, uint8_t iv_out) {
    uint16_t hid_off = 9;
    uint8_t blk[32] = {
        // Interface descriptor (9)
        9, 4, ifnum, 0, 2, 0x03, 0x00, 0x00, 0,
        // HID descriptor (9)
        9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, (uint8_t)(rlen & 0xFF), (uint8_t)(rlen >> 8),
        // Endpoint IN descriptor (7)
        7, 5, (uint8_t)(ep_num | 0x80), 0x03, (uint8_t)(mps_in & 0xFF), (uint8_t)(mps_in >> 8), iv_in,
        // Endpoint OUT descriptor (7)
        7, 5, ep_num, 0x03, (uint8_t)(mps_out & 0xFF), (uint8_t)(mps_out >> 8), iv_out,
    };
    memcpy(dst, blk, sizeof blk);
    return hid_off;
}

void usb_ghost_configure_identity(const amv_ghost_cfg_t *cfg) {
    if (!cfg || cfg->n_if == 0) {
        usb_ghost_clear();
        return;
    }
    NVIC_DISABLE_IRQ(IRQ_USB1);
    s_g.active = false;
    s_g.vid = cfg->vid;
    s_g.pid = cfg->pid;
    s_g.n_if = cfg->n_if > AMV_GHOST_MAX_IFS ? AMV_GHOST_MAX_IFS : cfg->n_if;
    for (uint8_t i = 0; i < s_g.n_if; i++) {
        s_g.ifs[i] = cfg->ifs[i];
        if (s_g.ifs[i].mps_in == 0 || s_g.ifs[i].mps_in > 64) s_g.ifs[i].mps_in = 64;
        if (s_g.ifs[i].mps_out == 0 || s_g.ifs[i].mps_out > 64) s_g.ifs[i].mps_out = 64;
        if (s_g.ifs[i].interval_in == 0) s_g.ifs[i].interval_in = 1;
        if (s_g.ifs[i].interval_out == 0) s_g.ifs[i].interval_out = 1;
    }

    memcpy(s_g.dev_desc, usb_base_device_descriptor(), 18);
    s_g.dev_desc[8]  = (uint8_t)(s_g.vid & 0xFF);
    s_g.dev_desc[9]  = (uint8_t)(s_g.vid >> 8);
    s_g.dev_desc[10] = (uint8_t)(s_g.pid & 0xFF);
    s_g.dev_desc[11] = (uint8_t)(s_g.pid >> 8);

    s_g.has_mfg = cfg->manufacturer[0] != 0;
    if (s_g.has_mfg) fill_utf16(cfg->manufacturer, &s_g.mfg_str.bLength, &s_g.mfg_str.bDescriptorType, s_g.mfg_str.wString);
    s_g.has_prod = cfg->product[0] != 0;
    if (s_g.has_prod) fill_utf16(cfg->product, &s_g.prod_str.bLength, &s_g.prod_str.bDescriptorType, s_g.prod_str.wString);

    uint16_t base_len = usb_base_config_desc_len();
    memcpy(s_g.cfg_480, usb_config_descriptor_480, base_len);
    memcpy(s_g.cfg_12, usb_config_descriptor_12, base_len);
    uint16_t pos = base_len;
    for (uint8_t i = 0; i < s_g.n_if; i++) {
        uint8_t iv_in_hs = s_g.ifs[i].interval_in > 16 ? 4 : s_g.ifs[i].interval_in;
        uint8_t iv_out_hs = s_g.ifs[i].interval_out > 16 ? 4 : s_g.ifs[i].interval_out;
        s_g.hid_off[i] = pos + append_if_desc(s_g.cfg_480 + pos, GHOST0_INTERFACE + i, GHOST0_ENDPOINT + i,
                                              s_g.ifs[i].desc_len, s_g.ifs[i].mps_in, s_g.ifs[i].mps_out,
                                              iv_in_hs, iv_out_hs);
        append_if_desc(s_g.cfg_12 + pos, GHOST0_INTERFACE + i, GHOST0_ENDPOINT + i,
                       s_g.ifs[i].desc_len, s_g.ifs[i].mps_in, s_g.ifs[i].mps_out, 1, 1);
        pos += 32;
    }
    s_g.cfg_len = pos;
    s_g.cfg_480[2] = s_g.cfg_12[2] = (uint8_t)(pos & 0xFF);
    s_g.cfg_480[3] = s_g.cfg_12[3] = (uint8_t)(pos >> 8);
    s_g.cfg_480[4] = s_g.cfg_12[4] = (uint8_t)(NUM_INTERFACE + s_g.n_if);

    s_set_head = s_set_tail = 0;
    s_get_pending = false;
    s_g.active = true;
    NVIC_ENABLE_IRQ(IRQ_USB1);
}

void usb_ghost_clear(void) {
    NVIC_DISABLE_IRQ(IRQ_USB1);
    s_g.active = false;
    s_g.n_if = 0;
    s_set_head = s_set_tail = 0;
    s_get_pending = false;
    NVIC_ENABLE_IRQ(IRQ_USB1);
}

bool usb_ghost_is_active(void) { return s_g.active; }

static void rx_queue_transfer(int slot, int i) {
    void *buf = s_rx_buf[slot][i];
    uint16_t mps = s_g.ifs[slot].mps_out;
    arm_dcache_delete(buf, 64);
    NVIC_DISABLE_IRQ(IRQ_USB1);
    usb_prepare_transfer(&s_rx_xfer[slot][i], buf, mps, (uint32_t)i);
    usb_receive(GHOST0_ENDPOINT + slot, &s_rx_xfer[slot][i]);
    NVIC_ENABLE_IRQ(IRQ_USB1);
}

static void rx_done(int slot, transfer_t *t) {
    int i = (int)t->callback_param;
    uint16_t mps = s_g.ifs[slot].mps_out;
    uint16_t rem = (uint16_t)((t->status >> 16) & 0x7FFFu);
    uint16_t got = (rem <= mps) ? (mps - rem) : 0;
    s_rx_len[slot][i] = got;
    uint8_t head = s_rx_qhead[slot];
    uint8_t next = (uint8_t)((head + 1) % (RX_NUM + 1));
    if (next != s_rx_qtail[slot]) {
        s_rx_list[slot][next] = (uint8_t)i;
        s_rx_qhead[slot] = next;
    } else {
        // Queue full: drop and immediately re-arm descriptor so hardware keeps receiving.
        void *buf = s_rx_buf[slot][i];
        arm_dcache_delete(buf, 64);
        usb_prepare_transfer(&s_rx_xfer[slot][i], buf, mps, (uint32_t)i);
        usb_receive(GHOST0_ENDPOINT + slot, &s_rx_xfer[slot][i]);
    }
}

static void rx0_cb(transfer_t *t) { rx_done(0, t); }
static void rx1_cb(transfer_t *t) { rx_done(1, t); }

extern "C" int usb_ghost_get_descriptor(uint16_t wValue, uint16_t wIndex, uint8_t high_speed, const uint8_t **data, uint32_t *len) {
    if (!s_g.active) return 0;
    if (wValue == 0x0100 && wIndex == 0) {
        *data = s_g.dev_desc; *len = sizeof(s_g.dev_desc); return 1;
    }
    if (wValue == 0x0200 && wIndex == 0) {
        *data = high_speed ? s_g.cfg_480 : s_g.cfg_12; *len = s_g.cfg_len; return 1;
    }
    if (wValue == 0x0700 && wIndex == 0) {
        const uint8_t *src = high_speed ? s_g.cfg_12 : s_g.cfg_480;
        memcpy(s_g.other_cfg, src, s_g.cfg_len);
        s_g.other_cfg[1] = 7;
        *data = s_g.other_cfg; *len = s_g.cfg_len; return 1;
    }
    if (wValue == 0x0301 && (wIndex == 0x0409 || wIndex == 0) && s_g.has_mfg) {
        *data = (const uint8_t *)&s_g.mfg_str; *len = s_g.mfg_str.bLength; return 1;
    }
    if (wValue == 0x0302 && (wIndex == 0x0409 || wIndex == 0) && s_g.has_prod) {
        *data = (const uint8_t *)&s_g.prod_str; *len = s_g.prod_str.bLength; return 1;
    }
    if (wIndex >= GHOST0_INTERFACE && wIndex < GHOST0_INTERFACE + s_g.n_if) {
        int slot = wIndex - GHOST0_INTERFACE;
        if (wValue == 0x2200) {
            *data = s_g.ifs[slot].desc; *len = s_g.ifs[slot].desc_len; return 1;
        }
        if (wValue == 0x2100) {
            *data = s_g.cfg_480 + s_g.hid_off[slot]; *len = 9; return 1;
        }
    }
    return 0;
}

extern "C" void usb_ghost_configure(void) {
    if (!s_g.active) return;
    for (int s = 0; s < s_g.n_if; s++) {
        memset(&s_tx_xfer[s], 0, sizeof(s_tx_xfer[s]));
        memset(&s_rx_xfer[s], 0, sizeof(s_rx_xfer[s]));
        s_tx_head[s] = 0;
        s_rx_qhead[s] = s_rx_qtail[s] = 0;
        usb_config_tx(GHOST0_ENDPOINT + s, s_g.ifs[s].mps_in, 0, NULL);
        usb_config_rx(GHOST0_ENDPOINT + s, s_g.ifs[s].mps_out, 0, s == 0 ? rx0_cb : rx1_cb);
        for (int i = 0; i < RX_NUM; i++) rx_queue_transfer(s, i);
    }
}

extern "C" int usb_ghost_ep0_setup(uint16_t wRequestAndType, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint32_t seq) {
    if (!s_g.active || wIndex < GHOST0_INTERFACE || wIndex >= GHOST0_INTERFACE + s_g.n_if) return 0;
    uint8_t slot = (uint8_t)(wIndex - GHOST0_INTERFACE);
    if (wRequestAndType == 0x0921) { // SET_REPORT
        if (wLength > sizeof(s_ep0_buf)) return 0;
        arm_dcache_delete(s_ep0_buf, sizeof(s_ep0_buf));
        usb_ep0_ghost_receive(s_ep0_buf, wLength, wRequestAndType, wValue, wIndex, wLength);
        return 1;
    }
    if (wRequestAndType == 0x01A1) { // GET_REPORT
        if (wLength > sizeof(s_ep0_buf)) return 0;
        s_get_req.slot = slot;
        s_get_req.report_type = (uint8_t)(wValue >> 8);
        s_get_req.report_id = (uint8_t)(wValue & 0xFF);
        s_get_req.len = wLength;
        s_get_req.seq = seq;
        s_get_pending = true;
        return 1;
    }
    return 0;
}

extern "C" void usb_ghost_ep0_set_complete(uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    if (!s_g.active || wIndex < GHOST0_INTERFACE || wIndex >= GHOST0_INTERFACE + s_g.n_if) return;
    if (wLength > sizeof(s_ep0_buf)) return;
    arm_dcache_delete(s_ep0_buf, sizeof(s_ep0_buf));
    uint8_t next = (uint8_t)((s_set_head + 1) % SET_Q_LEN);
    if (next == s_set_tail) return; // queue full
    amv_ghost_ep0_set_t *it = &s_set_q[s_set_head];
    it->slot = (uint8_t)(wIndex - GHOST0_INTERFACE);
    it->report_type = (uint8_t)(wValue >> 8);
    it->report_id = (uint8_t)(wValue & 0xFF);
    it->len = wLength;
    if (wLength) memcpy(it->data, s_ep0_buf, wLength);
    s_set_head = next;
}

bool usb_ghost_tx_ready(int slot) {
    if (!s_g.active || !usb_configuration || slot < 0 || slot >= s_g.n_if) return false;
    transfer_t *xfer = &s_tx_xfer[slot][s_tx_head[slot]];
    return !(usb_transfer_status(xfer) & 0x80);
}

bool usb_ghost_send(int slot, const uint8_t *data, uint16_t len) {
    if (!s_g.active || !usb_configuration || slot < 0 || slot >= s_g.n_if) return false;
    transfer_t *xfer = &s_tx_xfer[slot][s_tx_head[slot]];
    if (usb_transfer_status(xfer) & 0x80) return false;
    if (len > s_g.ifs[slot].mps_in) len = s_g.ifs[slot].mps_in;
    uint8_t *txdata = s_tx_buf[slot][s_tx_head[slot]];
    if (len) memcpy(txdata, data, len);
    arm_dcache_flush_delete(txdata, 64);
    usb_prepare_transfer(xfer, txdata, len, 0);
    usb_transmit(GHOST0_ENDPOINT + slot, xfer);
    if (++s_tx_head[slot] >= TX_NUM) s_tx_head[slot] = 0;
    return true;
}

int usb_ghost_recv(int slot, uint8_t *out, uint16_t cap) {
    if (!s_g.active || !usb_configuration || slot < 0 || slot >= s_g.n_if) return 0;
    uint8_t tail = s_rx_qtail[slot];
    if (tail == s_rx_qhead[slot]) return 0;
    tail = (uint8_t)((tail + 1) % (RX_NUM + 1));
    uint8_t i = s_rx_list[slot][tail];
    s_rx_qtail[slot] = tail;
    uint16_t got = s_rx_len[slot][i];
    if (got > cap) got = cap;
    arm_dcache_delete(s_rx_buf[slot][i], 64);
    if (got) memcpy(out, s_rx_buf[slot][i], got);
    rx_queue_transfer(slot, i);
    return (int)got;
}

bool usb_ghost_pop_set_report(amv_ghost_ep0_set_t *out) {
    NVIC_DISABLE_IRQ(IRQ_USB1);
    bool have = (s_set_head != s_set_tail);
    if (have) {
        *out = s_set_q[s_set_tail];
        s_set_tail = (uint8_t)((s_set_tail + 1) % SET_Q_LEN);
    }
    NVIC_ENABLE_IRQ(IRQ_USB1);
    return have;
}

bool usb_ghost_take_get_report(amv_ghost_ep0_get_t *out) {
    NVIC_DISABLE_IRQ(IRQ_USB1);
    bool have = s_get_pending;
    if (have) {
        *out = s_get_req;
        s_get_pending = false;
    }
    NVIC_ENABLE_IRQ(IRQ_USB1);
    return have;
}

void usb_ghost_finish_get_report(uint32_t seq, const uint8_t *data, int len) {
    if (len < 0 || !data) {
        usb_ep0_ghost_transmit(NULL, 0xFFFFFFFFu, seq);
        return;
    }
    uint16_t n = (uint16_t)len;
    if (n > sizeof(s_ep0_buf)) n = sizeof(s_ep0_buf);
    if (n) memcpy(s_ep0_buf, data, n);
    arm_dcache_flush_delete(s_ep0_buf, sizeof(s_ep0_buf));
    usb_ep0_ghost_transmit(s_ep0_buf, n, seq);
}
