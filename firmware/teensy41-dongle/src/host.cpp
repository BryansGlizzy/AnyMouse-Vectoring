#include "host.h"
#include <Arduino.h>
#include <USBHost_t36.h>
#include <string.h>
#include "amv_octant.h"
#include "pc_out.h"

host_if_t host_ifs[HOST_MAX_IF];

static USBHost usbhost;
static USBHub hub1(usbhost), hub2(usbhost);

static float g_w[8] = {1, 1, 1, 1, 1, 1, 1, 1};
static bool g_bypass;
static amv_octant_state_t g_rem[PC_OUT_MAX_SOURCES];
static uint32_t g_dev_serial;
static host_stats_t hs;
static uint32_t last_rx_cyc;
static uint32_t hist_edge_cyc[7];
static const uint16_t HIST_EDGE_US[7] = {60, 100, 150, 250, 500, 1000, 2000};

static inline uint32_t cyc() { return ARM_DWT_CYCCNT; }
static inline uint32_t irq_save() { uint32_t p; __asm__ volatile("mrs %0, primask" : "=r"(p)); __disable_irq(); return p; }
static inline void irq_restore(uint32_t p) { if (!p) __enable_irq(); }

static uint16_t nominal_rate(uint8_t speed, uint8_t interval) {
    if (!interval) return 0;
    if (speed == 2) return interval <= 16 ? (uint16_t)(8000u >> (interval - 1)) : 0;   // micro-frames, 2^(n-1)
    return (uint16_t)(1000u / interval);                                            // frames
}

static void copy_string(Device_t *dev, int which, char *out, size_t cap) {
    out[0] = 0;
    if (!dev->strbuf || !dev->strbuf->iStrings[which]) return;
    const char *s = (const char *)&dev->strbuf->buffer[dev->strbuf->iStrings[which]];
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '?';
    out[i] = 0;
    while (i && out[i - 1] == ' ') out[--i] = 0;     // some mice pad the string with spaces
}

// ---------------------------------------------------------------------------------------------------------------
// One parser per HID interface. It remembers which interface record it feeds and owns the control-transfer
// completion for the transport.
class AmvParser : public USBHIDParser {
public:
    AmvParser(USBHost &h) : USBHIDParser(h) {}
    volatile int slot = -1;
    volatile bool ctl_busy = false, ctl_done = false;
    volatile bool out_busy = false;
    void out_finished() { out_busy = false; }
protected:
    bool claim(Device_t *dev, int type, const uint8_t *d, uint32_t n) override {
        if (!USBHIDParser::claim(dev, type, d, n)) return false;
        int s = -1;
        for (int i = 0; i < HOST_MAX_IF; i++) if (!host_ifs[i].used) { s = i; break; }
        slot = s;
        if (s < 0) return true;           // no record left: the interface works but is invisible to us
        host_if_t *h = &host_ifs[s];
        memset((void *)h, 0, sizeof *h);
        h->dev = dev;
        h->dev_serial = ++g_dev_serial;
        h->vid = dev->idVendor;
        h->pid = dev->idProduct;
        h->speed = dev->speed;
        h->ifnum = d[2];
        h->subclass = d[6];
        h->protocol = d[7];
        h->src = -1;
        for (uint32_t i = 0; i + 2 <= n && d[i] >= 2 && i + d[i] <= n; i += d[i]) {
            if (i && d[i + 1] == 4) break;    // the next interface
            if (d[i + 1] == 5 && d[i] >= 7) {
                uint16_t mps = (uint16_t)(d[i + 4] | (d[i + 5] << 8)) & 0x7FF;
                if (d[i + 2] & 0x80) { h->in_mps = mps; h->nominal_hz = nominal_rate(h->speed, d[i + 6]); }
                else { h->out_mps = mps; h->has_out = true; }
            }
        }
        copy_string(dev, strbuf_t::STR_ID_PROD, h->product, sizeof h->product);
        copy_string(dev, strbuf_t::STR_ID_MAN, h->manufacturer, sizeof h->manufacturer);
        h->claim_ms = millis();
        h->used = true;
        return true;
    }
    void control(const Transfer_t *t) override {
        if (ctl_busy) { ctl_busy = false; ctl_done = true; return; }
        USBHIDParser::control(t);
    }
    void disconnect() override {
        int s = slot;
        slot = -1;
        if (s >= 0) host_ifs[s].gone = true;
        ctl_busy = false;
        out_busy = false;
        USBHIDParser::disconnect();
    }
};

#define N_PARSERS 8
static AmvParser p0(usbhost), p1(usbhost), p2(usbhost), p3(usbhost), p4(usbhost), p5(usbhost), p6(usbhost), p7(usbhost);
static AmvParser *const parsers[N_PARSERS] = {&p0, &p1, &p2, &p3, &p4, &p5, &p6, &p7};
// Receive buffers big enough for any interrupt endpoint, four per interface so an 8 kHz stream survives a late
// interrupt (USBHost_t36's default is two, carved from the descriptor buffer).
DMAMEM static uint8_t rxbuf[N_PARSERS][4][1024] __attribute__((aligned(32)));

static AmvParser *parser_of(host_if_t *h) {
    int s = (int)(h - host_ifs);
    for (auto *p : parsers) if (p->slot == s) return p;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The driver that claims every collection: raw reports in, motion out.
class AllHid : public USBHIDInput {
public:
    hidclaim_t claim_collection(USBHIDParser *driver, Device_t *dev, uint32_t topusage) override {
        AmvParser *p = static_cast<AmvParser *>(driver);
        int s = p->slot;
        if (s < 0) return CLAIM_NO;
        host_if_t *h = &host_ifs[s];
        if (h->desc_len == 0) setup_interface(h, driver);
        return CLAIM_REPORT;
    }

    bool hid_process_in_data(const Transfer_t *t) override {
        uint32_t now = cyc();
        AmvParser *p = static_cast<AmvParser *>(static_cast<USBHIDParser *>(t->driver));
        int s = p->slot;
        if (s < 0) return true;
        host_if_t *h = &host_ifs[s];
        uint32_t len = t->length - ((t->qtd.token >> 16) & 0x7FFF);     // bytes actually received
        if (len > t->length) len = 0;
        const uint8_t *data = (const uint8_t *)t->buffer;
        if ((h->role == HOST_ROLE_MOUSE || h->role == HOST_ROLE_BOOT) && h->src >= 0) {
            amv_hid_report_t r;
            if (amv_hid_decode(&h->layout, data, len, &r)) {
                timing(now);
                h->motion_reports++;
                int32_t sx = r.dx, sy = r.dy;
                if ((r.dx || r.dy) && !g_bypass) {
                    uint8_t oct;
                    uint32_t t0 = cyc();
                    amv_octant_scale(g_w, &g_rem[h->src], r.dx, r.dy, &sx, &sy, &oct);
                    uint32_t c = cyc() - t0;
                    hs.scale_n++; hs.scale_sum += c; if (c > hs.scale_max) hs.scale_max = c;
                }
                pc_out_feed((unsigned)h->src, r.dx, r.dy, sx, sy, r.wheel, r.pan, (r.have & AMV_HID_HAVE_BUTTONS) != 0,
                            (uint16_t)r.buttons);
                uint32_t c = cyc() - now;
                hs.path_n++; hs.path_sum += c; if (c > hs.path_max) hs.path_max = c;
                return true;
            }
        }
        h->other_reports++;
        if (h->listen && len) {
            if (h->r_count == HOST_RING) { h->r_drops++; return true; }
            unsigned i = (h->r_head + h->r_count) % HOST_RING;
            if (len > HOST_REPORT_MAX) len = HOST_REPORT_MAX;
            memcpy(h->r_data[i], data, len);
            h->r_len[i] = (uint8_t)len;
            h->r_count++;
        }
        return true;
    }

    bool hid_process_out_data(const Transfer_t *t) override {
        static_cast<AmvParser *>(static_cast<USBHIDParser *>(t->driver))->out_finished();
        return true;
    }

private:
    static void timing(uint32_t now) {
        if (hs.rx) {
            uint32_t dt = now - last_rx_cyc;
            if (hs.rx == 1 || dt < hs.dt_min) hs.dt_min = dt;
            if (dt > hs.dt_max) hs.dt_max = dt;
            hs.dt_sum += dt;
            unsigned b = 0;
            while (b < 7 && dt >= hist_edge_cyc[b]) b++;
            hs.hist[b]++;
        }
        last_rx_cyc = now;
        hs.rx++;
    }

    static void setup_interface(host_if_t *h, USBHIDParser *driver) {
        uint16_t n = driver->getHIDReportDescriptorSize();
        if (n > sizeof h->desc) n = sizeof h->desc;
        memcpy(h->desc, driver->getHIDReportDescriptor(), n);
        h->desc_len = n;
        amv_hid_parse(h->desc, n, &h->layout);
        if (h->layout.valid) h->role = HOST_ROLE_MOUSE;
        else if (h->layout.has_mouse && h->layout.absolute) h->role = HOST_ROLE_ABSOLUTE;
        else if (h->subclass == 1 && h->protocol == 2) {
            // An unusable descriptor on a boot mouse: read it in boot protocol (the loop sends SET_PROTOCOL).
            amv_hid_parse(amv_hid_boot_mouse_desc, amv_hid_boot_mouse_desc_len, &h->layout);
            h->role = HOST_ROLE_BOOT;
        } else h->role = HOST_ROLE_OTHER;
        if (h->role == HOST_ROLE_MOUSE || h->role == HOST_ROLE_BOOT) {
            bool taken[PC_OUT_MAX_SOURCES] = {};
            for (auto &o : host_ifs) if (o.used && &o != h && o.src >= 0) taken[o.src] = true;
            for (int i = 0; i < PC_OUT_MAX_SOURCES; i++) if (!taken[i]) { h->src = (int8_t)i; break; }
            if (h->src >= 0) g_rem[h->src] = {};
        }
        h->claim_ms = millis();
    }
};
static AllHid allhid;

// Count interrupts and how many reports each delivered (the 8 kHz checks).
static void (*orig_isr)(void);
static uint32_t isr_last;
static void isr_wrap() {
    uint32_t now = cyc();
    uint32_t before = hs.rx;
    orig_isr();
    if (isr_last) {
        uint32_t dt = now - isr_last;
        if (hs.isr_n == 0 || dt < hs.isr_min) hs.isr_min = dt;
        if (dt > hs.isr_max) hs.isr_max = dt;
        hs.isr_sum += dt;
    }
    isr_last = now;
    hs.isr_n++;
    hs.isr_busy += cyc() - now;
    uint32_t got = hs.rx - before;
    hs.isr_per[got > 3 ? 3 : got]++;
}

void host_begin(void) {
    for (int i = 0; i < 7; i++) hist_edge_cyc[i] = HIST_EDGE_US[i] * (F_CPU_ACTUAL / 1000000u);
    for (int i = 0; i < N_PARSERS; i++)
        parsers[i]->setRXBuffers(rxbuf[i][0], rxbuf[i][1], 255, rxbuf[i][2], rxbuf[i][3]);
    USBHIDParser::driver_ready_for_hid_collection(&allhid);
    usbhost.begin();
    orig_isr = _VectorsRam[IRQ_USBHS + 16];
    attachInterruptVector(IRQ_USBHS, isr_wrap);
    // USBHost_t36 leaves the interrupt threshold at 1 micro-frame, which batches two 8 kHz reports into one
    // interrupt about half the time. Interrupt at once: one report, one interrupt, 125 us apart.
    host_set_itc(0);
}

void host_task(void) { usbhost.Task(); }

void host_set_itc(unsigned v) { USBHS_USBCMD = (USBHS_USBCMD & ~(0xFFu << 16)) | ((uint32_t)(v & 0xFF) << 16); }
unsigned host_get_itc(void) { return (USBHS_USBCMD >> 16) & 0xFF; }

bool host_if_alive(const host_if_t *h) { return h->used && !h->gone; }

void host_set_weights(const float w[8], bool bypass) {
    uint32_t p = irq_save();
    bool changed = memcmp(g_w, w, sizeof g_w) != 0 || g_bypass != bypass;
    memcpy(g_w, w, sizeof g_w);
    g_bypass = bypass;
    if (changed) memset(g_rem, 0, sizeof g_rem);
    irq_restore(p);
}

void host_take_stats(host_stats_t *out) {
    uint32_t p = irq_save();
    *out = hs;
    memset(&hs, 0, sizeof hs);
    irq_restore(p);
}

// ---------------------------------------------------------------------------------------------------------------
// transport

static uint8_t ctl_buf[1100];            // DTCM: no cache maintenance needed for the DMA

static int control(host_if_t *h, uint8_t bm, uint8_t req, uint16_t value, uint16_t index, uint16_t len, uint32_t timeout_ms) {
    AmvParser *p = parser_of(h);
    if (!p || !host_if_alive(h)) return -3;
    p->ctl_done = false;
    p->ctl_busy = true;
    if (!p->sendControlPacket(bm, req, value, index, len, ctl_buf)) { p->ctl_busy = false; return -1; }
    uint32_t start = millis();
    while (!p->ctl_done) {
        if (!host_if_alive(h)) { p->ctl_busy = false; return -3; }
        if (millis() - start > timeout_ms) { p->ctl_busy = false; return -2; }   // e.g. a STALL: USBHost_t36 never reports it
        usbhost.Task();
    }
    return 0;
}

int host_set_report(host_if_t *h, uint8_t type, uint8_t id, const uint8_t *data, uint16_t len) {
    uint16_t off = id ? 1 : 0;
    if (len + off > sizeof ctl_buf) return -4;
    ctl_buf[0] = id;
    memcpy(ctl_buf + off, data, len);
    return control(h, 0x21, 0x09, (uint16_t)((type << 8) | id), h->ifnum, (uint16_t)(len + off), 1000);
}

int host_get_report(host_if_t *h, uint8_t type, uint8_t id, uint8_t *data, uint16_t cap, int *actual) {
    uint16_t off = id ? 1 : 0;
    if (cap + off > sizeof ctl_buf) return -4;
    memset(ctl_buf, 0, cap + off);
    int e = control(h, 0xA1, 0x01, (uint16_t)((type << 8) | id), h->ifnum, (uint16_t)(cap + off), 1000);
    if (e) return e;
    memcpy(data, ctl_buf + off, cap);
    *actual = cap;
    return 0;
}

int host_send_output(host_if_t *h, uint8_t id, const uint8_t *data, uint16_t len) {
    if (!h->has_out) return host_set_report(h, 2, id, data, len);
    AmvParser *p = parser_of(h);
    if (!p || !host_if_alive(h)) return -3;
    uint8_t buf[1024];
    uint16_t off = id ? 1 : 0;
    if (len + off > sizeof buf) return -4;
    buf[0] = id;
    memcpy(buf + off, data, len);
    uint16_t n = len + off;
    // Interrupt OUT transfers are a whole report; a short report is padded to the endpoint size as the OS does.
    if (h->out_mps && n < h->out_mps && h->out_mps <= sizeof buf) { memset(buf + n, 0, h->out_mps - n); n = h->out_mps; }
    uint32_t start = millis();
    for (;;) {
        p->out_busy = true;
        if (p->sendPacket(buf, n)) break;
        p->out_busy = false;
        if (!host_if_alive(h)) return -3;
        if (millis() - start > 200) return -2;
        usbhost.Task();
    }
    while (p->out_busy) {
        if (!host_if_alive(h)) return -3;
        if (millis() - start > 200) { p->out_busy = false; return -2; }
        usbhost.Task();
    }
    return 0;
}

int host_pop_input(host_if_t *h, uint8_t *raw, int cap) {
    uint32_t p = irq_save();
    int n = 0;
    if (h->r_count) {
        unsigned i = h->r_head;
        n = h->r_len[i] < cap ? h->r_len[i] : cap;
        memcpy(raw, h->r_data[i], n);
        h->r_head = (uint8_t)((i + 1) % HOST_RING);
        h->r_count--;
    }
    irq_restore(p);
    return n;
}

void host_flush_input(host_if_t *h) {
    uint32_t p = irq_save();
    h->r_count = 0;
    irq_restore(p);
}

int host_set_boot_protocol(host_if_t *h) {
    return control(h, 0x21, 0x0B, 0, h->ifnum, 0, 500);      // SET_PROTOCOL(boot)
}

bool host_poll(void) {
    bool changed = false;
    for (auto &h : host_ifs) {
        if (!h.used || !h.gone) continue;
        if (h.src >= 0) { pc_out_release((unsigned)h.src); h.src = -1; }
        changed = true;
        // the mouse manager frees the record once it has seen it go (h.announced)
    }
    return changed;
}
