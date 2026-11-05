#include "pc_out.h"
#include <Arduino.h>
#include <string.h>

#define BTN_QUEUE 16

static int32_t pend_dx, pend_dy, pend_wheel, pend_pan;
static uint16_t src_buttons[PC_OUT_MAX_SOURCES];
static uint16_t btn_q[BTN_QUEUE];
static unsigned btn_head, btn_count;
static uint16_t btn_last_queued, btn_last_sent;
static bool pending_age;
static uint32_t pending_since;
static volatile uint32_t selftest_until;
static bool selftest_flip;
static pc_out_stats_t st;

static inline uint32_t irq_save() { uint32_t p; __asm__ volatile("mrs %0, primask" : "=r"(p)); __disable_irq(); return p; }
static inline void irq_restore(uint32_t p) { if (!p) __enable_irq(); }

static int32_t take(int32_t *pend, int32_t lim) {
    int32_t v = *pend > lim ? lim : (*pend < -lim ? -lim : *pend);
    *pend -= v;
    return v;
}

// Combined button state changed: queue it as its own frame. Interrupts are off.
static void queue_buttons() {
    uint16_t now = 0;
    for (unsigned i = 0; i < PC_OUT_MAX_SOURCES; i++) now |= src_buttons[i];
    if (now == btn_last_queued) return;
    if (btn_count == BTN_QUEUE) {          // keep the newest state: drop the oldest queued edge
        btn_head = (btn_head + 1) % BTN_QUEUE;
        btn_count--;
        st.button_overflow++;
    }
    btn_q[(btn_head + btn_count) % BTN_QUEUE] = now;
    btn_count++;
    btn_last_queued = now;
}

// Send the next frame if the endpoint is free. Interrupts are off.
static void try_send() {
    uint8_t r[8];
    if (selftest_until) {
        if ((int32_t)(millis() - selftest_until) >= 0) { selftest_until = 0; return; }
        int16_t x = selftest_flip ? 1 : -1;
        memset(r, 0, sizeof r);
        memcpy(r + 2, &x, 2);
        if (usb_mouse_send_raw(r, 8) == 0) { selftest_flip = !selftest_flip; st.frames++; }
        return;
    }
    bool edge = btn_count > 0;
    bool motion = pend_dx || pend_dy || pend_wheel || pend_pan;
    if (!edge && !motion) return;
    uint16_t buttons = edge ? btn_q[btn_head] : btn_last_sent;
    int32_t sdx = pend_dx, sdy = pend_dy, sw = pend_wheel, sp = pend_pan;
    int16_t dx = (int16_t)take(&pend_dx, 32767), dy = (int16_t)take(&pend_dy, 32767);
    int8_t wheel = (int8_t)take(&pend_wheel, 127), pan = (int8_t)take(&pend_pan, 127);
    memcpy(r, &buttons, 2);
    memcpy(r + 2, &dx, 2);
    memcpy(r + 4, &dy, 2);
    r[6] = (uint8_t)wheel;
    r[7] = (uint8_t)pan;
    if (usb_mouse_send_raw(r, 8) != 0) {      // endpoint busy: put it all back, the completion hook retries
        pend_dx = sdx; pend_dy = sdy; pend_wheel = sw; pend_pan = sp;
        return;
    }
    if (edge) { btn_head = (btn_head + 1) % BTN_QUEUE; btn_count--; }
    btn_last_sent = buttons;
    st.frames++;
    st.sent_x += dx;
    st.sent_y += dy;
    bool more = pend_dx || pend_dy || pend_wheel || pend_pan;
    if (more) st.carried++;
    if (pending_age) {
        uint32_t age = ARM_DWT_CYCCNT - pending_since;
        st.age_sum += age; st.age_n++;
        if (age > st.age_max) st.age_max = age;
    }
    pending_age = more || btn_count;
    if (pending_age) pending_since = ARM_DWT_CYCCNT;
}

static void tx_done() { try_send(); }          // device interrupt: the PC collected the last frame

void pc_out_begin(void) {
    usb_mouse_set_tx_hook(tx_done);
}

void pc_out_feed(unsigned src, int32_t raw_dx, int32_t raw_dy, int32_t sx, int32_t sy, int32_t wheel, int32_t pan,
                 bool has_buttons, uint16_t buttons) {
    if (src >= PC_OUT_MAX_SOURCES) return;
    uint32_t p = irq_save();
    st.reports++;
    st.raw_x += raw_dx;
    st.raw_y += raw_dy;
    pend_dx += sx; pend_dy += sy; pend_wheel += wheel; pend_pan += pan;
    if (has_buttons) { src_buttons[src] = buttons; queue_buttons(); }
    if (!pending_age && (sx || sy || wheel || pan || btn_count)) { pending_age = true; pending_since = ARM_DWT_CYCCNT; }
    try_send();
    irq_restore(p);
}

void pc_out_release(unsigned src) {
    if (src >= PC_OUT_MAX_SOURCES) return;
    uint32_t p = irq_save();
    src_buttons[src] = 0;
    queue_buttons();
    try_send();
    irq_restore(p);
}

void pc_out_selftest(uint32_t until_ms) {
    uint32_t p = irq_save();
    selftest_until = until_ms ? (until_ms ? until_ms : 1) : 0;
    try_send();
    irq_restore(p);
}

bool pc_out_selftest_active(void) { return selftest_until != 0; }

void pc_out_take_stats(pc_out_stats_t *out) {
    uint32_t p = irq_save();
    *out = st;
    memset(&st, 0, sizeof st);
    irq_restore(p);
}

void pc_out_poll(void) {
    uint32_t p = irq_save();
    if (!usb_configuration) {
        pend_dx = pend_dy = pend_wheel = pend_pan = 0;
        btn_count = 0;
        pending_age = false;
    } else if (selftest_until || pend_dx || pend_dy || pend_wheel || pend_pan || btn_count) {
        try_send();            // a frame refused while the endpoint was being reconfigured
    }
    irq_restore(p);
}
