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
static uint32_t now_ms=123;
static uint32_t uptime() { return now_ms; }
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
static uint8_t fake_count=1, sent_cmd;
static uint16_t fake_vid=0x361d, sent_value;
static uint8_t desc[53];
static const amv_mouse_ops_t mouse_ops={
    []() -> uint8_t { return fake_count; },
    [](uint8_t i, amv_mouse_info_t *p) -> bool {
        if(i>=fake_count) return false;
        *p={}; p->vid=fake_vid; p->pid=0x0100; p->tier=AMV_TIER_GENERIC; return true;
    },
    [](uint8_t, uint16_t *n) -> const uint8_t * { *n=sizeof desc; return desc; },
    [](uint8_t cmd,const uint8_t *p,uint8_t n) -> bool { sent_cmd=cmd; sent_value=n==2 ? p[0]|(p[1]<<8):0; return true; }
};
static_assert(sizeof(amv_mouse_desc_req_t)==4,"descriptor request size");
static void test_mouse() {
    amv_plat_t p={}; p.mouse=&mouse_ops; amv_proto_init(&p);
    uint8_t index=0;
    request(AMV_CMD_GET_MOUSE,&index,1);
    auto info=payload<amv_mouse_info_t>(); assert(out[6]==56 && info.count==1 && info.vid==0x361d);
    index=1; request(AMV_CMD_GET_MOUSE,&index,1,AMV_ST_NO_MOUSE);
    assert(out[6]==2 && out[8]==1 && out[9]==1);
    index=0; request(AMV_CMD_GET_MOUSE_MODEL,&index,1); assert(out[6]==56);
    for(unsigned i=0;i<sizeof desc;++i) desc[i]=i;
    amv_mouse_desc_req_t req={0,0,0};
    request(AMV_CMD_GET_MOUSE_DESC,&req,sizeof req);
    auto chunk=payload<amv_mouse_desc_rep_t>(); assert(chunk.total==53 && chunk.len==48 && chunk.data[47]==47);
    req.offset=48; request(AMV_CMD_GET_MOUSE_DESC,&req,sizeof req);
    chunk=payload<amv_mouse_desc_rep_t>(); assert(chunk.offset==48 && chunk.len==5 && chunk.data[4]==52 && chunk.data[5]==0);
    for(auto offset: {53,54,65535}) {
        req.offset=offset; request(AMV_CMD_GET_MOUSE_DESC,&req,sizeof req);
        assert(payload<amv_mouse_desc_rep_t>().len==0);
    }
    req.index=1; request(AMV_CMD_GET_MOUSE_DESC,&req,sizeof req,AMV_ST_NO_MOUSE); assert(out[6]==0);
    request(AMV_CMD_GET_MOUSE_DESC,nullptr,0,AMV_ST_BAD_LEN);
    amv_proto_notify_mouse(AMV_MEV_ATTACHED,42);
    fake_count=0; amv_proto_notify_mouse(AMV_MEV_REMOVED,42);
    assert(amv_proto_take_event(ev) && ev[3]==AMV_CMD_EVENT_MOUSE && ev[6]==4 && ev[8]==1 && ev[9]==AMV_MEV_ATTACHED && ev[10]==42);
    assert(amv_proto_take_event(ev) && ev[8]==0 && ev[9]==AMV_MEV_REMOVED && ev[10]==42);
    assert(!amv_proto_take_event(ev));
    request(AMV_CMD_GET_MOUSE,&index,1,AMV_ST_NO_MOUSE); assert(out[8]==0 && out[9]==0);
}
static void test_settings() {
    fake_count=1;
    amv_plat_t p={}; p.mouse=&mouse_ops; p.uptime_ms=uptime; amv_proto_init(&p);
    amv_options_req_t opt={0,AMV_SET_POLLING};
    request(AMV_CMD_GET_MOUSE_OPTIONS,&opt,sizeof opt);
    auto options=payload<amv_options_rep_t>();
    assert(out[6]==16 && options.n==5 && options.kind==AMV_OPT_LIST && options.unit==AMV_UNIT_HZ);
    unsigned i=0; for(auto hz:{500,1000,2000,4000,8000}) assert(options.values[i++]==hz);
    auto start=[](uint8_t cmd,const void *p,uint8_t n) {
        uint8_t in[64]={AMV_MAGIC0,AMV_MAGIC1,1,cmd,33,0,n,0}; memcpy(in+8,p,n);
        assert(amv_proto_handle(in,out)==0);
    };
    auto finish=[]() {
        amv_proto_finalmouse_reply(3,1600); amv_proto_finalmouse_reply(4,2000); amv_proto_finalmouse_reply(5,3800);
        assert(amv_proto_poll_mouse(out) && out[5]==AMV_ST_OK && out[4]==33);
    };
    uint8_t index=0; start(AMV_CMD_GET_MOUSE_SETTINGS,&index,1); assert(sent_cmd==96);
    request(AMV_CMD_GET_MOUSE_SETTINGS,&index,1,AMV_ST_BUSY);
    assert(!amv_proto_poll_mouse(out)); finish();
    auto settings=payload<amv_settings_rep_t>();
    assert(settings.count==3 && settings.s[0].value==2000 && settings.s[1].value==1600 && settings.s[1].flags==AMV_SETF_READABLE && settings.s[2].value==50);
    amv_setting_set_req_t set={0,AMV_SET_POLLING,4000};
    start(AMV_CMD_SET_MOUSE_SETTING,&set,sizeof set); assert(sent_cmd==17 && sent_value==4000);
    amv_proto_finalmouse_reply(17,4000); assert(!amv_proto_poll_mouse(out) && sent_cmd==96);
    finish(); assert(payload<amv_settings_rep_t>().s[0].value==2000);
    start(AMV_CMD_SET_MOUSE_POLLING,&set,sizeof set);
    amv_proto_finalmouse_reply(17,4000); assert(!amv_proto_poll_mouse(out)); finish();
    assert(payload<amv_mouse_info_t>().device_hz==2000);
    start(AMV_CMD_GET_MOUSE_SETTINGS,&index,1); now_ms+=1000;
    assert(amv_proto_poll_mouse(out) && out[5]==AMV_ST_DEVICE_ERR);
    set.value=750; request(AMV_CMD_SET_MOUSE_SETTING,&set,sizeof set,AMV_ST_BAD_VALUE);
    fake_vid=1; request(AMV_CMD_GET_MOUSE_SETTINGS,&index,1,AMV_ST_UNSUPPORTED);
    request(AMV_CMD_GET_MOUSE_OPTIONS,&opt,sizeof opt,AMV_ST_UNSUPPORTED); fake_vid=0x361d;
}
int main() {
    test_mouse();
    test_settings(); now_ms=123;
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
    amv_profile_t boot; for (auto &w : boot.w) w = 1.75f;
    amv_proto_restore(3, &boot, true);
    amv_state_payload_t snapshot; amv_proto_snapshot(&snapshot);
    assert(snapshot.active_slot == 3 && snapshot.bypass && snapshot.w[0] == 1.75f);
    uint8_t device[64] = {AMV_MAGIC0, AMV_MAGIC1, AMV_PROTO_VERSION, AMV_CMD_SET_BYPASS, 0, 0, 1, 0};
    uint32_t before = snapshot.gen;
    amv_proto_handle(device, out, AMV_SRC_DEVICE);
    amv_proto_snapshot(&snapshot); assert(snapshot.gen == before + 1 && !snapshot.bypass);
    event(AMV_SRC_DEVICE);
    amv_set_weights_req_t weights = {}; weights.base_gen = AMV_GEN_ANY;
    for (unsigned i=0; i<8; ++i) weights.w[i] = 2;
    device[3] = AMV_CMD_SET_WEIGHTS; device[6] = sizeof weights;
    memcpy(device + 8, &weights, sizeof weights);
    amv_proto_handle(device, out, AMV_SRC_DEVICE);
    amv_proto_snapshot(&snapshot); assert(snapshot.gen == before + 2 && snapshot.w[0] == 2);
    event(AMV_SRC_DEVICE);
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
