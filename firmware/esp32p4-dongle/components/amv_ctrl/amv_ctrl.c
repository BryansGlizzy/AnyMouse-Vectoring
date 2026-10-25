#include "amv_ctrl.h"
#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "amv_store.h"

#define AUTOSAVE_DELAY_US   (2000 * 1000)
#define MAX_SUBSCRIBERS     4

static const char *TAG = "amv_ctrl";

/* ---- control-plane state, guarded by s_mtx ---- */
static SemaphoreHandle_t s_mtx;
static float    s_w[8];
static uint32_t s_gen;
static bool     s_bypass;
static uint8_t  s_active;
static amv_ctrl_stats_t s_stats;
static amv_event_cb_t s_subs[MAX_SUBSCRIBERS];
static int s_nsubs;
static esp_timer_handle_t s_save_timer;

/* ---- hot-path copy: a seqlock over atomic float bit patterns ---- */
static _Atomic uint32_t s_seq;
static _Atomic uint32_t s_bits[8];
static _Atomic uint32_t s_byp;

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void publish_locked(void)
{
    uint32_t s = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, s + 1, memory_order_relaxed);        /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
    for (int i = 0; i < 8; i++) atomic_store_explicit(&s_bits[i], f2u(s_w[i]), memory_order_relaxed);
    atomic_store_explicit(&s_byp, s_bypass ? 1u : 0u, memory_order_relaxed);
    atomic_store_explicit(&s_seq, s + 2, memory_order_release);        /* even: stable */
}

void amv_ctrl_snapshot_effective(float w[8])
{
    uint32_t s1, s2, bits[8], byp;
    do {
        s1 = atomic_load_explicit(&s_seq, memory_order_acquire);
        for (int i = 0; i < 8; i++) bits[i] = atomic_load_explicit(&s_bits[i], memory_order_relaxed);
        byp = atomic_load_explicit(&s_byp, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((s1 & 1u) || s1 != s2);
    for (int i = 0; i < 8; i++) w[i] = byp ? 1.0f : u2f(bits[i]);
}

static bool weights_valid(const float w[8])
{
    for (int i = 0; i < 8; i++) {
        if (!isfinite(w[i]) || w[i] < (float)AMV_WEIGHT_MIN || w[i] > (float)AMV_WEIGHT_MAX) return false;
    }
    return true;
}

static void fill_state_locked(amv_state_t *out)
{
    if (!out) return;
    out->gen = s_gen;
    memcpy(out->w, s_w, sizeof(s_w));
    out->bypass = s_bypass;
    out->active_slot = s_active;
}

static void notify(const amv_state_t *st, uint8_t source)
{
    amv_event_cb_t subs[MAX_SUBSCRIBERS];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_nsubs;
    memcpy(subs, s_subs, sizeof(subs));
    xSemaphoreGive(s_mtx);
    for (int i = 0; i < n; i++) subs[i](st, source);
}

static void arm_autosave(void)
{
    esp_timer_stop(s_save_timer);   /* ignore "not running" */
    esp_timer_start_once(s_save_timer, AUTOSAVE_DELAY_US);
}

static void autosave_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = amv_store_save_slot(s_active, s_gen, s_w);
    if (err == ESP_OK) s_stats.saves++;
    uint8_t slot = s_active;
    xSemaphoreGive(s_mtx);
    if (err == ESP_OK) ESP_LOGI(TAG, "auto-saved slot %u", slot);
    else ESP_LOGE(TAG, "auto-save failed: %s", esp_err_to_name(err));
}

static void set_neutral_locked(void)
{
    for (int i = 0; i < 8; i++) s_w[i] = 1.0f;
}

esp_err_t amv_ctrl_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t targs = { .callback = autosave_cb, .name = "amv_autosave" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_save_timer));
    ESP_ERROR_CHECK(amv_store_init());

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    set_neutral_locked();
    s_gen = 1;
    s_active = 0;
    uint8_t active;
    if (amv_store_get_active(&active) == ESP_OK) s_active = active;

    uint32_t gen;
    float w[8];
    esp_err_t err = amv_store_load_slot(s_active, &gen, w);
    if (err == ESP_OK && weights_valid(w)) {
        memcpy(s_w, w, sizeof(w));
        s_gen = gen ? gen : 1;
        ESP_LOGI(TAG, "restored slot %u (gen %u)", s_active, (unsigned)s_gen);
    } else {
        ESP_LOGW(TAG, "slot %u not restored (%s); starting neutral", s_active, esp_err_to_name(err));
    }
    publish_locked();
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

void amv_ctrl_subscribe(amv_event_cb_t cb)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_nsubs < MAX_SUBSCRIBERS) s_subs[s_nsubs++] = cb;
    xSemaphoreGive(s_mtx);
}

void amv_ctrl_get_state(amv_state_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    fill_state_locked(out);
    xSemaphoreGive(s_mtx);
}

void amv_ctrl_get_stats(amv_ctrl_stats_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_mtx);
}

amv_result_t amv_ctrl_set_weights(uint32_t base_gen, const float w[8], uint8_t source, amv_state_t *out)
{
    amv_state_t st;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!weights_valid(w)) {
        s_stats.bad_value_rejects++;
        fill_state_locked(out);
        xSemaphoreGive(s_mtx);
        return AMV_R_BAD_VALUE;
    }
    if (base_gen != AMV_GEN_ANY && base_gen != s_gen) {
        s_stats.stale_rejects++;
        fill_state_locked(out);
        xSemaphoreGive(s_mtx);
        return AMV_R_STALE;
    }
    memcpy(s_w, w, sizeof(s_w));
    s_gen++;
    s_stats.sets_ok++;
    publish_locked();
    fill_state_locked(&st);
    xSemaphoreGive(s_mtx);

    arm_autosave();
    if (out) *out = st;
    notify(&st, source);
    return AMV_R_OK;
}

amv_result_t amv_ctrl_set_bypass(bool on, uint8_t source, amv_state_t *out)
{
    amv_state_t st;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_bypass = on;
    s_gen++;
    publish_locked();
    fill_state_locked(&st);
    xSemaphoreGive(s_mtx);
    /* Bypass is a runtime failsafe and is deliberately not persisted. */
    if (out) *out = st;
    notify(&st, source);
    return AMV_R_OK;
}

/* Refusals still hand back the current state, so a caller never forwards uninitialized memory. */
static amv_result_t refuse(amv_result_t r, amv_state_t *out)
{
    if (out) amv_ctrl_get_state(out);
    return r;
}

amv_result_t amv_ctrl_save_slot(uint8_t slot, amv_state_t *out)
{
    if (slot >= AMV_NUM_SLOTS) return refuse(AMV_R_BAD_SLOT, out);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = amv_store_save_slot(slot, s_gen, s_w);
    if (err == ESP_OK) err = amv_store_set_active(slot);
    if (err == ESP_OK) {
        s_active = slot;
        s_stats.saves++;
    }
    fill_state_locked(out);
    xSemaphoreGive(s_mtx);
    return err == ESP_OK ? AMV_R_OK : AMV_R_STORE;
}

amv_result_t amv_ctrl_load_slot(uint8_t slot, uint8_t source, amv_state_t *out)
{
    if (slot >= AMV_NUM_SLOTS) return refuse(AMV_R_BAD_SLOT, out);
    uint32_t gen;
    float w[8];
    esp_err_t err = amv_store_load_slot(slot, &gen, w);
    if (err == ESP_ERR_NOT_FOUND) return refuse(AMV_R_EMPTY, out);
    if (err != ESP_OK || !weights_valid(w)) return refuse(AMV_R_STORE, out);

    amv_state_t st;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_w, w, sizeof(w));
    s_gen++;                       /* generation only ever increases while running */
    s_active = slot;
    amv_store_set_active(slot);
    s_stats.loads++;
    publish_locked();
    fill_state_locked(&st);
    xSemaphoreGive(s_mtx);

    if (out) *out = st;
    notify(&st, source);
    return AMV_R_OK;
}

amv_result_t amv_ctrl_factory_reset(uint8_t source, amv_state_t *out)
{
    amv_state_t st;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_save_timer);
    esp_err_t err = amv_store_erase_all();
    if (err != ESP_OK) {
        fill_state_locked(out);
        xSemaphoreGive(s_mtx);
        return AMV_R_STORE;
    }
    set_neutral_locked();
    s_bypass = false;
    s_active = 0;
    s_gen++;
    publish_locked();
    fill_state_locked(&st);
    xSemaphoreGive(s_mtx);

    if (out) *out = st;
    notify(&st, source);
    return AMV_R_OK;
}

uint8_t amv_ctrl_slots_mask(void)
{
    uint8_t mask = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < AMV_NUM_SLOTS; i++) {
        uint32_t gen;
        float w[8];
        if (amv_store_load_slot(i, &gen, w) == ESP_OK && weights_valid(w)) mask |= (uint8_t)(1u << i);
    }
    xSemaphoreGive(s_mtx);
    return mask;
}
