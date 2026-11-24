#include "ghost.h"
#include <Arduino.h>
#include <string.h>
#include "host.h"
#include "mice.h"
#include "pc_out.h"
#include "usb_ghost.h"

extern "C" int usb_rawhid_tx_idle(void);
extern "C" void amv_log(const char *fmt, ...);

#define GHOST_LOST_DEBOUNCE_MS 1500

static bool s_wanted = false;
static bool s_active = false;
static void *s_cloaked_dev = nullptr;
static uint16_t s_cloaked_vid = 0, s_cloaked_pid = 0;
static uint32_t s_lost_since = 0;
static host_if_t *s_targets[AMV_GHOST_MAX_IFS];
static uint32_t   s_target_serials[AMV_GHOST_MAX_IFS];
static uint8_t    s_n_targets = 0;

// Inspect top-level collections for vendor usage pages (>= 0xFF00).
static bool desc_has_vendor_usage(const uint8_t *d, uint16_t n) {
    uint32_t usage_page = 0;
    int depth = 0;
    uint16_t i = 0;
    while (i < n) {
        uint8_t b = d[i];
        if (b == 0xFE) {
            if (i + 2 >= n) break;
            i += 3 + d[i + 1];
            continue;
        }
        uint16_t sz = b & 3;
        if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        uint8_t type = (b >> 2) & 3;
        uint8_t tag = b >> 4;
        uint32_t v = 0;
        for (uint16_t k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + sz;
        if (type == 1 && tag == 0x0) usage_page = v;
        else if (type == 0 && tag == 0xA) {
            if (depth == 0 && usage_page >= 0xFF00u) return true;
            depth++;
        } else if (type == 0 && tag == 0xC && depth > 0) {
            depth--;
        }
    }
    return false;
}

// Extract top-level vendor collections (>= 0xFF00) from a combined mouse+vendor HID report descriptor
// (e.g. Finalmouse ULX) so Ghost Mode never duplicates the OS mouse collection.
static uint16_t extract_vendor_collections(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t cap) {
    uint32_t usage_page = 0;
    int depth = 0;
    uint16_t preamble_start = 0, out_len = 0;
    bool copying = false;
    uint16_t copy_start = 0;
    uint16_t i = 0;

    while (i < n) {
        uint16_t item_off = i;
        uint8_t b = in[i];
        if (b == 0xFE) {
            if (i + 2 >= n) break;
            i += 3 + in[i + 1];
            continue;
        }
        uint16_t sz = b & 3;
        if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        uint8_t type = (b >> 2) & 3;
        uint8_t tag = b >> 4;
        uint32_t v = 0;
        for (uint16_t k = 0; k < sz; k++) v |= (uint32_t)in[i + 1 + k] << (8 * k);
        i += 1 + sz;

        if (type == 1 && tag == 0x0) {
            usage_page = v;
        } else if (type == 0 && tag == 0xA) {
            if (depth == 0) {
                copying = (usage_page >= 0xFF00u);
                copy_start = preamble_start;
            }
            depth++;
        } else if (type == 0 && tag == 0xC) {
            if (depth > 0) depth--;
            if (depth == 0) {
                if (copying) {
                    uint16_t chunk = i - copy_start;
                    if (out_len + chunk <= cap) {
                        memcpy(out + out_len, in + copy_start, chunk);
                        out_len += chunk;
                    }
                    copying = false;
                }
                preamble_start = i;
            }
        }
    }
    return out_len;
}

static host_if_t *find_primary_mouse(void) {
    host_if_t *best = nullptr;
    for (auto &h : host_ifs) {
        if (!h.used || h.gone || !h.desc_len) continue;
        if (h.role != HOST_ROLE_MOUSE && h.role != HOST_ROLE_BOOT) continue;
        if (!best || h.ifnum < best->ifnum) best = &h;
    }
    return best;
}

static bool build_ghost_cfg(amv_ghost_cfg_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    s_n_targets = 0;
    memset(s_targets, 0, sizeof s_targets);
    memset(s_target_serials, 0, sizeof s_target_serials);

    host_if_t *pri = find_primary_mouse();
    if (!pri) {
        s_cloaked_dev = nullptr;
        return false;
    }

    s_cloaked_dev = pri->dev;
    s_cloaked_vid = pri->vid;
    s_cloaked_pid = pri->pid;
    cfg->vid = pri->vid;
    cfg->pid = pri->pid;
    snprintf(cfg->manufacturer, sizeof cfg->manufacturer, "%s", pri->manufacturer);
    snprintf(cfg->product, sizeof cfg->product, "%s", pri->product);

    auto add_slot = [&](host_if_t *h, const uint8_t *desc, uint16_t dlen) {
        if (cfg->n_if >= AMV_GHOST_MAX_IFS || !dlen || dlen > AMV_GHOST_DESC_MAX) return;
        uint8_t s = cfg->n_if;
        memcpy(cfg->ifs[s].desc, desc, dlen);
        cfg->ifs[s].desc_len = dlen;
        cfg->ifs[s].mps_in = h->in_mps ? h->in_mps : 64;
        cfg->ifs[s].mps_out = h->out_mps ? h->out_mps : 64;
        cfg->ifs[s].interval_in = h->in_interval ? h->in_interval : 1;
        cfg->ifs[s].interval_out = h->out_interval ? h->out_interval : 1;
        s_targets[s] = h;
        s_target_serials[s] = h->dev_serial;
        cfg->n_if++;
        s_n_targets++;
    };

    // Pass 1: sibling interfaces with vendor usage pages (>= 0xFF00) or Interrupt OUT endpoints.
    for (auto &h : host_ifs) {
        if (!h.used || h.gone || h.dev != pri->dev || &h == pri || !h.desc_len) continue;
        if (desc_has_vendor_usage(h.desc, h.desc_len) || h.has_out) {
            add_slot(&h, h.desc, h.desc_len);
        }
    }
    // Pass 2: other non-mouse sibling interfaces if a slot remains (excluding keyboards with no OUT/vendor).
    if (cfg->n_if < AMV_GHOST_MAX_IFS) {
        for (auto &h : host_ifs) {
            if (!h.used || h.gone || h.dev != pri->dev || &h == pri || !h.desc_len) continue;
            bool already = false;
            for (uint8_t s = 0; s < s_n_targets; s++) if (s_targets[s] == &h) already = true;
            if (already || h.role != HOST_ROLE_OTHER) continue;
            add_slot(&h, h.desc, h.desc_len);
        }
    }
    // Single-interface combined mouse fallback (e.g. Finalmouse ULX): slice vendor collection.
    if (cfg->n_if == 0) {
        uint8_t sliced[AMV_GHOST_DESC_MAX];
        uint16_t slen = extract_vendor_collections(pri->desc, pri->desc_len, sliced, sizeof sliced);
        if (slen > 0) add_slot(pri, sliced, slen);
    }
    return cfg->n_if > 0;
}

static void pump_wait(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        host_task();
        pc_out_poll();
    }
}

static bool target_alive(uint8_t slot) {
    if (slot >= s_n_targets || !s_targets[slot]) return false;
    host_if_t *h = s_targets[slot];
    return h->used && !h->gone && h->dev_serial == s_target_serials[slot];
}

void ghost_set_wanted(bool on) { s_wanted = on; }
bool ghost_is_active(void) { return s_active; }

void ghost_poll(void) {
    host_if_t *pri = find_primary_mouse();
    void *cur_dev = pri ? pri->dev : nullptr;
    uint32_t now = millis();
    bool dev_changed = false;

    if (s_active && s_wanted) {
        if (!cur_dev) {
            if (s_cloaked_dev) {
                if (!s_lost_since) s_lost_since = now ? now : 1;
                else if (now - s_lost_since >= GHOST_LOST_DEBOUNCE_MS) dev_changed = true;
            }
        } else {
            s_lost_since = 0;
            if (cur_dev != s_cloaked_dev) {
                if (s_cloaked_dev && pri->vid == s_cloaked_vid && pri->pid == s_cloaked_pid) {
                    // Brief USB2 bounce with same mouse: rebind targets silently without dropping USB1.
                    amv_ghost_cfg_t tmp;
                    if (build_ghost_cfg(&tmp)) {
                        for (uint8_t s = 0; s < s_n_targets; s++) {
                            if (s_targets[s]) {
                                s_targets[s]->listen = true;
                                host_flush_input(s_targets[s]);
                            }
                        }
                        amv_log("[ghost] rebound %04x:%04x after brief USB2 blip (USB1 kept)\n",
                                s_cloaked_vid, s_cloaked_pid);
                    }
                } else {
                    dev_changed = true;
                }
            }
        }
    } else {
        s_lost_since = 0;
    }

    if (s_wanted != s_active || dev_changed) {
        s_lost_since = 0;
        // Let any pending AMV_CMD_SET_BYPASS reply finish transmitting on EP5 first.
        uint32_t t0 = millis();
        while (!usb_rawhid_tx_idle() && millis() - t0 < 200) {
            host_task();
            pc_out_poll();
        }
        // If activating Ghost Mode, wait for any in-flight OpenMouse JS call to settle cleanly.
        if (s_wanted) {
            mice_set_paused(true);
            uint32_t t1 = millis();
            while (mice_busy() && millis() - t1 < 1500) {
                host_task();
                pc_out_poll();
                mice_poll();
            }
        }

        if (s_wanted) {
            amv_ghost_cfg_t cfg;
            if (build_ghost_cfg(&cfg)) {
                amv_log("[ghost] activating %04x:%04x \"%s\" (%u vendor if)\n",
                        cfg.vid, cfg.pid, cfg.product, cfg.n_if);
                for (uint8_t s = 0; s < s_n_targets; s++) {
                    if (s_targets[s]) {
                        s_targets[s]->listen = true;
                        host_flush_input(s_targets[s]);
                    }
                }
                usb_soft_disconnect();
                usb_ghost_configure_identity(&cfg);
                pump_wait(60);
                usb_soft_connect();
                s_active = true;
            } else if (s_active) {
                // Mouse unplugged while in Ghost Mode: fall back cleanly to native identity.
                amv_log("[ghost] no cloakable mouse attached; restoring native USB identity\n");
                usb_soft_disconnect();
                usb_ghost_clear();
                pump_wait(60);
                usb_soft_connect();
                s_active = false;
                s_n_targets = 0;
                s_cloaked_dev = nullptr;
                s_cloaked_vid = s_cloaked_pid = 0;
            } else {
                // Wanted Ghost Mode, but no mouse with vendor interfaces is attached yet.
                if (pri) s_cloaked_dev = pri->dev;
                s_active = true;
            }
        } else {
            amv_log("[ghost] deactivating; restoring native 16c0:0489 identity\n");
            usb_soft_disconnect();
            usb_ghost_clear();
            pump_wait(60);
            usb_soft_connect();
            s_active = false;
            s_n_targets = 0;
            s_cloaked_dev = nullptr;
            s_cloaked_vid = s_cloaked_pid = 0;
            s_lost_since = 0;
            mice_set_paused(false);
            mice_refresh_now();
        }
    }

    if (!s_active) return;

    // 1. Drain EP0 SET_REPORT from PC -> Mouse (before GET_REPORT to preserve write-then-read order).
    amv_ghost_ep0_set_t sr;
    while (usb_ghost_pop_set_report(&sr)) {
        if (target_alive(sr.slot)) {
            host_set_report_raw(s_targets[sr.slot], sr.report_type, sr.report_id, sr.data, sr.len);
        }
    }

    // 2. Service EP0 GET_REPORT from PC -> Mouse -> PC.
    amv_ghost_ep0_get_t gr;
    if (usb_ghost_take_get_report(&gr)) {
        if (target_alive(gr.slot)) {
            uint8_t buf[256];
            int actual = 0;
            uint16_t cap = gr.len <= sizeof(buf) ? gr.len : (uint16_t)sizeof(buf);
            int e = host_get_report_raw(s_targets[gr.slot], gr.report_type, gr.report_id, buf, cap, &actual);
            usb_ghost_finish_get_report(gr.seq, e == 0 ? buf : nullptr, e == 0 ? actual : -1);
        } else {
            usb_ghost_finish_get_report(gr.seq, nullptr, -1);
        }
    }

    // 3. Interrupt OUT from PC EP6/EP7 -> Mouse Interrupt OUT (preserving exact packet length).
    uint8_t buf[64];
    for (uint8_t s = 0; s < s_n_targets; s++) {
        if (!target_alive(s)) continue;
        int n;
        while ((n = usb_ghost_recv(s, buf, sizeof buf)) > 0) {
            host_send_output_raw(s_targets[s], buf, (uint16_t)n);
        }
    }

    // 4. Mouse Interrupt IN -> PC EP6/EP7 (non-motion replies).
    for (uint8_t s = 0; s < s_n_targets; s++) {
        if (!target_alive(s) || !usb_ghost_tx_ready(s)) continue;
        int n = host_pop_input(s_targets[s], buf, sizeof buf);
        if (n > 0) usb_ghost_send(s, buf, (uint16_t)n);
    }
}
