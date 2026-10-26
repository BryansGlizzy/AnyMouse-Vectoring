#include "amv_vector.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "amv_ctrl.h"
#include "amv_octant.h"
#include "amv_usb_device.h"

static const char *TAG = "amv_vector";

#define BTN_QUEUE 16

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static amv_octant_state_t s_rem[AMV_VECTOR_MAX_SOURCES];
static uint32_t s_src_buttons[AMV_VECTOR_MAX_SOURCES];
static int32_t s_pend_dx, s_pend_dy, s_pend_wheel, s_pend_pan;
static uint16_t s_btn_q[BTN_QUEUE];
static unsigned s_btn_head, s_btn_count;
static uint16_t s_btn_last_queued, s_btn_last_sent;
static amv_vector_stats_t s_stats;
static TaskHandle_t s_task;

static void kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

static void kick_from_isr_or_task(void)
{
    kick();
}

/* Combined button state changed: queue it as its own frame. Caller holds s_lock. */
static void queue_buttons_locked(void)
{
    uint32_t any = 0;
    for (int i = 0; i < AMV_VECTOR_MAX_SOURCES; i++) any |= s_src_buttons[i];
    uint16_t now = (uint16_t)any;
    if (now == s_btn_last_queued) return;
    if (s_btn_count == BTN_QUEUE) {
        /* Keep the newest state: drop the oldest queued edge */
        s_btn_head = (s_btn_head + 1) % BTN_QUEUE;
        s_btn_count--;
        s_stats.button_overflow++;
    }
    s_btn_q[(s_btn_head + s_btn_count) % BTN_QUEUE] = now;
    s_btn_count++;
    s_btn_last_queued = now;
}

void amv_vector_feed(unsigned src, int32_t raw_dx, int32_t raw_dy, int32_t wheel, int32_t pan,
                     bool has_buttons, uint32_t buttons)
{
    if (src >= AMV_VECTOR_MAX_SOURCES) return;
    int32_t sx = 0, sy = 0;
    uint8_t octant;
    if (raw_dx || raw_dy) {
        float w[8];
        amv_ctrl_snapshot_effective(w);
        /* s_rem[src] is only touched by the one task servicing this source (and by release, which runs after its transfers ended) */
        amv_octant_scale(w, &s_rem[src], raw_dx, raw_dy, &sx, &sy, &octant);
    }
    portENTER_CRITICAL(&s_lock);
    s_stats.reports_in++;
    s_stats.raw_dx += raw_dx;
    s_stats.raw_dy += raw_dy;
    s_pend_dx += sx;
    s_pend_dy += sy;
    s_pend_wheel += wheel;
    s_pend_pan += pan;
    if (has_buttons) {
        s_src_buttons[src] = buttons;
        queue_buttons_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    kick_from_isr_or_task();
}

void amv_vector_release(unsigned src)
{
    if (src >= AMV_VECTOR_MAX_SOURCES) return;
    portENTER_CRITICAL(&s_lock);
    s_src_buttons[src] = 0;
    s_rem[src] = (amv_octant_state_t){ 0 };
    queue_buttons_locked();
    portEXIT_CRITICAL(&s_lock);
    kick();
}

void amv_vector_get_stats(amv_vector_stats_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}

static int32_t clamp_take(int32_t *pend, int32_t lim)
{
    int32_t v = *pend > lim ? lim : (*pend < -lim ? -lim : *pend);
    *pend -= v;
    return v;
}

static void try_send(void)
{
    if (!amv_usb_device_mouse_ready()) return;
    int lim = amv_usb_device_mouse_max_delta();
    uint16_t buttons;
    int32_t dx, dy, wheel, pan;
    bool again;

    portENTER_CRITICAL(&s_lock);
    bool edge = s_btn_count > 0;
    buttons = edge ? s_btn_q[s_btn_head] : s_btn_last_sent;
    bool motion = s_pend_dx || s_pend_dy || s_pend_wheel || s_pend_pan;
    if (!edge && !motion) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (edge) { s_btn_head = (s_btn_head + 1) % BTN_QUEUE; s_btn_count--; }
    dx = clamp_take(&s_pend_dx, lim);
    dy = clamp_take(&s_pend_dy, lim);
    wheel = clamp_take(&s_pend_wheel, 127);
    pan = clamp_take(&s_pend_pan, 127);
    s_btn_last_sent = buttons;
    s_stats.frames_out++;
    s_stats.out_dx += dx;
    s_stats.out_dy += dy;
    again = s_btn_count > 0 || s_pend_dx || s_pend_dy || s_pend_wheel || s_pend_pan;
    portEXIT_CRITICAL(&s_lock);

    amv_usb_device_mouse_send(buttons, dx, dy, wheel, pan);
    (void)again;   /* the completion callback kicks us for the next frame */
}

static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, 1);    /* woken by feed, by the previous report finishing, or every tick */
        try_send();
    }
}

void amv_vector_start(void)
{
    amv_usb_device_set_mouse_done_cb(kick);
    xTaskCreatePinnedToCore(sender_task, "amv_send", 3072, NULL, configMAX_PRIORITIES - 3, &s_task, 1);
    ESP_LOGI(TAG, "vector sender up");
}
