#include "amv_proto_teensy.h"
#include "../../../common/amv_octant.h"
#include <cstring>
#include <cmath>
#include "amv_catalog.h"

static amv_plat_t plat;
static amv_state_payload_t state;
static amv_octant_state_t rem;
static bool reboot_pending;
static uint8_t events[32][64];
static unsigned head, count;

static void frame(uint8_t *out, uint8_t cmd, uint8_t seq, uint8_t status,
                  const void *p = nullptr, uint8_t len = 0) {
    memset(out, 0, 64);
    amv_hdr_t h = {AMV_MAGIC0, AMV_MAGIC1, AMV_PROTO_VERSION, cmd, seq, status, len, 0};
    memcpy(out, &h, sizeof h);
    if (len) memcpy(out + 8, p, len);
}
// GET_STATS counters (amv_stats_payload_t, the ESP32's layout)
static amv_stats_payload_t counters;
// A mouse write whose reply waits for the driver (one at a time).
static struct { bool active; uint8_t cmd, seq, index; } deferred;
// TEST_SELF_CHANGE: one pending device-side change.
static struct { bool pending; uint8_t octant; float value; uint32_t due; } self_change;

void amv_proto_count_event(bool sent) { if (sent) counters.events_sent++; else counters.events_dropped++; }

static bool valid(const float *w) {
    for (unsigned i=0; i<8; ++i)
        if (!std::isfinite(w[i]) || w[i] < float(AMV_WEIGHT_MIN) || w[i] > float(AMV_WEIGHT_MAX)) return false;
    return true;
}
void amv_proto_notify_state(uint8_t source) {
    if (count == 32) return; // bounded FIFO: preserve earlier events on overflow
    amv_event_payload_t ev = {state, source};
    frame(events[(head+count)%32], AMV_CMD_EVENT_STATE, 0, AMV_ST_OK, &ev, sizeof ev);
    ++count;
}
static void changed(uint8_t source) {
    ++state.gen;
    float w[8]; memcpy(w, state.w, sizeof w);
    if (plat.apply) plat.apply(w, state.bypass != 0);
    amv_proto_notify_state(source);
}
bool amv_proto_take_event(uint8_t *out) {
    if (!count) return false;
    memcpy(out, events[head], 64); head=(head+1)%32; --count; return true;
}
void amv_proto_init(const amv_plat_t *p) {
    plat = p ? *p : amv_plat_t{};
    state = {}; state.gen=1;
    for (unsigned i=0;i<8;++i) state.w[i]=1;
    rem={}; head=count=0; reboot_pending=false; counters={}; deferred={}; self_change={};
    float w[8]; memcpy(w,state.w,sizeof w);
    if (plat.apply) plat.apply(w,false);
}
bool amv_proto_reboot_pending() { return reboot_pending; }
void amv_proto_run_deferred() {
    if (!reboot_pending) return;
    reboot_pending=false;
    if (plat.reboot) plat.reboot();
}
void amv_proto_notify_mouse(uint8_t change, uint16_t uid) {
    if (count == 32) return;
    amv_mouse_event_t ev = {uint8_t(plat.mouse && plat.mouse->count ? plat.mouse->count() : 0), change, uid};
    frame(events[(head+count)%32], AMV_CMD_EVENT_MOUSE, 0, AMV_ST_OK, &ev, sizeof ev);
    ++count;
}
// Reply to a mouse write: the mouse's entry (SET_MOUSE_CPI / _POLLING) or its settings list (SET_MOUSE_SETTING),
// sent even when the write was refused so a client can resync, as the ESP32 does.
static void amv_proto_mouse_reply(uint8_t *out, uint8_t status) {
    if (deferred.cmd==AMV_CMD_SET_MOUSE_SETTING) {
        amv_settings_rep_t r={}; r.index=deferred.index;
        uint8_t g=plat.mouse->settings(deferred.index,&r);
        uint8_t s=status!=AMV_ST_OK ? status : g;
        frame(out,deferred.cmd,deferred.seq,s,&r,s==AMV_ST_NO_MOUSE || g!=AMV_ST_OK ? 0 : uint8_t(4+4*r.count));
    } else {
        amv_mouse_info_t r={}; r.index=deferred.index;
        plat.mouse->info(deferred.index,&r);
        frame(out,deferred.cmd,deferred.seq,status,&r,status==AMV_ST_NO_MOUSE ? 2 : sizeof r);
    }
}
bool amv_proto_poll_mouse(uint8_t *out) {
    uint8_t status;
    if (!deferred.active || !plat.mouse || !plat.mouse->write_done(&status)) return false;
    deferred.active=false;
    amv_proto_mouse_reply(out,status);
    return true;
}
void amv_proto_tick() {
    if (!self_change.pending || !plat.uptime_ms || int32_t(plat.uptime_ms()-self_change.due)<0) return;
    self_change.pending=false;
    float w[8]; memcpy(w,state.w,sizeof w);
    w[self_change.octant]=self_change.value;
    if (valid(w)) { memcpy(state.w,w,sizeof w); changed(AMV_SRC_DEVICE); }
}
size_t amv_proto_handle(const uint8_t *input, uint8_t *out, uint8_t source) {
    uint8_t in[64]; memcpy(in,input,64);
    const uint8_t cmd=in[3], seq=in[4], len=in[6];
    const uint8_t *p=in+8;
    auto reply=[&](uint8_t s, const void *data=nullptr, uint8_t n=0) { frame(out,cmd,seq,s,data,n); };
    auto st=[&](uint8_t s) { reply(s,&state,sizeof state); };
    if (in[0]!=AMV_MAGIC0 || in[1]!=AMV_MAGIC1) { reply(AMV_ST_BAD_MAGIC); return 64; }
    if (len>56) { reply(AMV_ST_BAD_LEN); return 64; }
    switch(cmd) {
    case AMV_CMD_HELLO: {
        if (len!=8) { reply(AMV_ST_BAD_LEN); break; }
        amv_hello_rep_t r={}; r.ident=AMV_HELLO_IDENT; memcpy(r.nonce,p,8);
        r.proto_ver=AMV_PROTO_VERSION; r.fw_major=AMV_TEENSY_FW_MAJOR; r.fw_minor=AMV_TEENSY_FW_MINOR; r.fw_patch=AMV_TEENSY_FW_PATCH;
        r.caps=AMV_CAP_VECTORED_MOUSE|AMV_CAP_MOUSE_HOST|AMV_CAP_PRO;
        if (plat.unit_id) plat.unit_id(r.mac);
        reply(AMV_ST_OK,&r,sizeof r); break;
    }
    case AMV_CMD_GET_INFO: {
        amv_info_payload_t r={}; r.proto_ver=AMV_PROTO_VERSION; r.fw_major=AMV_TEENSY_FW_MAJOR; r.fw_minor=AMV_TEENSY_FW_MINOR; r.fw_patch=AMV_TEENSY_FW_PATCH; r.num_slots=AMV_NUM_SLOTS;
        if (plat.unit_id) plat.unit_id(r.mac);
        if (plat.uptime_ms) r.uptime_ms=plat.uptime_ms();
        if (plat.mouse_count) r.has_mouse=plat.mouse_count();
        reply(AMV_ST_OK,&r,sizeof r); break;
    }
    case AMV_CMD_GET_WEIGHTS: st(AMV_ST_OK); break;
    case AMV_CMD_SET_WEIGHTS: {
        if (len!=36) { reply(AMV_ST_BAD_LEN); break; }
        uint32_t gen; float w[8]; memcpy(&gen,p,4); memcpy(w,p+4,32);
        if (!valid(w)) { counters.bad_value_rejects++; st(AMV_ST_BAD_VALUE); }
        else if (gen!=AMV_GEN_ANY && gen!=state.gen) { counters.stale_rejects++; st(AMV_ST_STALE); }
        else { memcpy(state.w,w,32); changed(source); counters.sets_ok++; st(AMV_ST_OK); }
        break;
    }
    case AMV_CMD_SET_BYPASS:
        if (len!=1) reply(AMV_ST_BAD_LEN);
        else { state.bypass=!!p[0]; changed(source); st(AMV_ST_OK); } break;
    case AMV_CMD_GET_STATS: {
        uint8_t data[56]={}, n=sizeof(amv_stats_payload_t);
        memcpy(data,&counters,sizeof counters);
        if (plat.get_stats) plat.get_stats(data,&n);
        if (n>56) reply(AMV_ST_BAD_LEN); else reply(AMV_ST_OK,data,n);
        break;
    }
    case AMV_CMD_TEST_SCALE: {
        if (len!=5) { reply(AMV_ST_BAD_LEN); break; }
        amv_test_scale_req_t req; memcpy(&req,p,sizeof req);
        if (req.flags & AMV_TEST_RESET_REM) rem={};
        float w[8]; for (unsigned i=0;i<8;++i) w[i]=state.bypass ? 1 : state.w[i];
        int32_t x,y; uint8_t oct;
        amv_octant_scale(w,&rem,req.dx,req.dy,&x,&y,&oct);
        amv_test_scale_rep_t r={x,y,rem.rem_x,rem.rem_y,oct};
        counters.test_scales++;
        reply(AMV_ST_OK,&r,sizeof r); break;
    }
    case AMV_CMD_TEST_SELF_CHANGE: {
        if (len!=sizeof(amv_test_self_change_req_t)) { reply(AMV_ST_BAD_LEN); break; }
        amv_test_self_change_req_t req; memcpy(&req,p,sizeof req);
        if (req.octant>7) { reply(AMV_ST_BAD_VALUE); break; }
        if (self_change.pending) { reply(AMV_ST_BUSY); break; }
        self_change.pending=true; self_change.octant=req.octant; self_change.value=req.value;
        self_change.due=(plat.uptime_ms?plat.uptime_ms():0)+(req.delay_ms?req.delay_ms:1);
        reply(AMV_ST_OK); break;
    }
    // ---- mice: the same replies as the ESP32-P4 (amv_proto.c) ----
    case AMV_CMD_GET_MOUSE: {
        if (!len) { reply(AMV_ST_BAD_LEN); break; }
        amv_mouse_info_t r={}; r.index=p[0];
        uint8_t s=plat.mouse ? plat.mouse->info(p[0],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? sizeof r : 2); break;
    }
    case AMV_CMD_GET_MOUSE_CAPS: {
        if (!len) { reply(AMV_ST_BAD_LEN); break; }
        amv_mouse_caps_t r={}; r.index=p[0];
        uint8_t s=plat.mouse ? plat.mouse->caps(p[0],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? sizeof r : 0); break;
    }
    case AMV_CMD_GET_MOUSE_DESC: {
        if (len < sizeof(amv_mouse_desc_req_t)) { reply(AMV_ST_BAD_LEN); break; }
        amv_mouse_desc_req_t req; memcpy(&req,p,sizeof req);
        amv_mouse_desc_rep_t r={}; r.index=req.index; r.offset=req.offset;
        uint8_t s=plat.mouse ? plat.mouse->desc(req.index,req.offset,&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? sizeof r : 0); break;
    }
    case AMV_CMD_GET_MOUSE_MODEL: {
        if (!len) { reply(AMV_ST_BAD_LEN); break; }
        amv_mouse_model_t r={}; r.index=p[0];
        uint8_t s=plat.mouse ? plat.mouse->model(p[0],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? sizeof r : 0); break;
    }
    case AMV_CMD_GET_MOUSE_SETTINGS: {
        if (!len) { reply(AMV_ST_BAD_LEN); break; }
        amv_settings_rep_t r={}; r.index=p[0];
        uint8_t s=plat.mouse ? plat.mouse->settings(p[0],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? uint8_t(4+4*r.count) : 0); break;
    }
    case AMV_CMD_GET_MOUSE_OPTIONS: {
        if (len < sizeof(amv_options_req_t)) { reply(AMV_ST_BAD_LEN); break; }
        amv_options_rep_t r={}; r.index=p[0]; r.setting=p[1];
        uint8_t s=plat.mouse ? plat.mouse->options(p[0],p[1],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? uint8_t(6+2*r.n) : 0); break;
    }
    case AMV_CMD_SET_MOUSE_CPI:
    case AMV_CMD_SET_MOUSE_POLLING:
    case AMV_CMD_SET_MOUSE_SETTING: {
        if (len < 4) { reply(AMV_ST_BAD_LEN); break; }
        uint8_t index=p[0], setting=cmd==AMV_CMD_SET_MOUSE_CPI ? AMV_SET_CPI : cmd==AMV_CMD_SET_MOUSE_POLLING ? AMV_SET_POLLING : p[1];
        uint16_t value; memcpy(&value,p+2,2);
        if (!plat.mouse) { reply(AMV_ST_NO_MOUSE); break; }
        if (deferred.active) { reply(AMV_ST_BUSY); break; }
        uint8_t s=plat.mouse->write(index,setting,value);
        if (s==AMV_ST_PENDING) { deferred.active=true; deferred.cmd=cmd; deferred.seq=seq; deferred.index=index; return 0; }
        deferred.cmd=cmd; deferred.seq=seq; deferred.index=index;
        amv_proto_mouse_reply(out,s);
        break;
    }
    case AMV_CMD_SAVE_PROFILE: case AMV_CMD_LOAD_PROFILE:
    case AMV_CMD_GET_SLOTS: case AMV_CMD_FACTORY_RESET: {
        const auto *s=plat.store;
        if ((cmd==AMV_CMD_SAVE_PROFILE || cmd==AMV_CMD_LOAD_PROFILE) && len!=1) { reply(AMV_ST_BAD_LEN); break; }
        if (!s) { reply(AMV_ST_BAD_CMD); break; }
        if (cmd==AMV_CMD_GET_SLOTS) { uint8_t m=s->valid_mask(); reply(AMV_ST_OK,&m,1); break; }
        if (cmd==AMV_CMD_FACTORY_RESET) {
            if (!s->factory_reset()) { st(AMV_ST_STORE_ERR); break; }
            for (unsigned i=0;i<8;++i) state.w[i]=1;
            state.bypass=0; state.active_slot=0; changed(source); st(AMV_ST_OK); break;
        }
        if (p[0]>=AMV_NUM_SLOTS) { st(AMV_ST_BAD_SLOT); break; }
        amv_profile_t profile; memcpy(profile.w,state.w,32);
        if (cmd==AMV_CMD_SAVE_PROFILE) {
            if (!s->save(p[0],&profile)) st(AMV_ST_STORE_ERR);
            else { counters.saves++; if (state.active_slot!=p[0]) { state.active_slot=p[0]; changed(source); } st(AMV_ST_OK); }
        } else {
            if (!(s->valid_mask() & (1u<<p[0]))) st(AMV_ST_EMPTY);
            else if (!s->load(p[0],&profile) || !valid(profile.w)) st(AMV_ST_STORE_ERR);
            else { memcpy(state.w,profile.w,32); state.active_slot=p[0]; changed(AMV_SRC_LOAD); counters.loads++; st(AMV_ST_OK); }
        }
        break;
    }
    case AMV_CMD_REBOOT: reply(AMV_ST_OK); reboot_pending=true; break;
    default: reply(AMV_ST_BAD_CMD); break;
    }
    return 64;
}

void amv_proto_snapshot(amv_state_payload_t *out) { *out = state; }
void amv_proto_restore(uint8_t slot, const amv_profile_t *p, bool bypass) {
    if (slot >= AMV_NUM_SLOTS || !valid(p->w)) return;
    state.active_slot = slot; state.bypass = bypass;
    memcpy(state.w, p->w, sizeof state.w);
    if (plat.apply) plat.apply(p->w, bypass);
}
