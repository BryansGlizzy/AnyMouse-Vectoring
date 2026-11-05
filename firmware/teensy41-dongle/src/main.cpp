/*
 * AnyMouse Teensy 4.1 bridge, bring-up version.
 *
 *   mouse / receiver --(USB host port, High-Speed)--> octant vectoring --(main USB port, High-Speed)--> PC
 *
 * Goal of this version: prove 8 kHz on both sides and measure what the vectoring costs.
 *   - Host side: every report from the mouse is timestamped in the USB interrupt (CPU cycle counter),
 *     so the console shows reports/s and the spacing between reports.
 *   - PC side: the patched core (tools/make_core.sh) gives the mouse endpoint bInterval 1, which is a
 *     125 us poll at High-Speed. One report is kept in flight at a time; a new one is queued the moment the
 *     previous one is collected, carrying everything that arrived in between.
 *   - Scaling: amv_octant_scale() from common/amv_octant.h, the same code as the ESP32-P4 dongle and the
 *     Linux engine, applied to every native report. Weights are 1.0 until you change them ('w').
 *
 * Console (USB serial, 115200 is ignored; send a line, end with Enter):
 *   h                 help
 *   p                 toggle the once-a-second stats line
 *   i                 what is attached to the host port
 *   t [seconds]       self-test: send 8 kHz +1/-1 X reports to the PC without a mouse (net motion zero)
 *   b 0|1             bypass the vectoring (1 = raw pass-through)
 *   w a                set all 8 weights to a;  w e ne n nw w sw s se  sets each (E,NE,N,NW,W,SW,S,SE)
 *   q                 ask a Finalmouse dongle for CPI / polling / battery and print the answers
 *   r <hz>            set a Finalmouse dongle's polling rate (500, 1000, 2000, 4000, 8000)
 */
#include <Arduino.h>
#include <USBHost_t36.h>
#include "amv_octant.h"
#include "amv_vendor_usb.h"
#include "amv_store_teensy.h"
extern "C" int usb_rawhid_tx_idle(void);

// ---------------------------------------------------------------------------------------------------------
// shared state (written in the host interrupt, read by the main loop)
// ---------------------------------------------------------------------------------------------------------
static volatile int32_t acc_dx, acc_dy, acc_wheel, acc_pan;   // scaled motion waiting for the PC link
static volatile uint8_t cur_buttons;                          // latest button state from the mouse
static volatile bool    pending;                              // acc_* holds something not yet sent
static volatile uint32_t pending_since;                       // cycle count when it became pending

static float g_w[8] = {1, 1, 1, 1, 1, 1, 1, 1};
static volatile bool g_bypass = false;
static amv_octant_state_t g_state;                            // sub-pixel remainders

static const uint32_t CYC_PER_US = F_CPU_ACTUAL / 1000000u;

// Host-side statistics, snapshot-and-cleared once a second.
struct HostStats {
    uint32_t rx;              // reports from the mouse
    uint32_t vendor;          // vendor (non-motion) replies
    uint32_t dt_min, dt_max;  // spacing between reports, cycles
    uint64_t dt_sum;
    uint32_t hist[8];         // spacing histogram
    uint32_t scale_calls;
    uint32_t scale_sum, scale_max;   // cycles spent in amv_octant_scale
    uint32_t isr_n, isr_sum, isr_min, isr_max;   // host-controller interrupts: count and spacing (cycles)
    int32_t raw_x, raw_y;     // sum of the mouse's own deltas
    uint32_t isr_busy;        // cycles spent inside the host interrupt (driver included)
    uint32_t isr_per[4];      // interrupts that delivered 0, 1, 2, 3+ reports
    uint32_t parse_n, parse_sum, parse_max;   // cycles from raw report to accumulated (parse + scale)
};
static HostStats hs;
static uint32_t last_rx_cyc;
static uint32_t in_data_cyc;      // cycle count when the current raw report entered the driver
static uint32_t hist_edge_cyc[7];
static const uint16_t HIST_EDGE_US[7] = {60, 100, 150, 250, 500, 1000, 2000};
static const char *HIST_NAME[8] = {"<60", "60-100", "100-150", "150-250", "250-500", "0.5-1ms", "1-2ms", ">2ms"};

// PC-side statistics.
struct TxStats {
    uint32_t frames;          // reports queued to the PC
    uint32_t fails;
    uint32_t clamped;         // frames where the motion exceeded +-127 and the rest was carried
    uint32_t age_sum, age_max;// cycles between "motion arrived" and "report queued"
    uint32_t age_n;
    int32_t sent_x, sent_y;   // sum of what was queued to the PC
};
static TxStats ts;

static inline uint32_t cyc() { return ARM_DWT_CYCCNT; }

// ---------------------------------------------------------------------------------------------------------
// USB host: claim the mouse collection, timestamp and scale every report
// ---------------------------------------------------------------------------------------------------------
USBHost host;
USBHub hub1(host);
// Retain endpoint metadata while the interface descriptors are available.
class MouseParser : public USBHIDParser {
public:
    MouseParser(USBHost &h) : USBHIDParser(h) {}
    uint16_t nominal=0;
    uint8_t speed=0;
protected:
    bool claim(Device_t *d, int type, const uint8_t *p, uint32_t n) override {
        if (!USBHIDParser::claim(d,type,p,n)) return false;
        speed=d->speed; nominal=0;
        for (uint32_t i=0; i+2<=n && p[i]>=2 && i+p[i]<=n; i+=p[i]) {
            if (p[i+1]==5 && p[i]>=7 && (p[i+2]&0x80)) {
                unsigned interval=p[i+6];
                if (interval) nominal=speed==2 ? (interval<=16 ? 8000u/(1u<<(interval-1)) : 0) : 1000u/interval;
            }
        }
        return true;
    }
};
MouseParser hid1(host), hid2(host), hid3(host);

// More than the default two receive buffers, so an 8 kHz stream survives a late interrupt.
DMAMEM static uint8_t rxbuf[3][4][256] __attribute__((aligned(32)));

class Bridge : public USBHIDInput {
public:
    USBHIDParser *parser = nullptr;
    uint16_t vid = 0, pid = 0;
    volatile bool online = false;
    amv_mouse_info_t info={};
    uint8_t descriptor[928];
    uint16_t descriptor_len=0;
    volatile uint32_t reports=0;
    uint16_t next_uid=0;


    hidclaim_t claim_collection(USBHIDParser *driver, Device_t *dev, uint32_t topusage) override {
        if (topusage != 0x10002) return CLAIM_NO;              // Generic Desktop / Mouse only
        if (mydevice != nullptr && dev != mydevice) return CLAIM_NO;
        if (claimed) return CLAIM_NO; // exactly one collection
        info={}; info.tier=AMV_TIER_GENERIC; info.battery=0xFF;
        info.vid=dev->idVendor; info.pid=dev->idProduct;
        info.uid=++next_uid; info.iface=driver->interfaceNumber();
        info.speed=dev->speed;
        info.nominal_hz=static_cast<MouseParser *>(driver)->nominal;
        if (dev->strbuf) strncpy(info.name, (const char *)&dev->strbuf->buffer[dev->strbuf->iStrings[strbuf_t::STR_ID_PROD]], sizeof info.name-1);
        descriptor_len=driver->getHIDReportDescriptorSize();
        if (descriptor_len>sizeof descriptor) descriptor_len=sizeof descriptor;
        memcpy(descriptor,driver->getHIDReportDescriptor(),descriptor_len);
        // Inspect short HID items for the layout facts known before reports arrive.
        uint32_t page=0, size=0, usage=0;
        for (unsigned i=0;i<descriptor_len;) {
            uint8_t tag=descriptor[i++];
            if (tag==0xFE) { if(i+2>descriptor_len) break; unsigned n=descriptor[i]; i+=2+n; continue; }
            unsigned n=tag&3; if(n==3) n=4;
            if(i+n>descriptor_len) break;
            uint32_t v=0; for(unsigned j=0;j<n;++j) v|=uint32_t(descriptor[i++])<<(8*j);
            switch(tag&0xFC) {
            case 0x04: page=v; break;
            case 0x08: usage=v; break;
            case 0x74: size=v; break;
            case 0x84: info.flags|=AMV_MF_REPORT_IDS; break;
            case 0x28: if(page==9) info.num_buttons=v>255?255:v; break;
            case 0x80:
                if(page==1 && usage==0x30) info.x_bits=size;
                if(page==1 && usage==0x38) info.flags|=AMV_MF_WHEEL;
                if(page==12 && usage==0x238) info.flags|=AMV_MF_PAN;
                break;
            }
        }
        reports=0;
        mydevice = dev;
        parser = driver;
        vid = dev->idVendor;
        pid = dev->idProduct;
        claimed++;
        online = true;
        return CLAIM_REPORT;
    }
    void disconnect_collection(Device_t *dev) override {
        if (--claimed <= 0) {
            claimed = 0;
            mydevice = nullptr;
            parser = nullptr;
            online = false;
            cur_buttons = 0;   // never leave a button held after an unplug
        }
    }

    // Called in the USB interrupt for every raw report. Timestamp it; let the generic parser decode motion.
    bool hid_process_in_data(const Transfer_t *t) override {
        uint32_t now = cyc();
        in_data_cyc = now;
        if (hs.rx) {
            uint32_t dt = now - last_rx_cyc;
            if (hs.rx == 1 || dt < hs.dt_min) hs.dt_min = dt;
            if (dt > hs.dt_max) hs.dt_max = dt;
            hs.dt_sum += dt;
            uint32_t b = 0;
            while (b < 7 && dt >= hist_edge_cyc[b]) b++;
            hs.hist[b]++;
        }
        last_rx_cyc = now;
        hs.rx++;
        reports++;
        const uint8_t *p = (const uint8_t *)t->buffer;
        if (vid == 0x361D && t->length >= 4 && p[0] == 5) {   // Finalmouse vendor reply
            hs.vendor++;
            hs.rx--;                                          // not motion
            on_finalmouse_reply(p, t->length);
            return true;
        }
        return false;
    }

    // The parser calls begin() once per Input item (buttons, X/Y, wheel, ...) and end() once per top-level
    // collection, so the per-report values are cleared at the end of end(), not here.
    void hid_input_begin(uint32_t topusage, uint32_t type, int lgmin, int lgmax) override {}
    void hid_input_data(uint32_t usage, int32_t value) override {
        uint32_t page = usage >> 16;
        usage &= 0xFFFF;
        if (page == 9 && usage >= 1 && usage <= 8) {
            if (value) btn |= (uint8_t)(1u << (usage - 1));
        } else if (page == 1) {
            switch (usage) {
                case 0x30: dx = value; break;
                case 0x31: dy = value; break;
                case 0x32: pn = value; break;
                case 0x38: wh = value; break;
            }
        } else if (page == 12 && usage == 0x238) {
            pn = value;
        }
    }
    void hid_input_end() override {
        int32_t ox = dx, oy = dy;
        hs.raw_x += dx; hs.raw_y += dy;
        if ((dx || dy) && !g_bypass) {
            uint8_t oct;
            uint32_t t0 = cyc();
            amv_octant_scale(g_w, &g_state, dx, dy, &ox, &oy, &oct);
            uint32_t c = cyc() - t0;
            hs.scale_calls++;
            hs.scale_sum += c;
            if (c > hs.scale_max) hs.scale_max = c;
        }
        // The main loop only touches these with interrupts masked, so plain adds are safe here.
        acc_dx += ox;
        acc_dy += oy;
        acc_wheel += wh;
        acc_pan += pn;
        cur_buttons = btn;
        if (!pending && (ox || oy || wh || pn || btn != last_sent_buttons)) {
            pending = true;
            pending_since = cyc();
        }
        uint32_t c = cyc() - in_data_cyc;           // whole path: parse + scale + accumulate
        hs.parse_sum += c;
        if (c > hs.parse_max) hs.parse_max = c;
        hs.parse_n++;
        dx = dy = wh = pn = 0;
        btn = 0;
    }

    // Set by the main loop after each send, read here only to decide whether a button edge is news.
    volatile uint8_t last_sent_buttons = 0;

    // Output report on the interrupt OUT endpoint (Finalmouse commands, see src/hid_driver.c build_fm_report()).
    bool send_finalmouse(uint8_t cmd, const uint8_t *payload, uint8_t plen) {
        if (!parser) return false;
        uint8_t r[64] = {0};
        r[0] = 0x04;
        r[1] = (uint8_t)(2 + plen);
        r[2] = (uint8_t)(0x80 | (cmd & 0x7F));
        r[3] = plen;
        for (uint8_t i = 0; i < plen && i < 60; i++) r[4 + i] = payload[i];
        return parser->sendPacket(r, 64);
    }

private:
    int claimed = 0;
    int32_t dx = 0, dy = 0, wh = 0, pn = 0;
    uint8_t btn = 0;
    void on_finalmouse_reply(const uint8_t *p, uint32_t len);
};
static Bridge bridge;

// Chain in front of USBHost_t36's own interrupt handler, to see how often the controller interrupts and how
// many reports each interrupt delivers.
static void (*orig_host_isr)(void);
static uint32_t isr_last_cyc;
static void host_isr_wrap() {
    uint32_t now = cyc();
    uint32_t before = hs.rx;
    orig_host_isr();
    if (isr_last_cyc) {
        uint32_t dt = now - isr_last_cyc;
        if (hs.isr_n == 0 || dt < hs.isr_min) hs.isr_min = dt;
        if (dt > hs.isr_max) hs.isr_max = dt;
        hs.isr_sum += dt;
    }
    isr_last_cyc = now;
    hs.isr_n++;
    hs.isr_busy += cyc() - now;
    uint32_t got = hs.rx - before;
    hs.isr_per[got > 3 ? 3 : got]++;
}

static volatile bool g_print_replies = false;
void Bridge::on_finalmouse_reply(const uint8_t *p, uint32_t len) {
    if (!g_print_replies) return;
    uint8_t cmd = p[2], plen = p[3];
    if (plen > len - 4) plen = (uint8_t)(len - 4);
    const uint8_t *d = p + 4;
    if ((cmd == 3 || cmd == 16) && plen >= 2) Serial.printf("  [ulx] CPI %u\n", d[0] | (d[1] << 8));
    else if ((cmd == 4 || cmd == 17) && plen >= 2) Serial.printf("  [ulx] polling rate %u Hz\n", d[0] | (d[1] << 8));
    else if (cmd == 5 && plen >= 2) Serial.printf("  [ulx] battery %u mV\n", d[0] | (d[1] << 8));
    else if (cmd == 18 && plen >= 1) Serial.printf("  [ulx] motion sync %s\n", d[0] == 1 ? "on" : "off");
    else if ((cmd == 11 || cmd == 12) && plen > 0) Serial.printf("  [ulx] %s firmware %.*s\n", cmd == 11 ? "dongle" : "mouse", plen, (const char *)d);
    else if (cmd == 13 && plen >= 1) Serial.printf("  [ulx] rssi %d dBm\n", (int8_t)d[0]);
}

// ---------------------------------------------------------------------------------------------------------
// PC side: one report in flight, queued as soon as the previous one is collected
// ---------------------------------------------------------------------------------------------------------
static uint32_t selftest_until_ms;
static bool selftest_flip;

static inline int32_t clamp127(int32_t v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

static void pump_tx() {
    if (!usb_configuration) {
        // PC asleep or not enumerated: drop motion rather than replay a huge jump later.
        if (pending) { __disable_irq(); acc_dx = acc_dy = acc_wheel = acc_pan = 0; pending = false; __enable_irq(); }
        return;
    }
    if (!usb_mouse_idle()) return;

    if (selftest_until_ms) {
        if ((int32_t)(millis() - selftest_until_ms) >= 0) { selftest_until_ms = 0; return; }
        selftest_flip = !selftest_flip;
        if (usb_mouse_move(selftest_flip ? 1 : -1, 0, 0, 0) == 0) ts.frames++; else ts.fails++;
        return;
    }
    if (!pending) return;

    __disable_irq();
    int32_t cx = clamp127(acc_dx), cy = clamp127(acc_dy), cw = clamp127(acc_wheel), cp = clamp127(acc_pan);
    bool clamped = (cx != acc_dx) || (cy != acc_dy) || (cw != acc_wheel) || (cp != acc_pan);
    acc_dx -= cx; acc_dy -= cy; acc_wheel -= cw; acc_pan -= cp;
    uint8_t b = cur_buttons;
    uint32_t since = pending_since;
    bool more = acc_dx || acc_dy || acc_wheel || acc_pan;
    pending = more;
    if (more) pending_since = cyc();
    __enable_irq();

    if (!cx && !cy && !cw && !cp && b == bridge.last_sent_buttons) return;   // e.g. a rounding-only frame
    usb_mouse_buttons_state = b;
    uint32_t age = cyc() - since;
    if (usb_mouse_move((int8_t)cx, (int8_t)cy, (int8_t)cw, (int8_t)cp) == 0) {
        bridge.last_sent_buttons = b;
        ts.frames++;
        ts.sent_x += cx; ts.sent_y += cy;
        if (clamped) ts.clamped++;
        ts.age_sum += age; ts.age_n++;
        if (age > ts.age_max) ts.age_max = age;
    } else {
        ts.fails++;
        __disable_irq();                       // keep the motion for the next try
        acc_dx += cx; acc_dy += cy; acc_wheel += cw; acc_pan += cp; pending = true;
        __enable_irq();
    }
}

// ---------------------------------------------------------------------------------------------------------
// protocol platform: all callbacks run on the loop thread.
static uint32_t vendor_loop_max;
static void apply_state(const float w[8], bool bypass) {
    __disable_irq();
    if (memcmp(g_w, w, sizeof g_w) || g_bypass != bypass) g_state = {};
    memcpy(g_w, w, sizeof g_w);
    g_bypass = bypass;
    __enable_irq();
}
static void unit_id(uint8_t mac[6]) {
    uint32_t hi = HW_OCOTP_MAC1, lo = HW_OCOTP_MAC0;
    mac[0] = hi >> 8; mac[1] = hi;
    mac[2] = lo >> 24; mac[3] = lo >> 16; mac[4] = lo >> 8; mac[5] = lo;
}
static void protocol_stats(uint8_t *p, uint8_t *len) {
    // The wire layout is protocol counters, with no host/tx equivalents.
    amv_stats_payload_t stats = {};
    memcpy(p, &stats, sizeof stats); *len = sizeof stats;
}
static void console_request(uint8_t cmd, const void *payload, uint8_t len) {
    uint8_t in[64] = {AMV_MAGIC0, AMV_MAGIC1, AMV_PROTO_VERSION, cmd, 0, 0, len, 0}, out[64];
    memcpy(in + 8, payload, len);
    amv_proto_handle(in, out, AMV_SRC_DEVICE);
}
// console
// ---------------------------------------------------------------------------------------------------------
static bool g_periodic = true;
static uint32_t last_stats_ms;

static void print_stats(uint32_t window_ms) {
    HostStats h; TxStats t;
    __disable_irq();
    h = hs; memset(&hs, 0, sizeof(hs));
    t = ts; memset(&ts, 0, sizeof(ts));
    __enable_irq();
    float sec = window_ms / 1000.0f;
    float us_per_cyc = 1.0f / CYC_PER_US;

    Serial.printf("[host] %s %04X:%04X  rx %.0f/s", bridge.online ? "mouse" : "none ", bridge.vid, bridge.pid, h.rx / sec);
    if (h.rx > 1) {
        Serial.printf("  spacing us min/avg/max %.0f/%.0f/%.0f", h.dt_min * us_per_cyc,
                      (double)h.dt_sum / (h.rx - 1) * us_per_cyc, h.dt_max * us_per_cyc);
        Serial.print("  hist");
        for (int i = 0; i < 8; i++) if (h.hist[i]) Serial.printf(" %s:%lu", HIST_NAME[i], (unsigned long)h.hist[i]);
    }
    Serial.println();
    if (h.isr_n > 1)
        Serial.printf("[isr ] %.0f/s  spacing us min/avg/max %.0f/%.0f/%.0f  reports per interrupt 0:%lu 1:%lu 2:%lu 3+:%lu  cpu %.1f%%\n",
                      h.isr_n / sec, h.isr_min * us_per_cyc, (double)h.isr_sum / (h.isr_n - 1) * us_per_cyc, h.isr_max * us_per_cyc,
                      (unsigned long)h.isr_per[0], (unsigned long)h.isr_per[1], (unsigned long)h.isr_per[2], (unsigned long)h.isr_per[3],
                      100.0 * h.isr_busy / ((double)CYC_PER_US * window_ms * 1000.0));
    Serial.printf("[sum ] mouse x=%ld y=%ld  ->  sent to PC x=%ld y=%ld\n", (long)h.raw_x, (long)h.raw_y, (long)t.sent_x, (long)t.sent_y);
    Serial.printf("[ pc ] tx %.0f/s  fails %lu  carried %lu", t.frames / sec, (unsigned long)t.fails, (unsigned long)t.clamped);
    if (t.age_n) Serial.printf("  motion->queued us avg/max %.1f/%.1f", (double)t.age_sum / t.age_n * us_per_cyc, t.age_max * us_per_cyc);
    if (h.parse_n) Serial.printf("  report->accumulated us avg/max %.2f/%.2f", (double)h.parse_sum / h.parse_n * us_per_cyc, h.parse_max * us_per_cyc);
    if (h.scale_calls) Serial.printf("  scale %lu cyc avg / %lu max (%.2f/%.2f us)", (unsigned long)(h.scale_sum / h.scale_calls),
                                     (unsigned long)h.scale_max, (double)h.scale_sum / h.scale_calls * us_per_cyc, h.scale_max * us_per_cyc);
    Serial.printf("  vendor-loop max %.2f us", vendor_loop_max * us_per_cyc);
    vendor_loop_max = 0;
    Serial.printf("  %s%s\n", g_bypass ? "[bypass] " : "", selftest_until_ms ? "[selftest]" : "");
}

static void print_help() {
    Serial.println("h help | p stats on/off | i info | c <itc> host irq threshold | t [sec] 8kHz self-test | b 0|1 bypass | w a | w e ne n nw w sw s se | q query ULX | r <hz> set ULX rate");
}

static void handle_line(char *line) {
    char cmd = line[0];
    char *arg = line + 1;
    while (*arg == ' ') arg++;
    switch (cmd) {
        case 'h': case '?': print_help(); break;
        case 'p': g_periodic = !g_periodic; Serial.printf("stats %s\n", g_periodic ? "on" : "off"); break;
        case 'i': {
            amv_state_payload_t state; amv_proto_snapshot(&state);
            Serial.printf("firmware 0.1.0 gen=%lu active=%u slots=0x%02X\n", (unsigned long)state.gen, state.active_slot, amv_store_teensy_ops()->valid_mask());
            Serial.printf("host port: %s", bridge.online ? "mouse claimed" : "no mouse claimed");
            if (bridge.online) Serial.printf(" (%04X:%04X)", bridge.vid, bridge.pid);
            Serial.printf("\nPORTSC1=%08lX  speed=%s  F_CPU=%lu MHz  pc link: %s\n", (unsigned long)USB2_PORTSC1,
                          (((USB2_PORTSC1 >> 26) & 3) == 2) ? "high" : ((((USB2_PORTSC1 >> 26) & 3) == 0) ? "full" : "low"),
                          (unsigned long)(F_CPU_ACTUAL / 1000000), usb_configuration ? "configured" : "not configured");
            Serial.printf("USBHS_USBCMD=%08lX (ITC=%lu micro-frames)\n", (unsigned long)USBHS_USBCMD, (unsigned long)((USBHS_USBCMD >> 16) & 0xFF));
            Serial.printf("weights: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f%s\n", g_w[0], g_w[1], g_w[2], g_w[3], g_w[4], g_w[5], g_w[6], g_w[7],
                          g_bypass ? "  (bypass on)" : "");
            break;
        }
        case 't': {
            int s = atoi(arg); if (s <= 0) s = 5; if (s > 60) s = 60;
            selftest_until_ms = millis() + s * 1000UL; if (!selftest_until_ms) selftest_until_ms = 1;
            Serial.printf("self-test for %d s: 8 kHz +1/-1 X steps to the PC\n", s);
            break;
        }
        case 'c': {   // host controller interrupt threshold, in micro-frames (0 = interrupt at once)
            int v = atoi(arg);
            if (v < 0 || v > 64) { Serial.println("ITC 0..64"); break; }
            USBHS_USBCMD = (USBHS_USBCMD & ~(0xFFu << 16)) | ((uint32_t)v << 16);
            Serial.printf("ITC now %d\n", v);
            break;
        }
        case 'b': { uint8_t b = atoi(arg) != 0; console_request(AMV_CMD_SET_BYPASS, &b, 1); } Serial.printf("bypass %s\n", g_bypass ? "on" : "off"); break;
        case 'w': {
            float v[8]; int n = 0; char *tok = strtok(arg, " ");
            while (tok && n < 8) { v[n++] = atof(tok); tok = strtok(nullptr, " "); }
            if (n == 1) for (int i = 1; i < 8; i++) v[i] = v[0];
            else if (n != 8) { Serial.println("usage: w a   or   w e ne n nw w sw s se"); break; }
            for (int i = 0; i < 8; i++) if (!(v[i] >= 0.01f && v[i] <= 5.0f)) { Serial.println("weights must be 0.01..5.0"); return; }
            amv_set_weights_req_t req; req.base_gen = AMV_GEN_ANY; memcpy(req.w, v, sizeof v);
            console_request(AMV_CMD_SET_WEIGHTS, &req, sizeof req);
            Serial.println("weights set");
            break;
        }
        case 'q': {
            if (bridge.vid != 0x361D) { Serial.println("no Finalmouse attached"); break; }
            g_print_replies = true;
            Serial.println(bridge.send_finalmouse(96, nullptr, 0) ? "asked (wake-all); replies follow" : "send failed");
            break;
        }
        case 'r': {
            int hz = atoi(arg);
            if (bridge.vid != 0x361D) { Serial.println("no Finalmouse attached"); break; }
            if (hz != 500 && hz != 1000 && hz != 2000 && hz != 4000 && hz != 8000) { Serial.println("rate must be 500, 1000, 2000, 4000 or 8000"); break; }
            uint8_t pl[2] = {(uint8_t)(hz & 0xFF), (uint8_t)(hz >> 8)};
            g_print_replies = true;
            Serial.printf(bridge.send_finalmouse(17, pl, 2) ? "polling rate %d Hz sent\n" : "send failed (%d)\n", hz);
            break;
        }
        default: Serial.println("unknown command; h for help");
    }
}

static void poll_console() {
    static char line[96];
    static uint8_t n;
    while (Serial.available()) {
        int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (n) { line[n] = 0; n = 0; handle_line(line); }
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
static amv_mouse_info_t mouse_entries[4];
static uint8_t mouse_entries_count;
static const amv_mouse_ops_t mouse_ops = {
    []() -> uint8_t { return mouse_entries_count; },
    [](uint8_t i, amv_mouse_info_t *out) -> bool {
        if(i>=mouse_entries_count) return false;
        *out=mouse_entries[i]; return true;
    },
    [](uint8_t i, uint16_t *n) -> const uint8_t * {
        if(i<mouse_entries_count && mouse_entries[i].tier!=AMV_TIER_IGNORED) {
            *n=bridge.descriptor_len; return bridge.descriptor;
        }
        *n=0; return nullptr;
    }
};
// Called after host.Task(), never by the USB interrupt. Snapshot and publish changes.
static void update_mouse_entries() {
    static uint32_t sample_ms, sample_reports;
    static uint16_t rates[10];
    static unsigned rate_slot;
    static uint16_t measured_uid;
    uint32_t now=millis();
    if(measured_uid!=bridge.info.uid) {
        measured_uid=bridge.info.uid; memset(rates,0,sizeof rates);
        sample_ms=now; sample_reports=bridge.reports;
    }
    if(now-sample_ms>=1000) {
        uint32_t reports=bridge.reports;
        rates[rate_slot++%10]=min(uint32_t(65535),uint32_t((reports-sample_reports)*1000u/(now-sample_ms)));
        sample_ms=now; sample_reports=reports;
    }
    amv_mouse_info_t next[4]={}; uint8_t n=0;
    if(bridge.online) {
        next[n]=bridge.info;
        for(auto rate: rates) if(rate>next[n].measured_hz) next[n].measured_hz=rate;
        ++n;
    }
    MouseParser *parsers[]={&hid1,&hid2,&hid3};
    for(unsigned j=0;j<3;++j) {
        auto *p=parsers[j];
        if(!*p || (bridge.online && bridge.parser==p)) continue;
        auto &entry=next[n++]; entry.tier=AMV_TIER_IGNORED; entry.reason=AMV_IGN_CLAIM;
        entry.vid=p->idVendor(); entry.pid=p->idProduct(); entry.battery=0xFF;
        entry.uid=0x8000+j; entry.iface=p->interfaceNumber(); entry.speed=p->speed;
        entry.nominal_hz=p->nominal;
        if(p->product()) strncpy(entry.name,(const char *)p->product(),sizeof entry.name-1);
    }
    uint16_t removed[4], attached[4]; unsigned nr=0, na=0;
    for(unsigned i=0;i<mouse_entries_count;++i) {
        bool found=false; for(unsigned j=0;j<n;++j) if(mouse_entries[i].uid==next[j].uid) found=true;
        if(!found) removed[nr++]=mouse_entries[i].uid;
    }
    for(unsigned i=0;i<n;++i) {
        bool found=false; for(unsigned j=0;j<mouse_entries_count;++j) if(next[i].uid==mouse_entries[j].uid) found=true;
        if(!found) attached[na++]=next[i].uid;
    }
    memcpy(mouse_entries,next,sizeof next); mouse_entries_count=n;
    for(unsigned i=0;i<nr;++i) amv_proto_notify_mouse(AMV_MEV_REMOVED,removed[i]);
    for(unsigned i=0;i<na;++i) amv_proto_notify_mouse(AMV_MEV_ATTACHED,attached[i]);
}

void setup() {
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    for (int i = 0; i < 7; i++) hist_edge_cyc[i] = HIST_EDGE_US[i] * CYC_PER_US;
    memset(&hs, 0, sizeof(hs));
    memset(&ts, 0, sizeof(ts));

    amv_plat_t plat = {};
    plat.apply = apply_state; plat.unit_id = unit_id;
    plat.uptime_ms = []() -> uint32_t { return millis(); };
    plat.reboot = []() { SCB_AIRCR = 0x05FA0004; while (true) {} };
    plat.mouse_count = []() -> uint8_t { return mouse_entries_count; };
    plat.mouse = &mouse_ops;
    plat.get_stats = protocol_stats; plat.store = amv_store_teensy_ops();
    amv_proto_init(&plat);
    uint8_t slot; amv_profile_t profile;
    if (plat.store->load_boot(&slot, &profile))
        amv_proto_restore(slot, &profile, amv_store_boot_bypass());

    hid1.setRXBuffers(rxbuf[0][0], rxbuf[0][1], 255, rxbuf[0][2], rxbuf[0][3]);
    hid2.setRXBuffers(rxbuf[1][0], rxbuf[1][1], 255, rxbuf[1][2], rxbuf[1][3]);
    hid3.setRXBuffers(rxbuf[2][0], rxbuf[2][1], 255, rxbuf[2][2], rxbuf[2][3]);
    USBHIDParser::driver_ready_for_hid_collection(&bridge);   // offer the mouse collection to our driver
    host.begin();
    orig_host_isr = _VectorsRam[IRQ_USBHS + 16];
    attachInterruptVector(IRQ_USBHS, host_isr_wrap);
    // USBHost_t36 leaves the controller's interrupt threshold at 1 micro-frame, which batches two 8 kHz reports
    // into one interrupt about half the time. Interrupt at once instead: one report, one interrupt, 125 us apart.
    USBHS_USBCMD &= ~(0xFFu << 16);
    last_stats_ms = millis();
}

void loop() {
    uint32_t loop_start = cyc();
    bool vendor_work = false;
    pump_tx();
    host.Task();
    update_mouse_entries();
    pump_tx();
    static uint8_t reply[64];
    static bool reply_pending;
    uint8_t vendor_frame[64];
    if (!reply_pending && !amv_proto_reboot_pending() && amv_vendor_recv(vendor_frame)) {
        vendor_work = true;
        reply_pending = amv_proto_handle(vendor_frame, reply) != 0;
    }
    if (reply_pending) {
        vendor_work = true;
        if (amv_vendor_send(reply)) reply_pending = false;
    }
    if (!reply_pending && amv_proto_reboot_pending() && usb_rawhid_tx_idle())
        amv_proto_run_deferred();
    if (!reply_pending && amv_proto_take_event(vendor_frame)) {
        vendor_work = true;
        amv_vendor_send(vendor_frame); // events may drop under backpressure
    }
    poll_console();
    uint32_t now = millis();
    static uint32_t saved_gen = 1, dirty_since;
    static bool dirty;
    amv_state_payload_t state; amv_proto_snapshot(&state);
    if (state.gen != saved_gen) {
        saved_gen = state.gen; dirty_since = now; dirty = true;
    }
    if (dirty && now - dirty_since >= 2000) {
        amv_profile_t p; memcpy(p.w, state.w, sizeof p.w);
        if (amv_store_teensy_ops()->save_boot(state.active_slot, &p, state.bypass)) dirty = false;
        else dirty_since = now; // bound retry frequency on backend failure
    }
    if (now - last_stats_ms >= 1000) {
        uint32_t window = now - last_stats_ms;
        last_stats_ms = now;
        if (g_periodic && Serial.dtr()) print_stats(window);
        else { __disable_irq(); memset(&hs, 0, sizeof(hs)); memset(&ts, 0, sizeof(ts)); __enable_irq(); }
    }
    if (vendor_work) {
        uint32_t elapsed = cyc() - loop_start;
        if (elapsed > vendor_loop_max) vendor_loop_max = elapsed;
    }

}
