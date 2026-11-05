/* Port of firmware/esp32p4-dongle/components/amv_js/amv_js.c to the Teensy 4.1 (no RTOS); see js_engine.h. */
#include "js_engine.h"
#include <stdio.h>
#include <string.h>
#include "psram_heap.h"
#include "quickjs.h"

#define MAX_SESSIONS     8
#define JS_STACK_LIMIT   (64 * 1024)     /* the engine runs on the main stack (DTCM, about 350 KB free) */
#define CALL_TIMEOUT_MS  15000           /* one driver call: OpenMouse retries a sleeping receiver for seconds */
#define LOAD_TIMEOUT_MS  30000
#define IO_MAX           128             /* report bytes per exchange */
#define HEAP_BYTES       (7u * 1024u * 1024u)

extern volatile uint32_t systick_millis_count;
static inline uint32_t now_ms(void) { return systick_millis_count; }
void amv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* main.cpp */
void amv_js_idle_hook(void);   /* main.cpp: keeps USB host enumeration going while the bundle loads */

/* generated/openmouse.bc, embedded by openmouse_bc.S */
extern const uint8_t openmouse_bc_start[];
extern const uint8_t openmouse_bc_end[];

typedef struct {
    int used;
    const js_transport_t *t;
    void *h;
} session_t;

static JSRuntime *s_rt;
static JSContext *s_ctx;
static JSValue s_amv;                        /* globalThis.amv, set by js/bridge.js */
static session_t s_sess[MAX_SESSIONS + 1];   /* index = session id; 0 unused */
static size_t s_used, s_peak;
static uint32_t s_load_ms, s_calls, s_call_ms_last, s_call_ms_max;
static bool s_ready, s_failed;
static int s_log = 1;
static uint32_t s_rand = 0x9E3779B9u;

/* the running call */
static bool s_busy;
static int s_call_sid, s_call_op;
static JSValue s_call_val;
static uint32_t s_call_start;

/* ---------- allocator: PSRAM, counted ---------- */

static void count_add(size_t n) { s_used += n; if (s_used > s_peak) s_peak = s_used; }
static void *js_calloc_cb(void *o, size_t count, size_t size) { (void)o; void *p = ph_calloc(count, size); count_add(ph_usable(p)); return p; }
static void *js_malloc_cb(void *o, size_t size) { (void)o; void *p = ph_malloc(size); count_add(ph_usable(p)); return p; }
static void js_free_cb(void *o, void *p) { (void)o; if (!p) return; s_used -= ph_usable(p); ph_free(p); }
static void *js_realloc_cb(void *o, void *p, size_t size)
{
    if (!size) { js_free_cb(o, p); return NULL; }
    size_t old = ph_usable(p);
    void *np = ph_realloc(p, size);
    if (np) { s_used -= old; count_add(ph_usable(np)); }
    return np;
}
static size_t js_usable_cb(const void *p) { return ph_usable(p); }

static const JSMallocFunctions s_mf = {
    .js_calloc = js_calloc_cb, .js_malloc = js_malloc_cb, .js_free = js_free_cb,
    .js_realloc = js_realloc_cb, .js_malloc_usable_size = js_usable_cb,
};

/* ---------- errors ---------- */

static void log_exception(const char *what)
{
    JSValue e = JS_GetException(s_ctx);
    const char *msg = JS_ToCString(s_ctx, e);
    const char *stack = NULL;
    JSValue sv = JS_UNDEFINED;
    if (JS_IsError(e)) {
        sv = JS_GetPropertyStr(s_ctx, e, "stack");
        if (!JS_IsUndefined(sv)) stack = JS_ToCString(s_ctx, sv);
    }
    if (s_log) amv_log("[js] %s: %s%s%s\n", what, msg ? msg : "(no message)", stack ? "\n" : "", stack ? stack : "");
    if (stack) JS_FreeCString(s_ctx, stack);
    if (msg) JS_FreeCString(s_ctx, msg);
    JS_FreeValue(s_ctx, sv);
    JS_FreeValue(s_ctx, e);
}

/* ---------- natives: globalThis.__amv (see js/prelude.js, js/hid_device.js) ---------- */

static session_t *session_arg(JSContext *c, JSValueConst v)
{
    int32_t sid;
    if (JS_ToInt32(c, &sid, v) || sid < 1 || sid > MAX_SESSIONS || !s_sess[sid].used) return NULL;
    return &s_sess[sid];
}

static JSValue n_now(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewFloat64(c, (double)now_ms());
}

static JSValue n_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t lvl = 1;
    if (argc > 0) JS_ToInt32(c, &lvl, argv[0]);
    const char *s = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    if (!s) return JS_UNDEFINED;
    if ((lvl >= 2 && s_log >= 1) || s_log >= 2) amv_log("[js] %s\n", s);
    JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

static JSValue n_random(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(c, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab)) return ab;
    size_t total;
    uint8_t *p = JS_GetArrayBuffer(c, &total, ab);
    if (p && off + len <= total) {
        s_rand ^= *(volatile uint32_t *)0xE0001004;      /* DWT cycle counter */
        for (size_t i = 0; i < len; i++) {
            s_rand ^= s_rand << 13; s_rand ^= s_rand >> 17; s_rand ^= s_rand << 5;
            p[off + i] = (uint8_t)s_rand;
        }
    }
    JS_FreeValue(c, ab);
    return JS_UNDEFINED;
}

/* send(sid, x, type, reportId, ArrayBuffer): type 2 output, 3 feature */
static JSValue n_send(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    session_t *s = session_arg(c, argv[0]);
    int32_t x, type, id;
    size_t len;
    if (!s || JS_ToInt32(c, &x, argv[1]) || JS_ToInt32(c, &type, argv[2]) || JS_ToInt32(c, &id, argv[3]))
        return JS_ThrowTypeError(c, "send: bad arguments");
    const uint8_t *d = JS_GetArrayBuffer(c, &len, argv[4]);
    if (!d) return JS_EXCEPTION;
    if (len > IO_MAX) return JS_ThrowRangeError(c, "send: %u bytes, at most %d", (unsigned)len, IO_MAX);
    int e = s->t->send(s->h, x, (uint8_t)type, (uint8_t)id, d, (uint16_t)len);
    if (e) return JS_ThrowInternalError(c, "Failed to write the %s report (error %d).", type == 3 ? "feature" : "output", e);
    return JS_UNDEFINED;
}

/* recv(sid, x, reportId, length) -> ArrayBuffer without the report ID */
static JSValue n_recv(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    session_t *s = session_arg(c, argv[0]);
    int32_t x, id, len;
    if (!s || JS_ToInt32(c, &x, argv[1]) || JS_ToInt32(c, &id, argv[2]) || JS_ToInt32(c, &len, argv[3]))
        return JS_ThrowTypeError(c, "recv: bad arguments");
    if (len < 1) len = 1;
    if (len > IO_MAX) len = IO_MAX;
    uint8_t buf[IO_MAX];
    int got = 0;
    int e = s->t->recv_feature(s->h, x, (uint8_t)id, buf, (uint16_t)len, &got);
    if (e) return JS_ThrowInternalError(c, "Failed to receive the feature report (error %d).", e);
    return JS_NewArrayBufferCopy(c, buf, got > 0 ? (size_t)got : 0);
}

static const JSCFunctionListEntry s_natives[] = {
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("log", 2, n_log),
    JS_CFUNC_DEF("random", 1, n_random),
    JS_CFUNC_DEF("send", 5, n_send),
    JS_CFUNC_DEF("recv", 4, n_recv),
};

/* ---------- event loop ---------- */

static void run_jobs(void)
{
    JSContext *c;
    while (JS_IsJobPending(s_rt)) {
        if (JS_ExecutePendingJob(s_rt, &c) < 0) log_exception("job");
    }
}

static JSValue call_amv(const char *fn, int argc, JSValueConst *argv)
{
    JSValue f = JS_GetPropertyStr(s_ctx, s_amv, fn);
    JSValue r = JS_Call(s_ctx, f, s_amv, argc, argv);
    JS_FreeValue(s_ctx, f);
    return r;
}

/* Input reports queued by the host since the last look, delivered as WebHID inputreport events. */
static void deliver_inputs(void)
{
    uint8_t raw[72];
    for (int sid = 1; sid <= MAX_SESSIONS; sid++) {
        session_t *s = &s_sess[sid];
        if (!s->used || !s->t->poll_input) continue;
        int x, n;
        for (int guard = 0; guard < 32 && (n = s->t->poll_input(s->h, &x, raw, sizeof raw)) > 0; guard++) {
            JSValue args[3] = { JS_NewInt32(s_ctx, sid), JS_NewInt32(s_ctx, x), JS_NewArrayBufferCopy(s_ctx, raw, (size_t)n) };
            JSValue r = call_amv("input", 3, args);
            if (JS_IsException(r)) log_exception("input");
            JS_FreeValue(s_ctx, r);
            JS_FreeValue(s_ctx, args[2]);
        }
    }
}

/* Due timers; returns ms to the next one, or -1. */
static int pump_timers(void)
{
    if (!JS_IsObject(s_amv)) return -1;
    JSValue r = call_amv("pump", 0, NULL);
    int32_t ms = -1;
    if (JS_IsException(r)) log_exception("pump");
    else JS_ToInt32(s_ctx, &ms, r);
    JS_FreeValue(s_ctx, r);
    return ms;
}

/* One step of driving a promise: returns 1 settled (value in *out, or JS_EXCEPTION with the exception
 * pending), 0 still pending. Takes ownership of `v` only when it settles. */
static int step_value(JSValue v, JSValue *out)
{
    run_jobs();
    if (!JS_IsPromise(v)) { *out = v; return 1; }
    JSPromiseStateEnum st = JS_PromiseState(s_ctx, v);
    if (st == JS_PROMISE_PENDING) {
        deliver_inputs();
        pump_timers();
        run_jobs();
        st = JS_PromiseState(s_ctx, v);
        if (st == JS_PROMISE_PENDING) return 0;
    }
    JSValue r = JS_PromiseResult(s_ctx, v);
    JS_FreeValue(s_ctx, v);
    *out = st == JS_PROMISE_FULFILLED ? r : JS_Throw(s_ctx, r);
    return 1;
}

/* Blocking version, for loading the bundle at boot. */
static JSValue await_value(JSValue v, int timeout_ms)
{
    if (JS_IsException(v)) return v;
    uint32_t start = now_ms();
    for (;;) {
        JSValue r;
        if (step_value(v, &r)) return r;
        if ((int)(now_ms() - start) > timeout_ms) {
            JS_FreeValue(s_ctx, v);
            return JS_ThrowInternalError(s_ctx, "timed out after %d ms", timeout_ms);
        }
        amv_js_idle_hook();
    }
}

/* ---------- status object -> C ---------- */

static int get_int(JSValueConst o, const char *k, int def)
{
    JSValue v = JS_GetPropertyStr(s_ctx, o, k);
    double d;
    int r = def;
    if (JS_IsNumber(v) && JS_ToFloat64(s_ctx, &d, v) == 0) r = (int)d;
    JS_FreeValue(s_ctx, v);
    return r;
}

static bool get_bool(JSValueConst o, const char *k)
{
    JSValue v = JS_GetPropertyStr(s_ctx, o, k);
    bool b = JS_ToBool(s_ctx, v) > 0;
    JS_FreeValue(s_ctx, v);
    return b;
}

static void get_str(JSValueConst o, const char *k, char *out, size_t cap)
{
    JSValue v = JS_GetPropertyStr(s_ctx, o, k);
    const char *s = JS_IsString(v) ? JS_ToCString(s_ctx, v) : NULL;
    snprintf(out, cap, "%s", s ? s : "");
    if (s) JS_FreeCString(s_ctx, s);
    JS_FreeValue(s_ctx, v);
}

static int get_u16s(JSValueConst o, const char *k, uint16_t *out, int max)
{
    JSValue v = JS_GetPropertyStr(s_ctx, o, k);
    int64_t len = 0;
    int n = 0;
    if (JS_IsArray(v) && JS_GetLength(s_ctx, v, &len) == 0) {
        for (int64_t i = 0; i < len && n < max; i++) {
            JSValue e = JS_GetPropertyUint32(s_ctx, v, (uint32_t)i);
            double d;
            if (JS_ToFloat64(s_ctx, &d, e) == 0 && d > 0 && d <= 65535) out[n++] = (uint16_t)d;
            JS_FreeValue(s_ctx, e);
        }
    }
    JS_FreeValue(s_ctx, v);
    return n;
}

static void read_status(JSValueConst o, js_status_t *st)
{
    memset(st, 0, sizeof *st);
    get_str(o, "brand", st->brand, sizeof st->brand);
    get_str(o, "name", st->name, sizeof st->name);
    get_str(o, "firmware", st->firmware, sizeof st->firmware);
    st->cpi = (uint16_t)get_int(o, "cpi", 0);
    st->hz = (uint16_t)get_int(o, "hz", 0);
    st->n_rates = (uint8_t)get_u16s(o, "rates", st->rates, 8);
    st->battery = (int16_t)get_int(o, "battery", -1);
    st->charging = (uint8_t)get_int(o, "charging", 0);
    st->link = (uint8_t)get_int(o, "link", 0);
    st->cpi_min = (uint16_t)get_int(o, "cpiMin", 0);
    st->cpi_max = (uint16_t)get_int(o, "cpiMax", 0);
    st->cpi_step = (uint16_t)get_int(o, "cpiStep", 0);
    st->lod = (uint8_t)get_int(o, "lod", 0);
    uint16_t lods[3];
    st->n_lods = (uint8_t)get_u16s(o, "lods", lods, 3);
    for (int i = 0; i < st->n_lods; i++) st->lods[i] = (uint8_t)lods[i];
    st->motion_sync = (int8_t)get_int(o, "motionSync", -1);
    st->can_cpi = get_bool(o, "canCpi");
    st->can_rate = get_bool(o, "canRate");
    st->can_lod = get_bool(o, "canLod");
    st->can_motion_sync = get_bool(o, "canMotionSync");
}

/* ---------- public API ---------- */

bool js_begin(void)
{
    uint32_t t0 = now_ms();
    if (!ph_init(HEAP_BYTES) && !ph_init(HEAP_BYTES / 2)) {
        amv_log("[js] no PSRAM: OpenMouse drivers are off, mice stay generic\n");
        s_failed = true;
        return false;
    }
    s_rt = JS_NewRuntime2(&s_mf, NULL);
    if (!s_rt) { s_failed = true; return false; }
    JS_SetMaxStackSize(s_rt, JS_STACK_LIMIT);
    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx) { s_failed = true; return false; }

    JSValue g = JS_GetGlobalObject(s_ctx);
    JSValue n = JS_NewObject(s_ctx);
    JS_SetPropertyFunctionList(s_ctx, n, s_natives, sizeof s_natives / sizeof s_natives[0]);
    JS_SetPropertyStr(s_ctx, g, "__amv", n);
    s_amv = JS_UNDEFINED;

    JSValue m = JS_ReadObject(s_ctx, openmouse_bc_start, (size_t)(openmouse_bc_end - openmouse_bc_start), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(m)) { log_exception("read bytecode"); JS_FreeValue(s_ctx, g); s_failed = true; return false; }
    if (JS_ResolveModule(s_ctx, m) < 0) { log_exception("resolve"); JS_FreeValue(s_ctx, g); s_failed = true; return false; }
    JSValue r = await_value(JS_EvalFunction(s_ctx, m), LOAD_TIMEOUT_MS);
    if (JS_IsException(r)) { log_exception("evaluate"); JS_FreeValue(s_ctx, g); s_failed = true; return false; }
    JS_FreeValue(s_ctx, r);

    s_amv = JS_GetPropertyStr(s_ctx, g, "amv");
    JS_FreeValue(s_ctx, g);
    if (!JS_IsObject(s_amv)) { amv_log("[js] the bundle did not define globalThis.amv\n"); s_failed = true; return false; }
    JS_RunGC(s_rt);
    s_load_ms = now_ms() - t0;
    s_ready = true;
    amv_log("[js] OpenMouse bundle loaded in %lu ms: bytecode %u KB, engine heap %u KB (peak %u KB) of %u KB PSRAM\n",
            (unsigned long)s_load_ms, (unsigned)((openmouse_bc_end - openmouse_bc_start) / 1024), (unsigned)(s_used / 1024),
            (unsigned)(s_peak / 1024), (unsigned)(ph_capacity() / 1024));
    return true;
}

bool js_ready(void) { return s_ready; }

int js_open(const js_transport_t *t, void *h, uint16_t vid, uint16_t pid, const char *name,
            const js_iface_t *ifs, int n, char *brand, size_t brand_cap)
{
    if (brand && brand_cap) brand[0] = 0;
    if (!s_ready) return 0;
    int sid = 0;
    for (int i = 1; i <= MAX_SESSIONS; i++) if (!s_sess[i].used) { sid = i; break; }
    if (!sid) return 0;
    s_sess[sid] = (session_t){ .used = 1, .t = t, .h = h };

    JS_UpdateStackTop(s_rt);
    JSValue arr = JS_NewArray(s_ctx);
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(s_ctx);
        JS_SetPropertyStr(s_ctx, o, "x", JS_NewInt32(s_ctx, ifs[i].x));
        JS_SetPropertyStr(s_ctx, o, "desc", JS_NewArrayBufferCopy(s_ctx, ifs[i].desc, ifs[i].desc_len));
        JS_SetPropertyUint32(s_ctx, arr, (uint32_t)i, o);
    }
    JSValue args[5] = { JS_NewInt32(s_ctx, sid), JS_NewInt32(s_ctx, vid), JS_NewInt32(s_ctx, pid),
                        JS_NewString(s_ctx, name ? name : ""), arr };
    JSValue res = call_amv("open", 5, args);
    JS_FreeValue(s_ctx, args[3]);
    JS_FreeValue(s_ctx, args[4]);
    int ok = 0;
    if (JS_IsException(res)) log_exception("open");
    else if (!JS_IsNull(res) && !JS_IsUndefined(res)) {
        if (brand) get_str(res, "brand", brand, brand_cap);
        ok = 1;
    }
    JS_FreeValue(s_ctx, res);
    if (!ok) { s_sess[sid].used = 0; return 0; }
    return sid;
}

bool js_call_busy(void) { return s_busy; }

bool js_call_start(int sid, int op, uint8_t setting, uint16_t value)
{
    if (!s_ready || s_busy || sid < 1 || sid > MAX_SESSIONS || !s_sess[sid].used) return false;
    JS_UpdateStackTop(s_rt);
    JSValue res;
    if (op == JS_OP_PROBE) {
        JSValue a[1] = { JS_NewInt32(s_ctx, sid) };
        res = call_amv("probe", 1, a);
    } else if (op == JS_OP_SET) {
        JSValue a[3] = { JS_NewInt32(s_ctx, sid), JS_NewInt32(s_ctx, setting), JS_NewInt32(s_ctx, value) };
        res = call_amv("set", 3, a);
    } else {
        JSValue a[1] = { JS_NewInt32(s_ctx, sid) };
        res = call_amv("close", 1, a);
    }
    s_busy = true;
    s_call_sid = sid;
    s_call_op = op;
    s_call_val = res;
    s_call_start = now_ms();
    s_peak = s_used;
    return true;
}

int js_call_poll(js_status_t *st)
{
    if (!s_busy) return -1;
    JS_UpdateStackTop(s_rt);
    JSValue r;
    int done;
    if (JS_IsException(s_call_val)) { r = s_call_val; done = 1; }
    else done = step_value(s_call_val, &r);
    if (!done) {
        if ((int)(now_ms() - s_call_start) <= CALL_TIMEOUT_MS) return 0;
        JS_FreeValue(s_ctx, s_call_val);
        r = JS_ThrowInternalError(s_ctx, "timed out after %d ms", CALL_TIMEOUT_MS);
    }
    s_busy = false;
    s_calls++;
    s_call_ms_last = now_ms() - s_call_start;
    if (s_call_ms_last > s_call_ms_max) s_call_ms_max = s_call_ms_last;
    int ret;
    if (JS_IsException(r)) {
        log_exception(s_call_op == JS_OP_PROBE ? "readStatus" : s_call_op == JS_OP_SET ? "write" : "close");
        ret = -1;
    } else {
        if (st && JS_IsObject(r)) read_status(r, st);
        ret = 1;
    }
    JS_FreeValue(s_ctx, r);
    if (s_call_op == JS_OP_CLOSE) {
        s_sess[s_call_sid].used = 0;
        JS_RunGC(s_rt);
    }
    return ret;
}

void js_forget(int sid)
{
    if (sid < 1 || sid > MAX_SESSIONS || !s_sess[sid].used) return;
    if (s_busy && s_call_sid == sid) return;      /* the running call finishes (its transfers fail) and releases it */
    JS_UpdateStackTop(s_rt);
    JSValue a[1] = { JS_NewInt32(s_ctx, sid) };
    JSValue r = call_amv("close", 1, a);          /* drops the session in the bridge; the driver's close is best effort */
    JS_FreeValue(s_ctx, r);
    s_sess[sid].used = 0;
    run_jobs();
    JS_RunGC(s_rt);
}

void js_get_stats(js_stats_t *out)
{
    memset(out, 0, sizeof *out);
    out->ready = s_ready;
    out->failed = s_failed;
    out->load_ms = s_load_ms;
    out->bytecode_bytes = (size_t)(openmouse_bc_end - openmouse_bc_start);
    out->heap_used = s_used;
    out->heap_peak = s_peak;
    out->heap_capacity = ph_capacity();
    out->calls = s_calls;
    out->call_ms_last = s_call_ms_last;
    out->call_ms_max = s_call_ms_max;
    for (int i = 1; i <= MAX_SESSIONS; i++) out->sessions += s_sess[i].used;
}

void js_set_log(int level) { s_log = level; }
