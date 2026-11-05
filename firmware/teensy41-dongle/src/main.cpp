/*
 * AnyMouse Teensy 4.1 dongle, firmware 0.2.0.
 *
 *   mice / receivers --(USB host port, High-Speed, hubs)--> octant vectoring --(main USB port, High-Speed)--> PC
 *
 * The same dongle as the ESP32-P4 (firmware/esp32p4-dongle), on a board whose two USB ports are both High-Speed,
 * so the PC sees an 8 kHz mouse:
 *   - host.cpp     every HID interface on the host port; mouse reports are decoded, scaled and sent in the host
 *                  interrupt (pc_out.cpp), several mice merged, every button change in its own frame;
 *   - pc_out.cpp   the 16-bit, 16-button mouse report on a 125 us endpoint, sent from interrupts only;
 *   - mice.cpp     the list the vendor channel reports, and OpenMouse's own drivers (js_engine.c: QuickJS-ng with
 *                  the ESP32's bundle) for real CPI, polling rate, battery, LOD and Motion Sync;
 *   - amv_proto_teensy.cpp + amv_store_teensy.cpp  the vendor protocol (usage page 0xFF4D, HELLO handshake) and
 *                  the four profile slots in EEPROM, so the Studio and ./anymouse drive it like the ESP32.
 *
 * Console (USB serial; send a line, end with Enter). h lists the commands.
 */
#include <Arduino.h>
#include <stdarg.h>
#include "amv_octant.h"
#include "amv_vendor_usb.h"
#include "amv_store_teensy.h"
#include "host.h"
#include "js_engine.h"
#include "mice.h"
#include "pc_out.h"
extern "C" int usb_rawhid_tx_idle(void);
void fake_mouse_test(void);
extern "C" uint8_t external_psram_size;

static const uint32_t CYC_PER_US = F_CPU_ACTUAL / 1000000u;
static bool g_periodic = false;
static uint32_t last_stats_ms, vendor_loop_max;

extern "C" void amv_log(const char *fmt, ...) {
    if (!Serial.dtr()) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

// While the OpenMouse bundle loads at boot (about a second), keep the USB host enumerating.
extern "C" void amv_js_idle_hook(void) { host_task(); pc_out_poll(); }

// ---------------------------------------------------------------------------------------------------------------
// protocol platform: every callback runs in loop()

static void apply_state(const float w[8], bool bypass) { host_set_weights(w, bypass); }
static void unit_id(uint8_t mac[6]) {
    uint32_t hi = HW_OCOTP_MAC1, lo = HW_OCOTP_MAC0;
    mac[0] = hi >> 8; mac[1] = hi;
    mac[2] = lo >> 24; mac[3] = lo >> 16; mac[4] = lo >> 8; mac[5] = lo;
}
static uint8_t console_request(uint8_t cmd, const void *payload, uint8_t len, uint8_t *reply = nullptr) {
    uint8_t in[64] = {AMV_MAGIC0, AMV_MAGIC1, AMV_PROTO_VERSION, cmd, 0, 0, len, 0}, out[64];
    memcpy(in + 8, payload, len);
    if (!amv_proto_handle(in, out, AMV_SRC_DEVICE)) return AMV_ST_PENDING;
    if (reply) memcpy(reply, out, 64);
    return out[5];
}

// ---------------------------------------------------------------------------------------------------------------
// console

static void print_stats(uint32_t window_ms) {
    host_stats_t h;
    pc_out_stats_t t;
    host_take_stats(&h);
    pc_out_take_stats(&t);
    float sec = window_ms / 1000.0f, us = 1.0f / CYC_PER_US;
    static const char *HIST[8] = {"<60", "60-100", "100-150", "150-250", "250-500", "0.5-1ms", "1-2ms", ">2ms"};
    amv_log("[host] rx %.0f/s", h.rx / sec);
    if (h.rx > 1) {
        amv_log("  spacing us min/avg/max %.0f/%.0f/%.0f  hist", h.dt_min * us, (double)h.dt_sum / (h.rx - 1) * us, h.dt_max * us);
        for (int i = 0; i < 8; i++) if (h.hist[i]) amv_log(" %s:%lu", HIST[i], (unsigned long)h.hist[i]);
    }
    amv_log("\n");
    if (h.isr_n > 1)
        amv_log("[isr ] %.0f/s  spacing us min/avg/max %.0f/%.0f/%.0f  reports per interrupt 0:%lu 1:%lu 2:%lu 3+:%lu  cpu %.1f%%\n",
                h.isr_n / sec, h.isr_min * us, (double)h.isr_sum / (h.isr_n - 1) * us, h.isr_max * us, (unsigned long)h.isr_per[0],
                (unsigned long)h.isr_per[1], (unsigned long)h.isr_per[2], (unsigned long)h.isr_per[3],
                100.0 * h.isr_busy / ((double)CYC_PER_US * window_ms * 1000.0));
    amv_log("[sum ] mouse x=%ld y=%ld  ->  sent to PC x=%ld y=%ld\n", (long)t.raw_x, (long)t.raw_y, (long)t.sent_x, (long)t.sent_y);
    amv_log("[ pc ] tx %.0f/s  carried %lu  button overflow %lu", t.frames / sec, (unsigned long)t.carried, (unsigned long)t.button_overflow);
    if (t.age_n) amv_log("  motion->queued us avg/max %.1f/%.1f", (double)t.age_sum / t.age_n * us, t.age_max * us);
    if (h.path_n) amv_log("  report->queued us avg/max %.2f/%.2f", (double)h.path_sum / h.path_n * us, h.path_max * us);
    if (h.scale_n) amv_log("  scale us avg/max %.2f/%.2f", (double)h.scale_sum / h.scale_n * us, h.scale_max * us);
    amv_log("  loop max %.0f us%s\n", vendor_loop_max * us, pc_out_selftest_active() ? "  [selftest]" : "");
    vendor_loop_max = 0;
}

static void print_help() {
    amv_log("h help | p stats on/off | i info | m mice | t [s] 8 kHz self-test | b 0|1 bypass | w a | w e ne n nw w sw s se\n"
            "c <itc> host irq threshold | j 0|1|2 driver log | set <mouse> <cpi|rate|lod|ms> <value> | lbl <mouse> <cpi> | fake (OpenMouse self-test)\n");
}

static void handle_line(char *line) {
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = line + strlen(line);
    if (!strcmp(line, "h") || !strcmp(line, "?")) print_help();
    else if (!strcmp(line, "p")) { g_periodic = !g_periodic; amv_log("stats %s\n", g_periodic ? "on" : "off"); }
    else if (!strcmp(line, "i")) {
        amv_state_payload_t st; amv_proto_snapshot(&st);
        amv_log("AnyMouse Teensy 4.1 dongle, firmware %u.%u.%u, gen %lu, slot %u, saved slots 0x%02X%s\n", AMV_TEENSY_FW_MAJOR,
                AMV_TEENSY_FW_MINOR, AMV_TEENSY_FW_PATCH, (unsigned long)st.gen, st.active_slot + 1,
                amv_store_teensy_ops()->valid_mask(), st.bypass ? ", bypass on" : "");
        amv_log("weights E %.3f NE %.3f N %.3f NW %.3f W %.3f SW %.3f S %.3f SE %.3f\n", st.w[0], st.w[1], st.w[2], st.w[3],
                st.w[4], st.w[5], st.w[6], st.w[7]);
        uint32_t pc = USB2_PORTSC1;
        amv_log("host port PORTSC1=%08lX (%s), ITC %u, PC link %s, PSRAM %u MB\n", (unsigned long)pc,
                ((pc >> 26) & 3) == 2 ? "high speed" : ((pc >> 26) & 3) == 0 ? "full speed" : "low speed", host_get_itc(),
                usb_configuration ? "configured" : "not configured", external_psram_size);
        mice_print();
    }
    else if (!strcmp(line, "m")) mice_print();
    else if (!strcmp(line, "fake")) fake_mouse_test();
    else if (!strcmp(line, "t")) {
        int s = atoi(arg); if (s <= 0) s = 5; if (s > 60) s = 60;
        pc_out_selftest(millis() + s * 1000UL);
        amv_log("self-test for %d s: 8 kHz +1/-1 X steps to the PC\n", s);
    }
    else if (!strcmp(line, "c")) {
        int v = atoi(arg);
        if (v < 0 || v > 64) amv_log("ITC 0..64\n"); else { host_set_itc(v); amv_log("ITC now %d\n", v); }
    }
    else if (!strcmp(line, "j")) { js_set_log(atoi(arg)); amv_log("driver log %d\n", atoi(arg)); }
    else if (!strcmp(line, "b")) {
        uint8_t b = atoi(arg) != 0;
        console_request(AMV_CMD_SET_BYPASS, &b, 1);
        amv_log("bypass %s\n", b ? "on" : "off");
    }
    else if (!strcmp(line, "w")) {
        float v[8]; int n = 0; char *tok = strtok(arg, " ");
        while (tok && n < 8) { v[n++] = atof(tok); tok = strtok(nullptr, " "); }
        if (n == 1) for (int i = 1; i < 8; i++) v[i] = v[0];
        else if (n != 8) { amv_log("usage: w a   or   w e ne n nw w sw s se\n"); return; }
        amv_set_weights_req_t req; req.base_gen = AMV_GEN_ANY; memcpy(req.w, v, sizeof v);
        uint8_t s = console_request(AMV_CMD_SET_WEIGHTS, &req, sizeof req);
        amv_log(s == AMV_ST_OK ? "weights set\n" : "weights refused (0.01..5.0)\n");
    }
    else if (!strcmp(line, "set")) {
        char what[8] = {0}; int idx = -1; unsigned value = 0;
        if (sscanf(arg, "%d %7s %u", &idx, what, &value) != 3) { amv_log("usage: set <mouse> <cpi|rate|lod|ms> <value>\n"); return; }
        uint8_t id = !strcmp(what, "cpi") ? AMV_SET_CPI : !strcmp(what, "rate") ? AMV_SET_POLLING : !strcmp(what, "lod") ? AMV_SET_LOD
                   : !strcmp(what, "ms") ? AMV_SET_MOTION_SYNC : 0;
        amv_setting_set_req_t req = {(uint8_t)idx, id, (uint16_t)value};
        uint8_t s = console_request(AMV_CMD_SET_MOUSE_SETTING, &req, sizeof req);
        amv_log(s == AMV_ST_PENDING ? "sent to the mouse; the reply goes to the vendor channel, see m\n" : "status %u\n", s);
    }
    else if (!strcmp(line, "lbl")) {
        int idx = -1; unsigned cpi = 0;
        if (sscanf(arg, "%d %u", &idx, &cpi) != 2) { amv_log("usage: lbl <mouse> <cpi>\n"); return; }
        amv_mouse_set_req_t req = {(uint8_t)idx, 0, (uint16_t)cpi};
        amv_log("status %u\n", console_request(AMV_CMD_SET_MOUSE_CPI, &req, sizeof req));
    }
    else amv_log("unknown command; h for help\n");
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

// ---------------------------------------------------------------------------------------------------------------

void setup() {
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    pc_out_begin();

    amv_plat_t plat = {};
    plat.apply = apply_state;
    plat.unit_id = unit_id;
    plat.uptime_ms = []() -> uint32_t { return millis(); };
    plat.reboot = []() { SCB_AIRCR = 0x05FA0004; while (true) {} };
    plat.mouse_count = []() -> uint8_t { return mice_count(); };
    plat.mouse = mice_ops();
    plat.store = amv_store_teensy_ops();
    amv_proto_init(&plat);
    uint8_t slot; amv_profile_t profile;
    if (plat.store->load_boot(&slot, &profile)) amv_proto_restore(slot, &profile, amv_store_boot_bypass());

    host_begin();
    bool engine = js_begin();      // about a second; motion already flows (interrupts), enumeration continues
    mice_begin(engine);
    last_stats_ms = millis();
}

void loop() {
    uint32_t loop_start = ARM_DWT_CYCCNT;
    host_task();
    pc_out_poll();
    mice_poll();
    amv_proto_tick();

    static uint8_t reply[64];
    static bool reply_pending;
    uint8_t frame[64];
    if (!reply_pending) reply_pending = amv_proto_poll_mouse(reply);
    if (!reply_pending && !amv_proto_reboot_pending() && amv_vendor_recv(frame))
        reply_pending = amv_proto_handle(frame, reply) != 0;
    if (reply_pending && amv_vendor_send(reply)) reply_pending = false;
    if (!reply_pending && amv_proto_reboot_pending() && usb_rawhid_tx_idle()) amv_proto_run_deferred();
    if (!reply_pending && amv_proto_take_event(frame)) amv_proto_count_event(amv_vendor_send(frame));   // events may drop

    poll_console();

    // auto-save the boot state 2 s after the last change
    uint32_t now = millis();
    static uint32_t saved_gen = 1, dirty_since;
    static bool dirty;
    amv_state_payload_t state; amv_proto_snapshot(&state);
    if (state.gen != saved_gen) { saved_gen = state.gen; dirty_since = now; dirty = true; }
    if (dirty && now - dirty_since >= 2000) {
        amv_profile_t p; memcpy(p.w, state.w, sizeof p.w);
        if (amv_store_teensy_ops()->save_boot(state.active_slot, &p, state.bypass)) dirty = false;
        else dirty_since = now;
    }
    if (now - last_stats_ms >= 1000) {
        uint32_t window = now - last_stats_ms;
        last_stats_ms = now;
        if (g_periodic && Serial.dtr()) print_stats(window);
        else { host_stats_t h; pc_out_stats_t t; host_take_stats(&h); pc_out_take_stats(&t); }
    }
    uint32_t elapsed = ARM_DWT_CYCCNT - loop_start;
    if (elapsed > vendor_loop_max) vendor_loop_max = elapsed;
}
