#include "amv_proto_teensy.h"
#include "../../../common/amv_octant.h"
#include <cstring>
#include <cmath>

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
    rem={}; head=count=0; reboot_pending=false;
    float w[8]; memcpy(w,state.w,sizeof w);
    if (plat.apply) plat.apply(w,false);
}
bool amv_proto_reboot_pending() { return reboot_pending; }
void amv_proto_run_deferred() {
    if (!reboot_pending) return;
    reboot_pending=false;
    if (plat.reboot) plat.reboot();
}
uint8_t __attribute__((weak)) amv_proto_mouse_get_info(const amv_mouse_ops_t *, uint8_t index, amv_mouse_info_t *p) {
    *p={}; p->index=index; return AMV_ST_NO_MOUSE;
}
size_t amv_proto_handle(const uint8_t *input, uint8_t *out) {
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
        r.caps=AMV_CAP_VECTORED_MOUSE|AMV_CAP_MOUSE_HOST;
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
        if (!valid(w)) st(AMV_ST_BAD_VALUE);
        else if (gen!=AMV_GEN_ANY && gen!=state.gen) st(AMV_ST_STALE);
        else { memcpy(state.w,w,32); changed(AMV_SRC_HOST); st(AMV_ST_OK); }
        break;
    }
    case AMV_CMD_SET_BYPASS:
        if (len!=1) reply(AMV_ST_BAD_LEN);
        else { state.bypass=!!p[0]; changed(AMV_SRC_HOST); st(AMV_ST_OK); } break;
    case AMV_CMD_GET_STATS: {
        uint8_t data[56]={}, n=sizeof(amv_stats_payload_t);
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
        reply(AMV_ST_OK,&r,sizeof r); break;
    }
    case AMV_CMD_GET_MOUSE: {
        if (!len) { reply(AMV_ST_BAD_LEN); break; }
        amv_mouse_info_t r={}; r.index=p[0];
        uint8_t s=plat.mouse ? amv_proto_mouse_get_info(plat.mouse,p[0],&r) : AMV_ST_NO_MOUSE;
        reply(s,&r,s==AMV_ST_OK ? sizeof r : 2); break;
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
            state.bypass=0; state.active_slot=0; changed(AMV_SRC_HOST); st(AMV_ST_OK); break;
        }
        if (p[0]>=AMV_NUM_SLOTS) { st(AMV_ST_BAD_SLOT); break; }
        amv_profile_t profile; memcpy(profile.w,state.w,32);
        if (cmd==AMV_CMD_SAVE_PROFILE) {
            if (!s->save(p[0],&profile)) st(AMV_ST_STORE_ERR);
            else { if (state.active_slot!=p[0]) { state.active_slot=p[0]; changed(AMV_SRC_HOST); } st(AMV_ST_OK); }
        } else {
            if (!(s->valid_mask() & (1u<<p[0]))) st(AMV_ST_EMPTY);
            else if (!s->load(p[0],&profile) || !valid(profile.w)) st(AMV_ST_STORE_ERR);
            else { memcpy(state.w,profile.w,32); state.active_slot=p[0]; changed(AMV_SRC_LOAD); st(AMV_ST_OK); }
        }
        break;
    }
    case AMV_CMD_REBOOT: reply(AMV_ST_OK); reboot_pending=true; break;
    default: reply(AMV_ST_BAD_CMD); break;
    }
    return 64;
}
