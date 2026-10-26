#include "amv_proto.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "tusb.h"
#include "amv_ctrl.h"
#include "amv_octant.h"
#include "amv_usb_host.h"

static const char *TAG = "amv_proto";

typedef struct {
    uint8_t is_tx;                      /* 1: ready-to-send frame (event); 0: received request to process */
    uint8_t buf[AMV_FRAME_SIZE];
} item_t;

static QueueHandle_t s_q;
static amv_octant_state_t s_test_rem;   /* TEST_SCALE remainder, touched only by the proto task */
static uint32_t s_events_sent, s_events_dropped, s_test_scales;

/* TEST_SELF_CHANGE: one pending delayed change, applied from a timer. */
static esp_timer_handle_t s_self_timer;
static volatile bool s_self_pending;
static uint8_t s_self_octant;
static float s_self_value;

static esp_timer_handle_t s_reboot_timer;

/* ---------- frame helpers ---------- */

static void build_frame(uint8_t out[AMV_FRAME_SIZE], uint8_t cmd, uint8_t seq, uint8_t status,
                        const void *payload, uint8_t len)
{
    memset(out, 0, AMV_FRAME_SIZE);
    amv_hdr_t *h = (amv_hdr_t *)out;
    h->magic0 = AMV_MAGIC0;
    h->magic1 = AMV_MAGIC1;
    h->ver = AMV_PROTO_VERSION;
    h->cmd = cmd;
    h->seq = seq;
    h->status = status;
    h->len = len;
    if (len) memcpy(out + AMV_HEADER_SIZE, payload, len);
}

static void to_payload(const amv_state_t *s, amv_state_payload_t *p)
{
    p->gen = s->gen;
    memcpy(p->w, s->w, sizeof(p->w));
    p->bypass = s->bypass ? 1 : 0;
    p->active_slot = s->active_slot;
}

static uint8_t result_to_status(amv_result_t r)
{
    switch (r) {
    case AMV_R_OK:        return AMV_ST_OK;
    case AMV_R_BAD_VALUE: return AMV_ST_BAD_VALUE;
    case AMV_R_STALE:     return AMV_ST_STALE;
    case AMV_R_BAD_SLOT:  return AMV_ST_BAD_SLOT;
    case AMV_R_EMPTY:     return AMV_ST_EMPTY;
    default:              return AMV_ST_STORE_ERR;
    }
}

/* ---------- state-change events (any thread) ---------- */

static void on_state_event(const amv_state_t *st, uint8_t source)
{
    item_t it = { .is_tx = 1 };
    amv_event_payload_t ev;
    to_payload(st, &ev.st);
    ev.source = source;
    build_frame(it.buf, AMV_CMD_EVENT_STATE, 0, AMV_ST_OK, &ev, sizeof(ev));
    if (xQueueSend(s_q, &it, 0) != pdTRUE) s_events_dropped++;
}

static void on_mouse_event(const amv_mouse_event_t *ev)
{
    item_t it = { .is_tx = 1 };
    build_frame(it.buf, AMV_CMD_EVENT_MOUSE, 0, AMV_ST_OK, ev, sizeof(*ev));
    if (xQueueSend(s_q, &it, 0) != pdTRUE) s_events_dropped++;
}

/* ---------- command handling (proto task) ---------- */

static void reply_state(uint8_t out[AMV_FRAME_SIZE], uint8_t cmd, uint8_t seq, amv_result_t r, const amv_state_t *s)
{
    amv_state_payload_t p;
    to_payload(s, &p);
    build_frame(out, cmd, seq, result_to_status(r), &p, sizeof(p));
}

static void self_change_cb(void *arg)
{
    (void)arg;
    amv_state_t cur;
    amv_ctrl_get_state(&cur);
    float w[8];
    memcpy(w, cur.w, sizeof(w));
    w[s_self_octant & 7] = s_self_value;
    amv_ctrl_set_weights(AMV_GEN_ANY, w, AMV_SRC_DEVICE, NULL);
    s_self_pending = false;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void handle_request(const uint8_t *in, uint8_t out[AMV_FRAME_SIZE])
{
    const amv_hdr_t *h = (const amv_hdr_t *)in;
    const uint8_t *pl = in + AMV_HEADER_SIZE;
    uint8_t seq = h->seq, cmd = h->cmd, len = h->len;
    amv_state_t st;

    if (h->magic0 != AMV_MAGIC0 || h->magic1 != AMV_MAGIC1) {
        build_frame(out, cmd, seq, AMV_ST_BAD_MAGIC, NULL, 0);
        return;
    }
    if (len > AMV_PAYLOAD_MAX) {
        build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0);
        return;
    }

    switch (cmd) {
    case AMV_CMD_GET_INFO: {
        amv_info_payload_t p = { .proto_ver = AMV_PROTO_VERSION, .fw_major = AMV_FW_MAJOR,
                                 .fw_minor = AMV_FW_MINOR, .fw_patch = AMV_FW_PATCH,
                                 .num_slots = AMV_NUM_SLOTS, .has_mouse = amv_ctrl_mouse_count() };
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        p.chip_rev = ci.revision;
        esp_efuse_mac_get_default(p.mac);
        p.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        build_frame(out, cmd, seq, AMV_ST_OK, &p, sizeof(p));
        break;
    }
    case AMV_CMD_GET_WEIGHTS:
        amv_ctrl_get_state(&st);
        reply_state(out, cmd, seq, AMV_R_OK, &st);
        break;
    case AMV_CMD_SET_WEIGHTS: {
        if (len != sizeof(amv_set_weights_req_t)) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_set_weights_req_t req;
        memcpy(&req, pl, sizeof(req));
        float w[8];                          /* aligned copy; req is packed */
        memcpy(w, req.w, sizeof(w));
        amv_result_t r = amv_ctrl_set_weights(req.base_gen, w, AMV_SRC_HOST, &st);
        reply_state(out, cmd, seq, r, &st);
        break;
    }
    case AMV_CMD_SAVE_PROFILE: {
        if (len != 1) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_result_t r = amv_ctrl_save_slot(pl[0], &st);
        reply_state(out, cmd, seq, r, &st);
        break;
    }
    case AMV_CMD_LOAD_PROFILE: {
        if (len != 1) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_result_t r = amv_ctrl_load_slot(pl[0], AMV_SRC_LOAD, &st);
        if (r != AMV_R_OK) amv_ctrl_get_state(&st);
        reply_state(out, cmd, seq, r, &st);
        break;
    }
    case AMV_CMD_SET_BYPASS: {
        if (len != 1) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_ctrl_set_bypass(pl[0] != 0, AMV_SRC_HOST, &st);
        reply_state(out, cmd, seq, AMV_R_OK, &st);
        break;
    }
    case AMV_CMD_GET_STATS: {
        amv_ctrl_stats_t cs;
        amv_ctrl_get_stats(&cs);
        amv_stats_payload_t p = { .sets_ok = cs.sets_ok, .stale_rejects = cs.stale_rejects,
                                  .bad_value_rejects = cs.bad_value_rejects, .saves = cs.saves,
                                  .loads = cs.loads, .events_sent = s_events_sent,
                                  .events_dropped = s_events_dropped, .test_scales = s_test_scales };
        build_frame(out, cmd, seq, AMV_ST_OK, &p, sizeof(p));
        break;
    }
    case AMV_CMD_TEST_SCALE: {
        if (len != sizeof(amv_test_scale_req_t)) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_test_scale_req_t req;
        memcpy(&req, pl, sizeof(req));
        if (req.flags & AMV_TEST_RESET_REM) memset(&s_test_rem, 0, sizeof(s_test_rem));
        float w[8];
        amv_ctrl_snapshot_effective(w);         /* the same lock-free read the hot path uses */
        amv_test_scale_rep_t rep;
        int32_t ox, oy;
        uint8_t oct;
        amv_octant_scale(w, &s_test_rem, req.dx, req.dy, &ox, &oy, &oct);
        rep.out_dx = ox; rep.out_dy = oy;
        rep.rem_x = s_test_rem.rem_x; rep.rem_y = s_test_rem.rem_y;
        rep.octant = oct;
        s_test_scales++;
        build_frame(out, cmd, seq, AMV_ST_OK, &rep, sizeof(rep));
        break;
    }
    case AMV_CMD_TEST_SELF_CHANGE: {
        if (len != sizeof(amv_test_self_change_req_t)) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_test_self_change_req_t req;
        memcpy(&req, pl, sizeof(req));
        if (req.octant > 7) { build_frame(out, cmd, seq, AMV_ST_BAD_VALUE, NULL, 0); break; }
        if (s_self_pending) { build_frame(out, cmd, seq, AMV_ST_BUSY, NULL, 0); break; }
        s_self_octant = req.octant;
        s_self_value = req.value;
        s_self_pending = true;
        esp_timer_start_once(s_self_timer, (uint64_t)(req.delay_ms ? req.delay_ms : 1) * 1000);
        build_frame(out, cmd, seq, AMV_ST_OK, NULL, 0);
        break;
    }
    case AMV_CMD_GET_SLOTS: {
        uint8_t mask = amv_ctrl_slots_mask();
        build_frame(out, cmd, seq, AMV_ST_OK, &mask, 1);
        break;
    }
    case AMV_CMD_HELLO: {
        if (len != sizeof(amv_hello_req_t)) {
            build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0);
            break;
        }
        amv_hello_rep_t p = { .ident = AMV_HELLO_IDENT, .proto_ver = AMV_PROTO_VERSION,
                              .fw_major = AMV_FW_MAJOR, .fw_minor = AMV_FW_MINOR, .fw_patch = AMV_FW_PATCH,
                              .caps = AMV_CAP_VECTORED_MOUSE | AMV_CAP_MOUSE_HOST };
        memcpy(p.nonce, pl, sizeof(p.nonce));
        esp_efuse_mac_get_default(p.mac);
        build_frame(out, cmd, seq, AMV_ST_OK, &p, sizeof(p));
        break;
    }
    case AMV_CMD_GET_MOUSE: {
        if (len < 1) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_mouse_info_t p;
        uint8_t s = amv_mouse_get_info(pl[0], &p);
        build_frame(out, cmd, seq, s, &p, s == AMV_ST_OK ? sizeof(p) : 2);
        break;
    }
    case AMV_CMD_GET_MOUSE_CAPS: {
        if (len < 1) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_mouse_caps_t p;
        uint8_t s = amv_mouse_get_caps(pl[0], &p);
        build_frame(out, cmd, seq, s, &p, s == AMV_ST_OK ? sizeof(p) : 0);
        break;
    }
    case AMV_CMD_GET_MOUSE_DESC: {
        if (len < sizeof(amv_mouse_desc_req_t)) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_mouse_desc_req_t r;
        memcpy(&r, pl, sizeof r);
        amv_mouse_desc_rep_t p;
        uint8_t s = amv_mouse_get_desc(r.index, r.offset, &p);
        build_frame(out, cmd, seq, s, &p, s == AMV_ST_OK ? sizeof(p) : 0);
        break;
    }
    case AMV_CMD_SET_MOUSE_CPI:
    case AMV_CMD_SET_MOUSE_POLLING: {
        if (len < sizeof(amv_mouse_set_req_t)) { build_frame(out, cmd, seq, AMV_ST_BAD_LEN, NULL, 0); break; }
        amv_mouse_set_req_t r;
        memcpy(&r, pl, sizeof r);
        amv_mouse_info_t p;
        uint8_t s = cmd == AMV_CMD_SET_MOUSE_CPI ? amv_mouse_set_cpi(r.index, r.value, &p) : amv_mouse_set_rate(r.index, r.value, &p);
        build_frame(out, cmd, seq, s, &p, s == AMV_ST_NO_MOUSE ? 2 : sizeof(p));
        break;
    }
    case AMV_CMD_FACTORY_RESET: {
        amv_result_t r = amv_ctrl_factory_reset(AMV_SRC_HOST, &st);
        if (r != AMV_R_OK) amv_ctrl_get_state(&st);
        reply_state(out, cmd, seq, r, &st);
        break;
    }
    case AMV_CMD_REBOOT:
        build_frame(out, cmd, seq, AMV_ST_OK, NULL, 0);
        esp_timer_start_once(s_reboot_timer, 150 * 1000);   /* let the reply out first */
        break;
    default:
        build_frame(out, cmd, seq, AMV_ST_BAD_CMD, NULL, 0);
        break;
    }
}

/* ---------- task: process requests, send replies/events ---------- */

static bool send_frame(const uint8_t *buf)
{
    if (!tud_mounted()) return false;
    for (int i = 0; i < 50; i++) {
        if (tud_hid_n_ready(AMV_HID_INSTANCE_VENDOR)) {
            return tud_hid_n_report(AMV_HID_INSTANCE_VENDOR, 0, buf, AMV_FRAME_SIZE);
        }
        vTaskDelay(1);
    }
    return false;
}

static void proto_task(void *arg)
{
    (void)arg;
    item_t it;
    uint8_t out[AMV_FRAME_SIZE];
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        const uint8_t *frame;
        bool is_event = it.is_tx && it.buf[3] == AMV_CMD_EVENT_STATE;
        if (it.is_tx) {
            frame = it.buf;
        } else {
            handle_request(it.buf, out);
            frame = out;
        }
        if (send_frame(frame)) {
            if (is_event) s_events_sent++;
        } else if (is_event) {
            s_events_dropped++;
        }
    }
}

void amv_proto_on_rx(const uint8_t *buf, uint16_t len)
{
    item_t it = { .is_tx = 0 };
    if (len > AMV_FRAME_SIZE) len = AMV_FRAME_SIZE;
    memcpy(it.buf, buf, len);
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "rx queue full, request dropped");
    }
}

void amv_proto_init(void)
{
    s_q = xQueueCreate(32, sizeof(item_t));
    configASSERT(s_q);
    amv_mouse_set_event_cb(on_mouse_event);
    const esp_timer_create_args_t a1 = { .callback = self_change_cb, .name = "amv_selfchg" };
    const esp_timer_create_args_t a2 = { .callback = reboot_cb, .name = "amv_reboot" };
    ESP_ERROR_CHECK(esp_timer_create(&a1, &s_self_timer));
    ESP_ERROR_CHECK(esp_timer_create(&a2, &s_reboot_timer));
    amv_ctrl_subscribe(on_state_event);
    xTaskCreatePinnedToCore(proto_task, "amv_proto", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "protocol v%d ready", AMV_PROTO_VERSION);
}
