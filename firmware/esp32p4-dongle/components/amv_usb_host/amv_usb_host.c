#include "amv_usb_host.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "soc/usb_dwc_struct.h"
#include "usb/usb_helpers.h"
#include "amv_catalog.h"
#include "amv_ctrl.h"
#include "amv_hid_parse.h"
#include "amv_mouse_drv.h"
#include "amv_store.h"
#include "amv_vector.h"

static const char *TAG = "amv_host";

#define MAX_IF        AMV_VECTOR_MAX_SOURCES
#define XFERS_PER_EP  3
#define MAX_REPORT_DESC 1024
#define HID_CLASS     3
#define MAX_IGNORED   4
#define RATE_WINDOW   10      /* seconds of per-second report counts kept for measured_hz */
#define REFRESH_MS    5000    /* tier-1 re-read (CPI changes with the mouse's own DPI button) */

/* A vendor interface claimed for a driver (amv_drv_t.cfg_match); owned by the driver's mouse slot */
typedef struct {
    bool used;
    uint8_t ifnum, ep_in, ep_out;
    uint16_t mps_in, mps_out;
    bool report_ids;
    QueueHandle_t q;          /* input reports, in_report_t */
    SemaphoreHandle_t out_sem;
    usb_transfer_t *xfer[2];
    volatile int inflight;
    volatile bool dying;
    usb_device_handle_t dev;
    uint32_t reports;
} cfg_if_t;

typedef struct {
    bool used;
    uint8_t tier;             /* AMV_TIER_KNOWN / GENERIC / BOOT */
    uint16_t uid;
    bool also_keyboard;
    uint8_t *desc;            /* this interface's report descriptor, for GET_MOUSE_DESC */
    uint16_t desc_len;
    uint16_t nominal_hz;
    uint16_t cpi_user;        /* label stored per VID:PID (tiers 2/3) */
    uint32_t last_reports;
    uint16_t hist[RATE_WINDOW];
    uint8_t hist_pos;
    const amv_drv_t *drv;     /* tier-1 driver, NULL otherwise */
    amv_drv_ctx_t ctx;        /* driver results (written under s_tab) */
    cfg_if_t cfg[AMV_DRV_MAX_CFG];
    bool probed;
    QueueHandle_t in_q;       /* vendor input reports for drivers with wants_input */
    volatile bool dying;
    usb_device_handle_t dev;
    uint8_t ifnum;
    uint8_t ep;
    uint16_t mps;
    uint8_t interval;
    uint16_t vid, pid;
    uint8_t speed;
    char name[40];
    amv_hid_layout_t layout;
    usb_transfer_t *xfer[XFERS_PER_EP];
    volatile int inflight;
    volatile uint32_t reports, errors;
    uint32_t consecutive_errors;
} mouse_if_t;

typedef struct {
    bool used;
    usb_device_handle_t dev;
    uint16_t uid, vid, pid;
    uint8_t speed, iface, reason;
    char name[40];
} ignored_t;

typedef enum { MSG_NEW, MSG_GONE } msg_type_t;
typedef struct { msg_type_t type; uint8_t addr; usb_device_handle_t dev; } msg_t;

static usb_host_client_handle_t s_client;
static QueueHandle_t s_q;
static mouse_if_t s_if[MAX_IF];
static ignored_t s_ign[MAX_IGNORED];
static SemaphoreHandle_t s_ctrl_sem;
static SemaphoreHandle_t s_ctrl_mtx;   /* one control transfer at a time (setup task, proto task) */
static SemaphoreHandle_t s_tab;        /* s_if / s_ign contents seen by other tasks */
static SemaphoreHandle_t s_drv;        /* driver exchanges; teardown waits for them */
static uint16_t s_next_uid = 1;
static amv_mouse_event_cb_t s_event_cb;

#if CONFIG_AMV_OPENMOUSE_JS
static const amv_drv_t *const DRIVERS[] = { &amv_drv_openmouse };
#else
static const amv_drv_t *const DRIVERS[] = { &amv_drv_razer, &amv_drv_finalmouse, &amv_drv_gearhub, &amv_drv_logitech };
#endif

#define TAB_LOCK()   xSemaphoreTake(s_tab, portMAX_DELAY)
#define TAB_UNLOCK() xSemaphoreGive(s_tab)

/* ---------- helpers ---------- */

static void emit(uint8_t change, uint16_t uid)
{
    if (!s_event_cb) return;
    amv_mouse_event_t ev = { .change = change, .uid = uid };
    int n = 0;
    for (int i = 0; i < MAX_IF; i++) n += s_if[i].used && !s_if[i].dying;
    for (int i = 0; i < MAX_IGNORED; i++) n += s_ign[i].used;
    ev.count = (uint8_t)n;
    s_event_cb(&ev);
}

static uint16_t nominal_rate(uint8_t speed, uint8_t interval)
{
    if (!interval) return 0;
    if (speed == USB_SPEED_HIGH) return interval > 16 ? 0 : (uint16_t)(8000 >> (interval - 1));
    return (uint16_t)(1000 / interval);
}

static void update_mouse_count(void)
{
    usb_device_handle_t seen[MAX_IF];
    int n = 0;
    for (int i = 0; i < MAX_IF; i++) {
        if (!s_if[i].used || s_if[i].dying) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (seen[k] == s_if[i].dev) dup = true;
        if (!dup) seen[n++] = s_if[i].dev;
    }
    amv_ctrl_set_mouse_count((uint8_t)n);
}

static void utf16_to_ascii(const usb_str_desc_t *s, char *out, size_t cap)
{
    size_t n = 0;
    if (s && s->bLength > 2) {
        for (int i = 0; i < (s->bLength - 2) / 2 && n + 1 < cap; i++) {
            uint16_t c = s->wData[i];
            out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

static void ctrl_done(usb_transfer_t *t)
{
    xSemaphoreGive((SemaphoreHandle_t)t->context);
}

/* Synchronous control transfer; the client task keeps running and delivers the callback. */
static esp_err_t ctrl_xfer_locked(usb_device_handle_t dev, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
                                  uint16_t len, uint8_t *data, int *actual)
{
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) != ESP_OK) return ESP_FAIL;
    int mps0 = info.bMaxPacketSize0 ? info.bMaxPacketSize0 : 64;
    bool in = bm & USB_BM_REQUEST_TYPE_DIR_IN;
    int data_len = in ? usb_round_up_to_mps(len, mps0) : len;
    usb_transfer_t *t;
    esp_err_t err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + data_len, 0, &t);
    if (err != ESP_OK) return err;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = bm;
    s->bRequest = req;
    s->wValue = val;
    s->wIndex = idx;
    s->wLength = len;
    if (!in && len) memcpy(t->data_buffer + USB_SETUP_PACKET_SIZE, data, len);
    t->device_handle = dev;
    t->bEndpointAddress = 0;
    t->callback = ctrl_done;
    t->context = s_ctrl_sem;
    t->num_bytes = USB_SETUP_PACKET_SIZE + data_len;
    xSemaphoreTake(s_ctrl_sem, 0);
    err = usb_host_transfer_submit_control(s_client, t);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;     /* leak the transfer rather than free one still owned by the stack */
            return err;
        }
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
            err = ESP_FAIL;
        } else if (in && data) {
            int got = t->actual_num_bytes - USB_SETUP_PACKET_SIZE;
            if (got > len) got = len;
            if (got < 0) got = 0;
            memcpy(data, t->data_buffer + USB_SETUP_PACKET_SIZE, got);
            if (actual) *actual = got;
        }
    }
    usb_host_transfer_free(t);
    return err;
}

static esp_err_t ctrl_xfer(usb_device_handle_t dev, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
                           uint16_t len, uint8_t *data, int *actual)
{
    xSemaphoreTake(s_ctrl_mtx, portMAX_DELAY);
    esp_err_t e = ctrl_xfer_locked(dev, bm, req, val, idx, len, data, actual);
    xSemaphoreGive(s_ctrl_mtx);
    return e;
}

typedef struct { uint8_t len; uint8_t data[64]; } in_report_t;

/* ---------- transport for the tier-1 drivers (amv_mouse_drv.h) ---------- */

esp_err_t amv_host_set_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len)
{
    uint8_t buf[128];
    if (len > sizeof buf) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, data, len);
    return ctrl_xfer(c->dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x09,
                     (uint16_t)((type << 8) | id), iface, len, buf, NULL);
}

esp_err_t amv_host_get_report(amv_drv_ctx_t *c, uint8_t iface, uint8_t type, uint8_t id, uint8_t *data, uint16_t len, int *actual)
{
    return ctrl_xfer(c->dev, USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x01,
                     (uint16_t)((type << 8) | id), iface, len, data, actual);
}

int amv_host_read_input(amv_drv_ctx_t *c, uint8_t *buf, int cap, int timeout_ms)
{
    in_report_t r;
    if (!c->in_q || xQueueReceive(c->in_q, &r, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    int n = r.len < cap ? r.len : cap;
    memcpy(buf, r.data, n);
    return n;
}

/* ---------- WebHID-shaped transport over claimed vendor interfaces (amv_hl_*) ---------- */

static cfg_if_t *cfg_of(amv_drv_ctx_t *c, int n)
{
    return n >= 0 && n < c->n_cfg ? (cfg_if_t *)c->cfg[n].h : NULL;
}

static void cfg_out_done(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

esp_err_t amv_hl_send_report(amv_drv_ctx_t *c, int n, uint8_t report_id, const uint8_t *data, uint16_t len)
{
    cfg_if_t *f = cfg_of(c, n);
    if (!f || f->dying) return ESP_ERR_INVALID_STATE;
    uint8_t buf[128];
    uint16_t total = (uint16_t)(len + (report_id ? 1 : 0));
    if (total > sizeof buf) return ESP_ERR_INVALID_SIZE;
    if (report_id) buf[0] = report_id;
    memcpy(buf + (report_id ? 1 : 0), data, len);
    if (!f->ep_out) {   /* no interrupt OUT endpoint: SET_REPORT(output), as WebHID falls back to */
        return ctrl_xfer(f->dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x09,
                         (uint16_t)((2 << 8) | report_id), f->ifnum, total, buf, NULL);
    }
    usb_transfer_t *t;
    esp_err_t err = usb_host_transfer_alloc(total, 0, &t);
    if (err != ESP_OK) return err;
    memcpy(t->data_buffer, buf, total);
    t->device_handle = f->dev;
    t->bEndpointAddress = f->ep_out;
    t->callback = cfg_out_done;
    t->context = f->out_sem;
    t->num_bytes = total;
    xSemaphoreTake(f->out_sem, 0);
    err = usb_host_transfer_submit(t);
    if (err == ESP_OK) {
        if (xSemaphoreTake(f->out_sem, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;   /* leaked: still owned */
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) err = ESP_FAIL;
    }
    usb_host_transfer_free(t);
    return err;
}

esp_err_t amv_hl_send_feature(amv_drv_ctx_t *c, int n, uint8_t report_id, const uint8_t *data, uint16_t len)
{
    cfg_if_t *f = cfg_of(c, n);
    if (!f || f->dying) return ESP_ERR_INVALID_STATE;
    uint8_t buf[128];
    uint16_t total = (uint16_t)(len + (report_id ? 1 : 0));
    if (total > sizeof buf) return ESP_ERR_INVALID_SIZE;
    if (report_id) buf[0] = report_id;
    memcpy(buf + (report_id ? 1 : 0), data, len);
    return ctrl_xfer(f->dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x09,
                     (uint16_t)((3 << 8) | report_id), f->ifnum, total, buf, NULL);
}

esp_err_t amv_hl_receive_feature(amv_drv_ctx_t *c, int n, uint8_t report_id, uint8_t *data, uint16_t cap, int *actual)
{
    cfg_if_t *f = cfg_of(c, n);
    if (!f || f->dying) return ESP_ERR_INVALID_STATE;
    uint8_t buf[129];
    uint16_t want = (uint16_t)(cap + (report_id ? 1 : 0));
    if (want > sizeof buf) return ESP_ERR_INVALID_SIZE;
    int got = 0;
    esp_err_t e = ctrl_xfer(f->dev, USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x01,
                            (uint16_t)((3 << 8) | report_id), f->ifnum, want, buf, &got);
    if (e != ESP_OK) return e;
    int skip = (report_id && got > 0) ? 1 : 0;      /* the ID byte is not part of WebHID's data */
    if (got - skip > cap) got = cap + skip;
    memcpy(data, buf + skip, (size_t)(got - skip));
    if (actual) *actual = got - skip;
    return ESP_OK;
}

int amv_hl_wait_input(amv_drv_ctx_t *c, int n, uint8_t *report_id, uint8_t *data, int cap, int timeout_ms)
{
    cfg_if_t *f = cfg_of(c, n);
    in_report_t r;
    if (!f || !f->q || xQueueReceive(f->q, &r, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    int skip = f->report_ids ? 1 : 0;
    if (report_id) *report_id = f->report_ids ? r.data[0] : 0;
    int len = r.len - skip;
    if (len < 0) len = 0;
    if (len > cap) len = cap;
    memcpy(data, r.data + skip, (size_t)len);
    return len > 0 ? len : 1;                       /* a bare report ID still counts as a report */
}

void amv_hl_flush_input(amv_drv_ctx_t *c, int n)
{
    cfg_if_t *f = cfg_of(c, n);
    if (f && f->q) xQueueReset(f->q);
}

/* ---------- interrupt IN handling ---------- */

static void cfg_in_cb(usb_transfer_t *t)
{
    cfg_if_t *f = t->context;
    bool resubmit = false;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (t->actual_num_bytes > 0 && f->q) {
            in_report_t v = { .len = (uint8_t)(t->actual_num_bytes > 64 ? 64 : t->actual_num_bytes) };
            memcpy(v.data, t->data_buffer, v.len);
            if (xQueueSend(f->q, &v, 0) != pdTRUE) {   /* full: drop the oldest, keep the newest */
                in_report_t drop;
                xQueueReceive(f->q, &drop, 0);
                xQueueSend(f->q, &v, 0);
            }
            f->reports++;
        }
        resubmit = !f->dying;
    } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) {
        if (t->status == USB_TRANSFER_STATUS_STALL) usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
        resubmit = !f->dying;
    }
    if (resubmit) {
        t->num_bytes = f->mps_in;
        if (usb_host_transfer_submit(t) == ESP_OK) return;
    }
    __atomic_fetch_sub(&f->inflight, 1, __ATOMIC_SEQ_CST);
}

static void in_cb(usb_transfer_t *t)
{
    mouse_if_t *m = t->context;
    unsigned src = (unsigned)(m - s_if);
    bool resubmit = false;

    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        m->consecutive_errors = 0;
        if (t->actual_num_bytes > 0) {
            amv_hid_report_t r;
            if (amv_hid_decode(&m->layout, t->data_buffer, (size_t)t->actual_num_bytes, &r)) {
                bool btn = r.have & AMV_HID_HAVE_BUTTONS;
                amv_vector_feed(src, r.dx, r.dy, r.wheel, r.pan, btn, r.buttons);
            } else if (m->in_q) {
                /* not motion: a vendor reply for the tier-1 driver (e.g. Finalmouse input report 0x05) */
                in_report_t v = { .len = (uint8_t)(t->actual_num_bytes > 64 ? 64 : t->actual_num_bytes) };
                memcpy(v.data, t->data_buffer, v.len);
                xQueueSend(m->in_q, &v, 0);
            }
            m->reports++;
        }
        resubmit = !m->dying;
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
        resubmit = false;
    } else {
        m->errors++;
        resubmit = !m->dying && ++m->consecutive_errors < 50;
        if (t->status == USB_TRANSFER_STATUS_STALL) usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    }
    if (resubmit) {
        t->num_bytes = m->mps;
        if (usb_host_transfer_submit(t) == ESP_OK) return;
    }
    __atomic_fetch_sub(&m->inflight, 1, __ATOMIC_SEQ_CST);
}

/* ---------- device setup / teardown (setup task) ---------- */

typedef struct { uint8_t ifnum, subclass, protocol, ep, ep_out, interval; uint16_t mps, mps_out, report_len; } hid_if_t;

static int collect_hid_interfaces(const usb_config_desc_t *cfg, hid_if_t *out, int max)
{
    int n = 0, off = 0;
    hid_if_t cur = { 0 };
    bool have = false;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    uint16_t total = cfg->wTotalLength;
    #define FLUSH() do { if (have && cur.report_len && cur.ep && n < max) out[n++] = cur; } while (0)
    while ((d = usb_parse_next_descriptor(d, total, &off)) != NULL) {
        const uint8_t *b = (const uint8_t *)d;
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            FLUSH();
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            have = (i->bInterfaceClass == HID_CLASS && i->bAlternateSetting == 0);
            cur = (hid_if_t){ .ifnum = i->bInterfaceNumber, .subclass = i->bInterfaceSubClass, .protocol = i->bInterfaceProtocol };
        } else if (have && d->bDescriptorType == 0x21 && d->bLength >= 9 && b[6] == 0x22) {
            cur.report_len = (uint16_t)(b[7] | (b[8] << 8));
        } else if (have && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bEndpointAddress & 0x80) && (e->bmAttributes & 3) == 3 && !cur.ep) {
                cur.ep = e->bEndpointAddress;
                cur.mps = USB_EP_DESC_GET_MPS(e);
                cur.interval = e->bInterval;
            } else if (!(e->bEndpointAddress & 0x80) && (e->bmAttributes & 3) == 3 && !cur.ep_out) {
                cur.ep_out = e->bEndpointAddress;
                cur.mps_out = USB_EP_DESC_GET_MPS(e);
            }
        }
    }
    FLUSH();
    #undef FLUSH
    return n;
}

static void teardown_if(mouse_if_t *m)
{
    xSemaphoreTake(s_drv, portMAX_DELAY);   /* let a running driver exchange finish */
    if (m->drv && m->drv->release) m->drv->release(&m->ctx);   /* its transport is still valid here */
    for (int w = 0; w < 100 && m->inflight > 0; w++) vTaskDelay(pdMS_TO_TICKS(5));
    if (m->inflight > 0) ESP_LOGW(TAG, "if %d: %d transfers still in flight at teardown", m->ifnum, m->inflight);
    for (int k = 0; k < XFERS_PER_EP; k++) {
        if (m->xfer[k] && m->inflight == 0) usb_host_transfer_free(m->xfer[k]);
        m->xfer[k] = NULL;
    }
    for (int i = 0; i < AMV_DRV_MAX_CFG; i++) {
        cfg_if_t *f = &m->cfg[i];
        if (!f->used) continue;
        f->dying = true;
        for (int w = 0; w < 100 && f->inflight > 0; w++) vTaskDelay(pdMS_TO_TICKS(5));
        for (int k = 0; k < 2; k++) {
            if (f->xfer[k] && f->inflight == 0) usb_host_transfer_free(f->xfer[k]);
            f->xfer[k] = NULL;
        }
        usb_host_interface_release(s_client, f->dev, f->ifnum);
        if (f->q) vQueueDelete(f->q);
        if (f->out_sem) vSemaphoreDelete(f->out_sem);
        f->q = NULL;
        f->out_sem = NULL;
        f->used = false;
    }
    usb_host_interface_release(s_client, m->dev, m->ifnum);
    amv_vector_release((unsigned)(m - s_if));
    TAB_LOCK();
    free(m->desc);
    m->desc = NULL;
    if (m->in_q) vQueueDelete(m->in_q);
    m->in_q = NULL;
    m->used = false;
    TAB_UNLOCK();
    xSemaphoreGive(s_drv);
}

static void start_reading(mouse_if_t *m, int *started)
{
    *started = 0;
    for (int x = 0; x < XFERS_PER_EP; x++) {
        if (usb_host_transfer_alloc(m->mps, 0, &m->xfer[x]) != ESP_OK) break;
        usb_transfer_t *t = m->xfer[x];
        t->device_handle = m->dev;
        t->bEndpointAddress = m->ep;
        t->callback = in_cb;
        t->context = m;
        t->num_bytes = m->mps;
        __atomic_fetch_add(&m->inflight, 1, __ATOMIC_SEQ_CST);
        if (usb_host_transfer_submit(t) != ESP_OK) {
            __atomic_fetch_sub(&m->inflight, 1, __ATOMIC_SEQ_CST);
            break;
        }
        (*started)++;
    }
}

/* Run the tier-1 driver's probe on a copy, then publish the results. */
static void probe_driver(mouse_if_t *m, const hid_if_t *hi, uint8_t *const *descs, const int *lens, int nhid)
{
    amv_drv_ctx_t c = m->ctx;
    c.n_hid_ifs = 0;
    for (int k = 0; k < nhid && c.n_hid_ifs < AMV_DRV_MAX_HID_IFS; k++) {
        if (!descs[k]) continue;
        c.hid_ifs[c.n_hid_ifs++] = (amv_drv_hid_if_t){ .ifnum = hi[k].ifnum, .desc = descs[k], .desc_len = (uint16_t)lens[k] };
    }
    xSemaphoreTake(s_drv, portMAX_DELAY);
    esp_err_t e = m->drv->probe(&c);
    xSemaphoreGive(s_drv);
    c.n_hid_ifs = 0;                       /* the descriptor pointers are freed after attach */
    TAB_LOCK();
    m->ctx = c;
    m->probed = true;
    if (e == ESP_OK) m->tier = AMV_TIER_KNOWN;
    TAB_UNLOCK();
}

static uint8_t ignore_reason_rank(uint8_t r)
{
    return r == AMV_IGN_UNPARSED ? 4 : r == AMV_IGN_CLAIM ? 3 : r == AMV_IGN_ABSOLUTE ? 2 : r == AMV_IGN_NO_SLOT ? 1 : 0;
}

static void handle_new(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "open device %u failed", addr);
        return;
    }
    const usb_device_desc_t *dd;
    usb_device_info_t info;
    const usb_config_desc_t *cfg;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || usb_host_device_info(dev, &info) != ESP_OK ||
        usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        usb_host_device_close(s_client, dev);
        return;
    }
    char man[32], prod[40];
    utf16_to_ascii(info.str_desc_manufacturer, man, sizeof man);
    utf16_to_ascii(info.str_desc_product, prod, sizeof prod);
    ESP_LOGI(TAG, "device %04x:%04x \"%s\" \"%s\" speed=%s class=%02x", dd->idVendor, dd->idProduct, man, prod,
             info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low", dd->bDeviceClass);

    hid_if_t hi[AMV_DRV_MAX_HID_IFS];
    int nhid = collect_hid_interfaces(cfg, hi, AMV_DRV_MAX_HID_IFS);
    uint8_t *descs[AMV_DRV_MAX_HID_IFS] = { 0 };
    int lens[AMV_DRV_MAX_HID_IFS] = { 0 };

    /* Read every HID report descriptor first: a driver may need a sibling interface (GearHub's config) */
    for (int k = 0; k < nhid; k++) {
        uint16_t rlen = hi[k].report_len > MAX_REPORT_DESC ? MAX_REPORT_DESC : hi[k].report_len;
        descs[k] = malloc(rlen);
        if (!descs[k]) continue;
        if (ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | 1, 0x06, 0x2200,
                      hi[k].ifnum, rlen, descs[k], &lens[k]) != ESP_OK || lens[k] <= 0) {
            ESP_LOGW(TAG, "  if %u: could not read the report descriptor", hi[k].ifnum);
            free(descs[k]);
            descs[k] = NULL;
            continue;
        }
        ESP_LOGI(TAG, "  if %u: HID subclass %u protocol %u, ep 0x%02x mps %u interval %u, report descriptor %d bytes:",
                 hi[k].ifnum, hi[k].subclass, hi[k].protocol, hi[k].ep, hi[k].mps, hi[k].interval, lens[k]);
        ESP_LOG_BUFFER_HEX(TAG, descs[k], lens[k]);
    }

    const amv_drv_t *drv = NULL;
    for (size_t d = 0; d < sizeof DRIVERS / sizeof DRIVERS[0]; d++) {
        if (DRIVERS[d]->match(dd->idVendor, dd->idProduct)) drv = DRIVERS[d];
    }
    uint16_t cpi_label = 0;
    amv_store_get_mouse_cpi(dd->idVendor, dd->idProduct, &cpi_label);

    int claimed = 0;
    uint8_t reason = AMV_IGN_NOT_MOUSE;
    mouse_if_t *first = NULL;

    for (int k = 0; k < nhid; k++) {
        if (!descs[k]) { if (ignore_reason_rank(AMV_IGN_CLAIM) > ignore_reason_rank(reason)) reason = AMV_IGN_CLAIM; continue; }
        amv_hid_layout_t L;
        int pr = amv_hid_parse(descs[k], (size_t)lens[k], &L);
        uint8_t tier = AMV_TIER_GENERIC;
        bool boot_mouse = hi[k].subclass == 1 && hi[k].protocol == 2;
        if (pr < 0 || !L.valid) {
            if (boot_mouse && !(pr >= 0 && L.absolute)) {
                /* Tier 3: the descriptor is unusable but the interface speaks the boot mouse protocol */
                amv_hid_parse(amv_hid_boot_mouse_desc, amv_hid_boot_mouse_desc_len, &L);
                tier = AMV_TIER_BOOT;
                ESP_LOGW(TAG, "  if %u: report descriptor unusable (%d); falling back to boot protocol (8-bit, 3 buttons)",
                         hi[k].ifnum, pr);
            } else {
                uint8_t r = pr < 0 ? AMV_IGN_UNPARSED : L.absolute ? AMV_IGN_ABSOLUTE : AMV_IGN_NOT_MOUSE;
                if (ignore_reason_rank(r) > ignore_reason_rank(reason)) reason = r;
                ESP_LOGI(TAG, "  if %u: not used (%s)", hi[k].ifnum,
                         r == AMV_IGN_UNPARSED ? "unparseable, no boot protocol" : r == AMV_IGN_ABSOLUTE ? "absolute pointer" : "no mouse collection with X/Y");
                continue;
            }
        }
        mouse_if_t *m = NULL;
        for (int i = 0; i < MAX_IF; i++) if (!s_if[i].used) { m = &s_if[i]; break; }
        if (!m) { reason = AMV_IGN_NO_SLOT; ESP_LOGW(TAG, "  if %u: no free mouse slot", hi[k].ifnum); continue; }

        ESP_LOGI(TAG, "  if %u: mouse (%s): X %u bit @%u, Y %u bit @%u, wheel %s, pan %s, %u buttons%s%s", hi[k].ifnum,
                 tier == AMV_TIER_BOOT ? "boot protocol" : "report protocol",
                 L.x.bit_size, L.x.bit_off, L.y.bit_size, L.y.bit_off, L.wheel.present ? "yes" : "no",
                 L.pan.present ? "yes" : "no", L.num_buttons, L.uses_report_ids ? ", report IDs" : "",
                 L.has_keyboard ? ", also a keyboard" : "");

        if (usb_host_interface_claim(s_client, dev, hi[k].ifnum, 0) != ESP_OK) {
            ESP_LOGW(TAG, "  if %u: claim failed", hi[k].ifnum);
            reason = AMV_IGN_CLAIM;
            continue;
        }
        /* No idle repeats; report protocol (or boot for tier 3). Devices may STALL either request. */
        ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x0A, 0, hi[k].ifnum, 0, NULL, NULL);
        if (hi[k].subclass == 1) {
            ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x0B,
                      tier == AMV_TIER_BOOT ? 0 : 1, hi[k].ifnum, 0, NULL, NULL);
        }

        TAB_LOCK();
        memset(m, 0, sizeof *m);
        m->layout = L;
        m->tier = tier;
        m->uid = s_next_uid++;
        m->also_keyboard = L.has_keyboard;
        m->dev = dev;
        m->ifnum = hi[k].ifnum;
        m->ep = hi[k].ep;
        m->mps = hi[k].mps ? hi[k].mps : 64;
        m->interval = hi[k].interval;
        m->vid = dd->idVendor;
        m->pid = dd->idProduct;
        m->speed = (uint8_t)info.speed;
        m->nominal_hz = nominal_rate(m->speed, m->interval);
        m->cpi_user = cpi_label;
        m->ctx.battery = 0xFF;
        m->ctx.cfg_iface = -1;
        snprintf(m->name, sizeof m->name, "%s", prod[0] ? prod : "USB mouse");
        m->desc = descs[k];                /* keep: GET_MOUSE_DESC */
        m->desc_len = (uint16_t)lens[k];
        descs[k] = NULL;
        if (drv && !first && tier == AMV_TIER_GENERIC) {
            m->drv = drv;
            m->ctx.dev = dev;
            m->ctx.vid = m->vid;
            m->ctx.pid = m->pid;
            m->ctx.mouse_iface = m->ifnum;
            snprintf(m->ctx.product, sizeof m->ctx.product, "%s", prod);
            if (drv->wants_input) m->in_q = xQueueCreate(8, sizeof(in_report_t));
            m->ctx.in_q = m->in_q;
        }
        m->used = true;
        TAB_UNLOCK();

        int started;
        start_reading(m, &started);
        if (!started) {
            ESP_LOGW(TAG, "  if %u: could not start reading", hi[k].ifnum);
            teardown_if(m);
            reason = AMV_IGN_CLAIM;
            continue;
        }
        if (!first) first = m;
        claimed++;
        ESP_LOGI(TAG, "  if %u: reading \"%s\" with %d transfers in flight; vectoring started (%s)", hi[k].ifnum, m->name,
                 started, tier == AMV_TIER_BOOT ? "tier 3, boot protocol" : drv && m->drv ? "tier 1 candidate" : "tier 2, generic");
    }

    /* Vendor interfaces the driver wants (HID++ on a Logitech receiver, ...): claim and listen */
    if (first && first->drv && first->drv->cfg_match) {
        int ncfg = 0;
        for (int k = 0; k < nhid && ncfg < AMV_DRV_MAX_CFG; k++) {
            if (hi[k].ifnum == first->ifnum || !descs[k] || !hi[k].ep) continue;
            bool is_mouse_if = false;
            for (int i = 0; i < MAX_IF; i++) if (s_if[i].used && s_if[i].dev == dev && s_if[i].ifnum == hi[k].ifnum) is_mouse_if = true;
            if (is_mouse_if || !first->drv->cfg_match(descs[k], (uint16_t)lens[k])) continue;
            if (usb_host_interface_claim(s_client, dev, hi[k].ifnum, 0) != ESP_OK) {
                ESP_LOGW(TAG, "  if %u: claim of the %s configuration interface failed", hi[k].ifnum, first->drv->name);
                continue;
            }
            amv_hid_layout_t CL;
            amv_hid_parse(descs[k], (size_t)lens[k], &CL);
            cfg_if_t *f = &first->cfg[ncfg];
            memset(f, 0, sizeof *f);
            f->dev = dev;
            f->ifnum = hi[k].ifnum;
            f->ep_in = hi[k].ep;
            f->ep_out = hi[k].ep_out;
            f->mps_in = hi[k].mps ? hi[k].mps : 64;
            f->mps_out = hi[k].mps_out;
            f->report_ids = CL.uses_report_ids;
            f->q = xQueueCreate(16, sizeof(in_report_t));
            f->out_sem = xSemaphoreCreateBinary();
            for (int x = 0; x < 2; x++) {
                if (usb_host_transfer_alloc(f->mps_in, 0, &f->xfer[x]) != ESP_OK) break;
                usb_transfer_t *t = f->xfer[x];
                t->device_handle = dev;
                t->bEndpointAddress = f->ep_in;
                t->callback = cfg_in_cb;
                t->context = f;
                t->num_bytes = f->mps_in;
                __atomic_fetch_add(&f->inflight, 1, __ATOMIC_SEQ_CST);
                if (usb_host_transfer_submit(t) != ESP_OK) { __atomic_fetch_sub(&f->inflight, 1, __ATOMIC_SEQ_CST); break; }
            }
            f->used = true;
            ESP_LOGI(TAG, "  if %u: claimed for the %s driver (in 0x%02x%s, %s)", f->ifnum, first->drv->name, f->ep_in,
                     f->ep_out ? ", out endpoint" : ", output via SET_REPORT", f->report_ids ? "report IDs" : "no report IDs");
            TAB_LOCK();
            first->ctx.cfg[ncfg] = (amv_drv_cfg_if_t){ .ifnum = f->ifnum, .report_ids = f->report_ids,
                                                       .has_out_ep = f->ep_out != 0, .h = f };
            first->ctx.cfg[ncfg].n_ids = (uint8_t)amv_hid_report_ids(descs[k], (size_t)lens[k], first->ctx.cfg[ncfg].ids,
                                                                     (int)sizeof first->ctx.cfg[ncfg].ids);
            first->ctx.n_cfg = (uint8_t)++ncfg;
            TAB_UNLOCK();
        }
    }

    /* Tier 1: the mouse already moves the cursor; now ask it about itself over its own protocol */
    if (first && first->drv) {
        ESP_LOGI(TAG, "  %s driver: probing (read-only)", first->drv->name);
        uint8_t *dp[AMV_DRV_MAX_HID_IFS];
        int dl[AMV_DRV_MAX_HID_IFS];
        for (int k = 0; k < nhid; k++) { dp[k] = descs[k]; dl[k] = lens[k]; }
        for (int k = 0; k < nhid; k++) if (!dp[k] && hi[k].ifnum == first->ifnum) { dp[k] = first->desc; dl[k] = first->desc_len; }
        probe_driver(first, hi, dp, dl, nhid);
        ESP_LOGI(TAG, "  tier %u: %u CPI, %u Hz, battery %u%%", first->tier, first->ctx.cpi, first->ctx.device_hz,
                 first->ctx.battery == 0xFF ? 0 : first->ctx.battery);
    }
    for (int k = 0; k < nhid; k++) free(descs[k]);

    if (claimed == 0) {
        ESP_LOGI(TAG, "device %04x:%04x has no usable mouse interface; listed as ignored", dd->idVendor, dd->idProduct);
        ignored_t *g = NULL;
        TAB_LOCK();
        for (int i = 0; i < MAX_IGNORED; i++) if (!s_ign[i].used) { g = &s_ign[i]; break; }
        if (g) {
            *g = (ignored_t){ .used = true, .dev = dev, .uid = s_next_uid++, .vid = dd->idVendor, .pid = dd->idProduct,
                              .speed = (uint8_t)info.speed, .iface = nhid ? hi[0].ifnum : 0, .reason = reason };
            snprintf(g->name, sizeof g->name, "%s", prod[0] ? prod : "USB device");
        }
        TAB_UNLOCK();
        if (g) emit(AMV_MEV_ATTACHED, g->uid);    /* stays open so its removal is seen */
        else usb_host_device_close(s_client, dev);
    } else {
        emit(AMV_MEV_ATTACHED, first->uid);
    }
    update_mouse_count();
}

static void handle_gone(usb_device_handle_t dev)
{
    bool any = false;
    uint16_t uid = 0;
    for (int i = 0; i < MAX_IF; i++) {
        if (s_if[i].used && s_if[i].dev == dev) {
            ESP_LOGI(TAG, "\"%s\" (if %u) unplugged after %u reports", s_if[i].name, s_if[i].ifnum, (unsigned)s_if[i].reports);
            uid = s_if[i].uid;
            teardown_if(&s_if[i]);
            any = true;
        }
    }
    TAB_LOCK();
    for (int i = 0; i < MAX_IGNORED; i++) {
        if (s_ign[i].used && s_ign[i].dev == dev) {
            uid = s_ign[i].uid;
            s_ign[i].used = false;
            any = true;
        }
    }
    TAB_UNLOCK();
    if (any) {
        usb_host_device_close(s_client, dev);
        emit(AMV_MEV_REMOVED, uid);
    }
    update_mouse_count();
}

/* Tier 1: re-read what can change (CPI through the mouse's DPI button), and announce changes */
static void refresh_known(void)
{
    for (int i = 0; i < MAX_IF; i++) {
        mouse_if_t *m = &s_if[i];
        /* A receiver driver that found no mouse awake (Logitech: asleep, out of range): try again;
         * the mouse answers once it has been moved. The descriptors are gone, so only drivers that
         * work purely over claimed configuration interfaces can be probed again. */
        if (m->used && !m->dying && m->tier != AMV_TIER_KNOWN && m->drv && m->drv->cfg_match && m->ctx.n_cfg) {
            xSemaphoreTake(s_drv, portMAX_DELAY);
            amv_drv_ctx_t c = m->ctx;
            bool live = m->used && !m->dying;
            esp_err_t e = live ? m->drv->probe(&c) : ESP_FAIL;
            xSemaphoreGive(s_drv);
            if (live) {
                bool changed = e == ESP_OK || c.link != m->ctx.link;
                TAB_LOCK();
                m->ctx = c;
                if (e == ESP_OK) m->tier = AMV_TIER_KNOWN;
                TAB_UNLOCK();
                if (changed) emit(AMV_MEV_UPDATED, m->uid);
            }
            continue;
        }
        if (!m->used || m->dying || m->tier != AMV_TIER_KNOWN || !m->drv || !m->drv->refresh) continue;
        xSemaphoreTake(s_drv, portMAX_DELAY);
        amv_drv_ctx_t c = m->ctx;
        bool live = m->used && !m->dying;
        if (live) m->drv->refresh(&c);
        xSemaphoreGive(s_drv);
        if (!live) continue;
        bool changed = c.cpi != m->ctx.cpi || c.device_hz != m->ctx.device_hz || c.battery != m->ctx.battery ||
                       c.error != m->ctx.error || c.lod != m->ctx.lod || c.motion_sync != m->ctx.motion_sync ||
                       c.charging != m->ctx.charging || c.link != m->ctx.link;
        TAB_LOCK();
        m->ctx = c;
        TAB_UNLOCK();
        if (changed) emit(AMV_MEV_UPDATED, m->uid);
    }
}

static void setup_task(void *arg)
{
    (void)arg;
    msg_t m;
    TickType_t next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(REFRESH_MS);
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (m.type == MSG_NEW) handle_new(m.addr);
            else handle_gone(m.dev);
        }
        if ((int32_t)(xTaskGetTickCount() - next_refresh) >= 0) {
            refresh_known();
            next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(REFRESH_MS);
        }
    }
}

/* ---------- library plumbing ---------- */

static void client_event_cb(const usb_host_client_event_msg_t *ev, void *arg)
{
    (void)arg;
    msg_t m = { 0 };
    if (ev->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        m.type = MSG_NEW;
        m.addr = ev->new_dev.address;
    } else {
        m.type = MSG_GONE;
        m.dev = ev->dev_gone.dev_hdl;
        for (int i = 0; i < MAX_IF; i++) if (s_if[i].used && s_if[i].dev == m.dev) s_if[i].dying = true;
    }
    if (xQueueSend(s_q, &m, 0) != pdTRUE) ESP_EARLY_LOGW(TAG, "host event queue full");
}

static void lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) ESP_LOGI(TAG, "all devices freed");
    }
}

static void client_task(void *arg)
{
    (void)arg;
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

void amv_usb_host_port_status(char *out, size_t cap)
{
    /* Read-only peek at the High-Speed controller's host port register: what the pins look like */
    usb_dwc_hprt_reg_t h = { .val = USB_DWC_HS.hprt_reg.val };
    snprintf(out, cap, "HPRT=%08x conn=%u en=%u pwr=%u linestate(D-,D+)=%u%u speed=%u ovrcur=%u rst=%u",
             (unsigned)h.val, h.prtconnsts, h.prtena, h.prtpwr, (h.prtlnsts >> 1) & 1, h.prtlnsts & 1, h.prtspd,
             h.prtovrcurract, h.prtrst);
}

/* Bring-up aid: log every change of the host port's line state / connect bits, 20 Hz */
static void diag_task(void *arg)
{
    (void)arg;
    uint32_t last = 0xFFFFFFFF, ticks = 0;
    char buf[128];
    for (;;) {
        uint32_t v = USB_DWC_HS.hprt_reg.val & ~((1u << 1) | (1u << 3) | (1u << 5));   /* ignore the sticky change bits */
        if (v != last) {
            last = v;
            amv_usb_host_port_status(buf, sizeof buf);
            ESP_LOGI(TAG, "port change: %s", buf);
        }
        if (++ticks % 20 == 0) {          /* once a second: reports/s per mouse */
            TAB_LOCK();
            for (int i = 0; i < MAX_IF; i++) {
                mouse_if_t *m = &s_if[i];
                if (!m->used) continue;
                uint32_t r = m->reports;
                m->hist[m->hist_pos] = (uint16_t)(r - m->last_reports > 0xFFFF ? 0xFFFF : r - m->last_reports);
                m->hist_pos = (uint8_t)((m->hist_pos + 1) % RATE_WINDOW);
                m->last_reports = r;
            }
            TAB_UNLOCK();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- mouse information API (protocol commands) ---------- */

void amv_mouse_set_event_cb(amv_mouse_event_cb_t cb) { s_event_cb = cb; }

/* index -> claimed slot (>= 0) or ignored slot (encoded as -1 - i); count of entries. Caller holds s_tab. */
static int resolve(uint8_t index, int *count)
{
    int n = 0, hit = INT32_MIN;
    for (int i = 0; i < MAX_IF; i++) if (s_if[i].used && !s_if[i].dying) { if (n == index) hit = i; n++; }
    for (int i = 0; i < MAX_IGNORED; i++) if (s_ign[i].used) { if (n == index) hit = -1 - i; n++; }
    *count = n;
    return hit;
}

static void fill_info(int slot, uint8_t index, int count, amv_mouse_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->index = index;
    o->count = (uint8_t)count;
    o->battery = 0xFF;
    if (slot < 0) {
        const ignored_t *g = &s_ign[-1 - slot];
        o->tier = AMV_TIER_IGNORED;
        o->vid = g->vid; o->pid = g->pid; o->speed = g->speed; o->iface = g->iface; o->reason = g->reason; o->uid = g->uid;
        snprintf(o->name, sizeof o->name, "%.29s", g->name);
        return;
    }
    const mouse_if_t *m = &s_if[slot];
    const amv_hid_layout_t *L = &m->layout;
    o->tier = m->tier;
    o->flags = (L->uses_report_ids ? AMV_MF_REPORT_IDS : 0) | (L->wheel.present ? AMV_MF_WHEEL : 0) |
               (L->pan.present ? AMV_MF_PAN : 0) | (m->also_keyboard ? AMV_MF_ALSO_KEYBOARD : 0);
    o->vid = m->vid; o->pid = m->pid; o->speed = m->speed; o->iface = m->ifnum; o->uid = m->uid;
    o->nominal_hz = m->nominal_hz;
    for (int k = 0; k < RATE_WINDOW; k++) if (m->hist[k] > o->measured_hz) o->measured_hz = m->hist[k];
    o->num_buttons = L->num_buttons;
    o->x_bits = L->x.bit_size;
    snprintf(o->name, sizeof o->name, "%.29s", m->tier == AMV_TIER_KNOWN && m->ctx.name[0] ? m->ctx.name : m->name);
    if (m->drv) {
        o->driver = m->drv->id;
        if (m->ctx.error) o->flags |= AMV_MF_DRV_ERROR;
    }
    if (m->tier == AMV_TIER_KNOWN) {
        if (m->ctx.verified) o->flags |= AMV_MF_VERIFIED;
        if (m->ctx.can_set_cpi) o->flags |= AMV_MF_CAN_SET_CPI;
        if (m->ctx.can_set_rate) o->flags |= AMV_MF_CAN_SET_RATE;
        o->device_hz = m->ctx.device_hz;
        o->battery = m->ctx.battery;
        if (m->ctx.cpi) { o->cpi = m->ctx.cpi; o->cpi_source = AMV_CPI_DEVICE; }
    }
    if (!o->cpi && m->cpi_user) { o->cpi = m->cpi_user; o->cpi_source = AMV_CPI_USER; }
}

uint8_t amv_mouse_get_info(uint8_t index, amv_mouse_info_t *out)
{
    int count;
    TAB_LOCK();
    int slot = resolve(index, &count);
    if (slot != INT32_MIN) fill_info(slot, index, count, out);
    TAB_UNLOCK();
    if (slot == INT32_MIN) {
        memset(out, 0, sizeof *out);
        out->index = index;
        out->count = (uint8_t)count;
        return AMV_ST_NO_MOUSE;
    }
    return AMV_ST_OK;
}

uint8_t amv_mouse_get_caps(uint8_t index, amv_mouse_caps_t *out)
{
    int count;
    uint8_t st = AMV_ST_OK;
    memset(out, 0, sizeof *out);
    out->index = index;
    TAB_LOCK();
    int slot = resolve(index, &count);
    if (slot == INT32_MIN) st = AMV_ST_NO_MOUSE;
    else if (slot < 0 || s_if[slot].tier != AMV_TIER_KNOWN) st = AMV_ST_UNSUPPORTED;
    else {
        const amv_drv_ctx_t *c = &s_if[slot].ctx;
        out->driver = s_if[slot].drv->id;
        out->n_rates = c->n_rates;
        memcpy(out->rates, c->rates, sizeof out->rates);
        out->cpi_min = c->cpi_min; out->cpi_max = c->cpi_max; out->cpi_step = c->cpi_step;
        snprintf(out->firmware, sizeof out->firmware, "%s", c->firmware);
    }
    TAB_UNLOCK();
    return st;
}

uint8_t amv_mouse_get_desc(uint8_t index, uint16_t offset, amv_mouse_desc_rep_t *out)
{
    int count;
    uint8_t st = AMV_ST_OK;
    memset(out, 0, sizeof *out);
    out->index = index;
    out->offset = offset;
    TAB_LOCK();
    int slot = resolve(index, &count);
    if (slot == INT32_MIN) st = AMV_ST_NO_MOUSE;
    else if (slot < 0 || !s_if[slot].desc) st = AMV_ST_UNSUPPORTED;
    else {
        const mouse_if_t *m = &s_if[slot];
        out->total = m->desc_len;
        if (offset > m->desc_len) st = AMV_ST_BAD_VALUE;
        else {
            int n = m->desc_len - offset;
            if (n > (int)sizeof out->data) n = sizeof out->data;
            out->len = (uint8_t)n;
            memcpy(out->data, m->desc + offset, n);
        }
    }
    TAB_UNLOCK();
    return st;
}

/* Runs a driver write on a copy of the context; the device cannot be torn down meanwhile. */
static uint8_t driver_write(uint8_t index, bool is_cpi, uint16_t value, amv_mouse_info_t *out)
{
    int count;
    xSemaphoreTake(s_drv, portMAX_DELAY);
    TAB_LOCK();
    int slot = resolve(index, &count);
    mouse_if_t *m = slot >= 0 && slot != INT32_MIN ? &s_if[slot] : NULL;
    amv_drv_ctx_t c;
    if (m) c = m->ctx;
    TAB_UNLOCK();

    uint8_t st;
    if (slot == INT32_MIN) st = AMV_ST_NO_MOUSE;
    else if (!m) st = AMV_ST_UNSUPPORTED;
    else if (m->tier != AMV_TIER_KNOWN) {
        if (!is_cpi) st = AMV_ST_UNSUPPORTED;
        else if (value != 0 && (value < 50 || value > 64000)) st = AMV_ST_BAD_VALUE;
        else {
            /* Tiers 2/3: no protocol to reach the sensor; remember the user's number as a label */
            st = amv_store_set_mouse_cpi(m->vid, m->pid, value) == ESP_OK ? AMV_ST_OK : AMV_ST_STORE_ERR;
            if (st == AMV_ST_OK) {
                TAB_LOCK();
                for (int i = 0; i < MAX_IF; i++) if (s_if[i].used && s_if[i].vid == m->vid && s_if[i].pid == m->pid) s_if[i].cpi_user = value;
                TAB_UNLOCK();
            }
        }
    } else {
        esp_err_t e = is_cpi ? m->drv->set_cpi(&c, value) : m->drv->set_rate(&c, value);
        st = e == ESP_OK ? AMV_ST_OK : e == ESP_ERR_NOT_SUPPORTED ? AMV_ST_UNSUPPORTED :
             e == ESP_ERR_INVALID_ARG ? AMV_ST_BAD_VALUE : AMV_ST_DEVICE_ERR;
        ESP_LOGI(TAG, "%s: set %s %u -> %s (now %u CPI, %u Hz)", m->name, is_cpi ? "CPI" : "polling", value,
                 st == AMV_ST_OK ? "ok" : "refused", c.cpi, c.device_hz);
        TAB_LOCK();
        if (m->used) m->ctx = c;
        TAB_UNLOCK();
    }
    xSemaphoreGive(s_drv);

    amv_mouse_get_info(index, out);
    if (m && st == AMV_ST_OK) emit(AMV_MEV_UPDATED, out->uid);
    return st;
}

uint8_t amv_mouse_set_cpi(uint8_t index, uint16_t cpi, amv_mouse_info_t *out) { return driver_write(index, true, cpi, out); }
uint8_t amv_mouse_set_rate(uint8_t index, uint16_t hz, amv_mouse_info_t *out) { return driver_write(index, false, hz, out); }

/* ---------- catalog names, settings (firmware/esp32p4-dongle/README.md) ---------- */

uint8_t amv_mouse_get_model(uint8_t index, amv_mouse_model_t *out)
{
    int count;
    memset(out, 0, sizeof *out);
    out->index = index;
    TAB_LOCK();
    int slot = resolve(index, &count);
    uint16_t vid = 0, pid = 0;
    bool driver = false;
    if (slot != INT32_MIN) {
        if (slot < 0) { vid = s_ign[-1 - slot].vid; pid = s_ign[-1 - slot].pid; }
        else { vid = s_if[slot].vid; pid = s_if[slot].pid; driver = s_if[slot].drv != NULL; }
    }
    TAB_UNLOCK();
    if (slot == INT32_MIN) return AMV_ST_NO_MOUSE;
    const amv_catalog_entry_t *e = amv_catalog_find(vid, pid);
    out->state = driver ? AMV_MODEL_DRIVER : e ? AMV_MODEL_KNOWN : AMV_MODEL_UNKNOWN;
    if (e) {
        snprintf(out->brand, sizeof out->brand, "%s", amv_catalog_brand(e));
        snprintf(out->model, sizeof out->model, "%s", e->model);
    }
    return AMV_ST_OK;
}

static void add_setting(amv_settings_rep_t *o, uint8_t id, uint8_t flags, uint16_t value)
{
    if (o->count < AMV_SETTINGS_MAX) o->s[o->count++] = (amv_setting_t){ .id = id, .flags = flags, .value = value };
}

#define BIT_OF(id) (1u << (id))

uint8_t amv_mouse_get_settings(uint8_t index, amv_settings_rep_t *o)
{
    int count;
    memset(o, 0, sizeof *o);
    o->index = index;
    TAB_LOCK();
    int slot = resolve(index, &count);
    if (slot != INT32_MIN && slot >= 0) {
        const mouse_if_t *m = &s_if[slot];
        const amv_drv_ctx_t *c = &m->ctx;
        if (m->tier == AMV_TIER_KNOWN) {
            o->link = c->link;
            if (c->cpi) add_setting(o, AMV_SET_CPI, AMV_SETF_READABLE | (c->can_set_cpi ? AMV_SETF_WRITABLE : 0), c->cpi);
            if (c->device_hz) add_setting(o, AMV_SET_POLLING, AMV_SETF_READABLE | (c->can_set_rate ? AMV_SETF_WRITABLE : 0), c->device_hz);
            if (c->readable & BIT_OF(AMV_SET_LOD))
                add_setting(o, AMV_SET_LOD, AMV_SETF_READABLE | ((c->writable & BIT_OF(AMV_SET_LOD)) ? AMV_SETF_WRITABLE : 0), c->lod);
            if (c->readable & BIT_OF(AMV_SET_MOTION_SYNC))
                add_setting(o, AMV_SET_MOTION_SYNC, AMV_SETF_READABLE | ((c->writable & BIT_OF(AMV_SET_MOTION_SYNC)) ? AMV_SETF_WRITABLE : 0), c->motion_sync);
            if (c->battery != 0xFF) add_setting(o, AMV_SET_BATTERY, AMV_SETF_READABLE, c->battery);
            if (c->readable & BIT_OF(AMV_SET_CHARGING)) add_setting(o, AMV_SET_CHARGING, AMV_SETF_READABLE, c->charging);
            add_setting(o, AMV_SET_LINK, AMV_SETF_READABLE, c->link);
        } else {
            /* tiers 2 and 3: the sensor cannot be reached, the CPI is the user's label */
            add_setting(o, AMV_SET_CPI, AMV_SETF_READABLE | AMV_SETF_WRITABLE, m->cpi_user);
        }
    }
    TAB_UNLOCK();
    if (slot == INT32_MIN) { o->count = 0; return AMV_ST_NO_MOUSE; }
    if (slot < 0) return AMV_ST_UNSUPPORTED;
    return AMV_ST_OK;
}

uint8_t amv_mouse_get_options(uint8_t index, uint8_t setting, amv_options_rep_t *o)
{
    int count;
    memset(o, 0, sizeof *o);
    o->index = index;
    o->setting = setting;
    uint8_t st = AMV_ST_OK;
    TAB_LOCK();
    int slot = resolve(index, &count);
    if (slot == INT32_MIN) st = AMV_ST_NO_MOUSE;
    else if (slot < 0) st = AMV_ST_UNSUPPORTED;
    else {
        const mouse_if_t *m = &s_if[slot];
        const amv_drv_ctx_t *c = &m->ctx;
        bool known = m->tier == AMV_TIER_KNOWN;
        if (setting == AMV_SET_CPI) {
            o->kind = AMV_OPT_RANGE; o->unit = AMV_UNIT_CPI; o->n = 3;
            o->values[0] = known && c->cpi_min ? c->cpi_min : 50;
            o->values[1] = known && c->cpi_max ? c->cpi_max : 64000;
            o->values[2] = known && c->cpi_step ? c->cpi_step : 50;
        } else if (known && setting == AMV_SET_POLLING && c->n_rates) {
            o->kind = AMV_OPT_LIST; o->unit = AMV_UNIT_HZ; o->n = c->n_rates > AMV_OPTIONS_MAX ? AMV_OPTIONS_MAX : c->n_rates;
            memcpy(o->values, c->rates, o->n * sizeof o->values[0]);
        } else if (known && setting == AMV_SET_LOD && (c->readable & BIT_OF(AMV_SET_LOD)) && c->n_lod) {
            o->kind = AMV_OPT_LIST; o->unit = c->lod_unit; o->n = c->n_lod;
            memcpy(o->values, c->lod_opts, o->n * sizeof o->values[0]);
        } else if (known && setting == AMV_SET_MOTION_SYNC && (c->readable & BIT_OF(AMV_SET_MOTION_SYNC))) {
            o->kind = AMV_OPT_LIST; o->unit = AMV_UNIT_BOOL; o->n = 2; o->values[0] = 0; o->values[1] = 1;
        } else {
            st = AMV_ST_UNSUPPORTED;
        }
    }
    TAB_UNLOCK();
    return st;
}

/* LOD and Motion Sync writes: same discipline as driver_write (copy the context, device cannot go away). */
static uint8_t driver_set_setting(uint8_t index, uint8_t id, uint16_t value)
{
    int count;
    xSemaphoreTake(s_drv, portMAX_DELAY);
    TAB_LOCK();
    int slot = resolve(index, &count);
    mouse_if_t *m = slot >= 0 && slot != INT32_MIN ? &s_if[slot] : NULL;
    amv_drv_ctx_t c;
    if (m) c = m->ctx;
    TAB_UNLOCK();
    uint8_t st;
    if (slot == INT32_MIN) st = AMV_ST_NO_MOUSE;
    else if (!m || m->tier != AMV_TIER_KNOWN || !m->drv || !m->drv->set_setting || !(c.writable & BIT_OF(id))) st = AMV_ST_UNSUPPORTED;
    else {
        bool listed = false;
        if (id == AMV_SET_LOD) for (int i = 0; i < c.n_lod; i++) listed |= c.lod_opts[i] == value;
        else if (id == AMV_SET_MOTION_SYNC) listed = value <= 1;
        if (!listed) st = AMV_ST_BAD_VALUE;
        else {
            esp_err_t e = m->drv->set_setting(&c, id, value);
            st = e == ESP_OK ? AMV_ST_OK : e == ESP_ERR_NOT_SUPPORTED ? AMV_ST_UNSUPPORTED :
                 e == ESP_ERR_INVALID_ARG ? AMV_ST_BAD_VALUE : AMV_ST_DEVICE_ERR;
            ESP_LOGI(TAG, "%s: set setting %u = %u -> %s", m->name, id, value, st == AMV_ST_OK ? "ok" : "refused");
            TAB_LOCK();
            if (m->used) m->ctx = c;
            TAB_UNLOCK();
        }
    }
    xSemaphoreGive(s_drv);
    if (st == AMV_ST_OK && m) emit(AMV_MEV_UPDATED, m->uid);
    return st;
}

uint8_t amv_mouse_set_setting(uint8_t index, uint8_t id, uint16_t value, amv_settings_rep_t *out)
{
    amv_mouse_info_t info;
    uint8_t st;
    if (id == AMV_SET_CPI) st = amv_mouse_set_cpi(index, value, &info);
    else if (id == AMV_SET_POLLING) st = amv_mouse_set_rate(index, value, &info);
    else if (id == AMV_SET_LOD || id == AMV_SET_MOTION_SYNC) st = driver_set_setting(index, id, value);
    else st = AMV_ST_UNSUPPORTED;
    uint8_t g = amv_mouse_get_settings(index, out);
    return st != AMV_ST_OK ? st : g;
}

int amv_usb_host_device_count(void)
{
    usb_host_lib_info_t info;
    return usb_host_lib_info(&info) == ESP_OK ? info.num_devices : -1;
}

int amv_usb_host_mice(amv_usb_host_mouse_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_IF && n < max; i++) {
        if (!s_if[i].used || s_if[i].dying) continue;
        out[n++] = (amv_usb_host_mouse_t){ .vid = s_if[i].vid, .pid = s_if[i].pid, .speed = s_if[i].speed,
                                           .iface = s_if[i].ifnum, .interval = s_if[i].interval, .mps = s_if[i].mps,
                                           .reports = s_if[i].reports, .errors = s_if[i].errors };
        snprintf(out[n - 1].name, sizeof out[n - 1].name, "%s", s_if[i].name);
    }
    return n;
}

esp_err_t amv_usb_host_start(void)
{
#if AMV_USB_VBUS_EN_GPIO >= 0
    gpio_config_t io = { .pin_bit_mask = 1ULL << AMV_USB_VBUS_EN_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(AMV_USB_VBUS_EN_GPIO, 1);
#endif
    s_q = xQueueCreate(8, sizeof(msg_t));
    s_ctrl_sem = xSemaphoreCreateBinary();
    s_ctrl_mtx = xSemaphoreCreateMutex();
    s_tab = xSemaphoreCreateMutex();
    s_drv = xSemaphoreCreateMutex();
    if (!s_q || !s_ctrl_sem || !s_ctrl_mtx || !s_tab || !s_drv) return ESP_ERR_NO_MEM;

    /* peripheral_map 0 selects the High-Speed controller on the P4; esp_tinyusb keeps the Full-Speed one */
    const usb_host_config_t cfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1, .peripheral_map = 0 };
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) return err;
    const usb_host_client_config_t cc = {
        .is_synchronous = false, .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    err = usb_host_client_register(&cc, &s_client);
    if (err != ESP_OK) return err;

    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_cli", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 0);
    xTaskCreatePinnedToCore(diag_task, "usb_diag", 3072, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(setup_task, "usb_setup", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "USB host up on the High-Speed port; waiting for mice");
    return ESP_OK;
}
