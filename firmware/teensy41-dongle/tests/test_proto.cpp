#include "amv_proto_teensy.h"
#include <cassert>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <cstdio>
static unsigned applies, reboots;
static void apply(const float *,bool) { ++applies; }
static void reboot() { ++reboots; }
static void id(uint8_t *p) { for(int i=0;i<6;++i) p[i]=i; }
static uint32_t uptime() { return 123; }
static uint8_t mice() { return 2; }
static void stats(uint8_t *p,uint8_t *n) { amv_stats_payload_t s={}; s.sets_ok=42; memcpy(p,&s,sizeof s); *n=sizeof s; }
static amv_profile_t saved[4]; static uint8_t mask;
static bool fail;
static bool save(uint8_t i,const amv_profile_t *p) { if(fail) return false; saved[i]=*p; mask|=1<<i; return true; }
static bool load(uint8_t i,amv_profile_t *p) { *p=saved[i]; return !fail; }
static uint8_t slots() { return mask; }
static bool reset() { if(fail) return false; mask=0; return true; }
static uint8_t out[64],ev[64];
static void request(uint8_t cmd,const void *p=nullptr,uint8_t len=0,uint8_t status=0) {
    uint8_t in[64]={AMV_MAGIC0,AMV_MAGIC1,1,cmd,71,0,len,0};
    if(p) memcpy(in+8,p,len);
    assert(amv_proto_handle(in,out)==64);
    assert(out[0]==AMV_MAGIC0 && out[1]==AMV_MAGIC1 && out[2]==1 && out[3]==cmd && out[4]==71 && out[5]==status);
}
template<class T> static T payload() { T p; memcpy(&p,out+8,sizeof p); return p; }
static void event(uint8_t source) { assert(amv_proto_take_event(ev)); assert(ev[3]==AMV_CMD_EVENT_STATE && ev[4]==0 && ev[6]==39 && ev[46]==source); }
int main() {
    amv_plat_t p={apply,uptime,id,reboot,mice,stats,nullptr,nullptr}; amv_proto_init(&p);
    uint8_t nonce[8]={1,2,3,4,5,6,7,8}; request(AMV_CMD_HELLO,nonce,8);
    auto h=payload<amv_hello_rep_t>(); assert(h.ident==AMV_HELLO_IDENT && h.caps==3 && !memcmp(h.nonce,nonce,8) && h.mac[5]==5);
    request(AMV_CMD_GET_INFO); auto info=payload<amv_info_payload_t>(); assert(info.proto_ver==1 && info.fw_minor==1 && info.chip_rev==0 && info.num_slots==4 && info.has_mouse==2 && info.uptime_ms==123);
    request(AMV_CMD_GET_WEIGHTS); auto initial=payload<amv_state_payload_t>(); assert(initial.gen==1 && initial.w[0]==1);
    amv_set_weights_req_t w={}; w.base_gen=initial.gen; for(unsigned i=0;i<8;++i) w.w[i]=1.5f;
    request(AMV_CMD_SET_WEIGHTS,&w,sizeof w); auto current=payload<amv_state_payload_t>(); assert(current.gen==2 && current.w[0]==1.5f); event(AMV_SRC_HOST);
    request(AMV_CMD_SET_WEIGHTS,&w,sizeof w,AMV_ST_STALE); assert(!memcmp(&current,out+8,sizeof current));
    for(float bad: {0.f,5.01f,NAN,INFINITY}) { w.w[3]=bad; request(AMV_CMD_SET_WEIGHTS,&w,sizeof w,AMV_ST_BAD_VALUE); assert(!memcmp(&current,out+8,sizeof current)); }
    assert(!amv_proto_take_event(ev)); w.w[3]=1.5f; w.base_gen=AMV_GEN_ANY;
    request(AMV_CMD_SET_WEIGHTS,&w,sizeof w); event(AMV_SRC_HOST);
    amv_test_scale_req_t scale={3,0,AMV_TEST_RESET_REM}; request(AMV_CMD_TEST_SCALE,&scale,sizeof scale);
    auto r=payload<amv_test_scale_rep_t>(); assert(r.out_dx==4 && r.out_dy==0 && r.rem_x==0.5f && r.octant==0);
    scale.flags=0; request(AMV_CMD_TEST_SCALE,&scale,sizeof scale); assert(payload<amv_test_scale_rep_t>().out_dx==5);
    uint8_t on=7; request(AMV_CMD_SET_BYPASS,&on,1); event(AMV_SRC_HOST);
    request(AMV_CMD_TEST_SCALE,&scale,sizeof scale); assert(payload<amv_test_scale_rep_t>().out_dx==3);
    request(AMV_CMD_GET_STATS); assert(out[6]==32 && payload<amv_stats_payload_t>().sets_ok==42);
    request(AMV_CMD_GET_MOUSE,&on,1,AMV_ST_NO_MOUSE); assert(out[6]==2 && out[8]==7 && out[9]==0);
    for(uint8_t c: {AMV_CMD_SAVE_PROFILE,AMV_CMD_LOAD_PROFILE,AMV_CMD_GET_SLOTS,AMV_CMD_FACTORY_RESET}) request(c,&on,c==4||c==5 ? 1:0,AMV_ST_BAD_CMD);
    request(127,nullptr,0,AMV_ST_BAD_CMD);
    for(uint8_t c: {AMV_CMD_HELLO,AMV_CMD_SET_WEIGHTS,AMV_CMD_SET_BYPASS,AMV_CMD_TEST_SCALE,AMV_CMD_GET_MOUSE,AMV_CMD_SAVE_PROFILE,AMV_CMD_LOAD_PROFILE}) request(c,nullptr,0,AMV_ST_BAD_LEN);
    uint8_t bad[64]={0,0,1,1,12}; amv_proto_handle(bad,out); assert(out[5]==AMV_ST_BAD_MAGIC && out[4]==12);
    bad[0]=AMV_MAGIC0; bad[1]=AMV_MAGIC1; bad[6]=57; amv_proto_handle(bad,out); assert(out[5]==AMV_ST_BAD_LEN);
    request(AMV_CMD_REBOOT); assert(!reboots && amv_proto_reboot_pending()); amv_proto_run_deferred(); amv_proto_run_deferred(); assert(reboots==1);
    amv_proto_notify_state(AMV_SRC_DEVICE); event(AMV_SRC_DEVICE);
    amv_store_ops_t store={load,save,slots,reset,nullptr,nullptr}; p.store=&store; amv_proto_init(&p);
    uint8_t slot=2; request(AMV_CMD_LOAD_PROFILE,&slot,1,AMV_ST_EMPTY);
    request(AMV_CMD_SAVE_PROFILE,&slot,1); assert(payload<amv_state_payload_t>().active_slot==2); event(AMV_SRC_HOST);
    request(AMV_CMD_GET_SLOTS); assert(out[8]==4);
    request(AMV_CMD_LOAD_PROFILE,&slot,1); event(AMV_SRC_LOAD);
    fail=true; request(AMV_CMD_SAVE_PROFILE,&slot,1,AMV_ST_STORE_ERR); request(AMV_CMD_LOAD_PROFILE,&slot,1,AMV_ST_STORE_ERR); request(AMV_CMD_FACTORY_RESET,nullptr,0,AMV_ST_STORE_ERR); fail=false;
    slot=9; request(AMV_CMD_SAVE_PROFILE,&slot,1,AMV_ST_BAD_SLOT); request(AMV_CMD_LOAD_PROFILE,&slot,1,AMV_ST_BAD_SLOT);
    request(AMV_CMD_FACTORY_RESET); event(AMV_SRC_HOST); assert(mask==0);
    // Replay actual shared fixtures, allowing platform-specific identity/version fields.
    std::ifstream f("../../web/tests/fixtures/frames.json"); assert(f); std::string line; unsigned replayed=0;
    while(std::getline(f,line)) {
        auto pos=line.find("\"request\": \""); if(pos==std::string::npos) continue;
        std::string hex=line.substr(pos+12,128); uint8_t in[64];
        for(unsigned i=0;i<64;++i) in[i]=std::stoul(hex.substr(2*i,2),nullptr,16);
        if(in[3]!=AMV_CMD_HELLO && in[3]!=AMV_CMD_GET_INFO) continue;
        amv_proto_handle(in,out); assert(out[3]==in[3] && out[4]==in[4] && out[5]==0);
        if(in[3]==AMV_CMD_HELLO) { assert(out[6]==24); assert(!memcmp(out+12,in+8,8)); assert(payload<amv_hello_rep_t>().ident==AMV_HELLO_IDENT); }
        else { assert(out[6]==18 && payload<amv_info_payload_t>().num_slots==4); }
        ++replayed;
    }
    assert(replayed==2); assert(applies>1); puts("protocol tests passed");
}
