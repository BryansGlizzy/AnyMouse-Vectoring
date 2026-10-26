#include "amv_usb_host.h"
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
#include "amv_ctrl.h"
#include "amv_hid_parse.h"
#include "amv_vector.h"

static const char *TAG = "amv_host";

#define MAX_IF        AMV_VECTOR_MAX_SOURCES
#define XFERS_PER_EP  3
#define MAX_REPORT_DESC 1024
#define HID_CLASS     3

typedef struct {
    bool used;
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

typedef enum { MSG_NEW, MSG_GONE } msg_type_t;
typedef struct { msg_type_t type; uint8_t addr; usb_device_handle_t dev; } msg_t;

static usb_host_client_handle_t s_client;
static QueueHandle_t s_q;
static mouse_if_t s_if[MAX_IF];
static SemaphoreHandle_t s_ctrl_sem;

/* ---------- helpers ---------- */

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
static esp_err_t ctrl_xfer(usb_device_handle_t dev, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
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

/* ---------- interrupt IN handling ---------- */

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

typedef struct { uint8_t ifnum, subclass, protocol, ep, interval; uint16_t mps, report_len; } hid_if_t;

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
            }
        }
    }
    FLUSH();
    #undef FLUSH
    return n;
}

static void teardown_if(mouse_if_t *m)
{
    for (int w = 0; w < 100 && m->inflight > 0; w++) vTaskDelay(pdMS_TO_TICKS(5));
    if (m->inflight > 0) ESP_LOGW(TAG, "if %d: %d transfers still in flight at teardown", m->ifnum, m->inflight);
    for (int k = 0; k < XFERS_PER_EP; k++) {
        if (m->xfer[k] && m->inflight == 0) usb_host_transfer_free(m->xfer[k]);
        m->xfer[k] = NULL;
    }
    usb_host_interface_release(s_client, m->dev, m->ifnum);
    amv_vector_release((unsigned)(m - s_if));
    m->used = false;
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

    hid_if_t hi[8];
    int nhid = collect_hid_interfaces(cfg, hi, 8);
    int claimed = 0;
    uint8_t *rd = malloc(MAX_REPORT_DESC);
    if (!rd) { usb_host_device_close(s_client, dev); return; }

    for (int k = 0; k < nhid; k++) {
        uint16_t rlen = hi[k].report_len > MAX_REPORT_DESC ? MAX_REPORT_DESC : hi[k].report_len;
        int got = 0;
        if (ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | 1, 0x06, 0x2200,
                      hi[k].ifnum, rlen, rd, &got) != ESP_OK || got <= 0) {
            ESP_LOGW(TAG, "  if %u: could not read the report descriptor", hi[k].ifnum);
            continue;
        }
        ESP_LOGI(TAG, "  if %u: HID subclass %u protocol %u, ep 0x%02x mps %u interval %u, report descriptor %d bytes:",
                 hi[k].ifnum, hi[k].subclass, hi[k].protocol, hi[k].ep, hi[k].mps, hi[k].interval, got);
        ESP_LOG_BUFFER_HEX(TAG, rd, got);

        mouse_if_t *m = NULL;
        for (int i = 0; i < MAX_IF; i++) if (!s_if[i].used) { m = &s_if[i]; break; }
        if (!m) { ESP_LOGW(TAG, "  if %u: no free mouse slot", hi[k].ifnum); continue; }

        memset(m, 0, sizeof *m);
        int pr = amv_hid_parse(rd, (size_t)got, &m->layout);
        if (pr < 0) { ESP_LOGW(TAG, "  if %u: malformed report descriptor (%d), ignored", hi[k].ifnum, pr); continue; }
        if (!m->layout.valid) {
            ESP_LOGI(TAG, "  if %u: not a relative mouse (%s), ignored", hi[k].ifnum,
                     m->layout.absolute ? "absolute pointer" : "no mouse collection with X/Y");
            continue;
        }
        const amv_hid_layout_t *L = &m->layout;
        ESP_LOGI(TAG, "  if %u: mouse: X %u bit @%u, Y %u bit @%u, wheel %s, pan %s, %u buttons%s%s", hi[k].ifnum,
                 L->x.bit_size, L->x.bit_off, L->y.bit_size, L->y.bit_off, L->wheel.present ? "yes" : "no",
                 L->pan.present ? "yes" : "no", L->num_buttons, L->uses_report_ids ? ", report IDs" : "",
                 L->has_keyboard ? ", also a keyboard" : "");

        if (usb_host_interface_claim(s_client, dev, hi[k].ifnum, 0) != ESP_OK) {
            ESP_LOGW(TAG, "  if %u: claim failed", hi[k].ifnum);
            continue;
        }
        /* Report protocol and no idle repeats; devices may STALL either request */
        ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x0A, 0, hi[k].ifnum, 0, NULL, NULL);
        if (hi[k].subclass == 1) {
            ctrl_xfer(dev, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | 1, 0x0B, 1, hi[k].ifnum, 0, NULL, NULL);
        }

        m->used = true;
        m->dev = dev;
        m->ifnum = hi[k].ifnum;
        m->ep = hi[k].ep;
        m->mps = hi[k].mps ? hi[k].mps : 64;
        m->interval = hi[k].interval;
        m->vid = dd->idVendor;
        m->pid = dd->idProduct;
        m->speed = (uint8_t)info.speed;
        snprintf(m->name, sizeof m->name, "%s", prod[0] ? prod : "USB mouse");

        int started = 0;
        for (int x = 0; x < XFERS_PER_EP; x++) {
            if (usb_host_transfer_alloc(m->mps, 0, &m->xfer[x]) != ESP_OK) break;
            usb_transfer_t *t = m->xfer[x];
            t->device_handle = dev;
            t->bEndpointAddress = m->ep;
            t->callback = in_cb;
            t->context = m;
            t->num_bytes = m->mps;
            __atomic_fetch_add(&m->inflight, 1, __ATOMIC_SEQ_CST);
            if (usb_host_transfer_submit(t) != ESP_OK) {
                __atomic_fetch_sub(&m->inflight, 1, __ATOMIC_SEQ_CST);
                break;
            }
            started++;
        }
        if (!started) {
            ESP_LOGW(TAG, "  if %u: could not start reading", hi[k].ifnum);
            teardown_if(m);
            continue;
        }
        claimed++;
        ESP_LOGI(TAG, "  if %u: reading \"%s\" with %d transfers in flight; vectoring started", hi[k].ifnum, m->name, started);
    }
    free(rd);
    if (claimed == 0) {
        ESP_LOGI(TAG, "device %04x:%04x has no mouse interface; ignored", dd->idVendor, dd->idProduct);
        usb_host_device_close(s_client, dev);
    }
    update_mouse_count();
}

static void handle_gone(usb_device_handle_t dev)
{
    bool any = false;
    for (int i = 0; i < MAX_IF; i++) {
        if (s_if[i].used && s_if[i].dev == dev) {
            ESP_LOGI(TAG, "\"%s\" (if %u) unplugged after %u reports", s_if[i].name, s_if[i].ifnum, (unsigned)s_if[i].reports);
            teardown_if(&s_if[i]);
            any = true;
        }
    }
    if (any) usb_host_device_close(s_client, dev);
    update_mouse_count();
}

static void setup_task(void *arg)
{
    (void)arg;
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.type == MSG_NEW) handle_new(m.addr);
        else handle_gone(m.dev);
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
    uint32_t last = 0xFFFFFFFF;
    char buf[128];
    for (;;) {
        uint32_t v = USB_DWC_HS.hprt_reg.val & ~((1u << 1) | (1u << 3) | (1u << 5));   /* ignore the sticky change bits */
        if (v != last) {
            last = v;
            amv_usb_host_port_status(buf, sizeof buf);
            ESP_LOGI(TAG, "port change: %s", buf);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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
    if (!s_q || !s_ctrl_sem) return ESP_ERR_NO_MEM;

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
