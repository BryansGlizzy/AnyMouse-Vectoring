#include "amv_js.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "quickjs.h"

static const char *TAG = "amv_js";

#define MAX_SESSIONS     8
#define TASK_STACK       (48 * 1024)     /* internal RAM; the engine's recursion runs here */
#define JS_STACK_LIMIT   (40 * 1024)     /* QuickJS throws "stack overflow" before the task overflows */
#define CALL_TIMEOUT_MS  15000           /* one driver call: OpenMouse retries a sleeping receiver for seconds */
#define LOAD_TIMEOUT_MS  30000
#define HEAP_CAPS        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define IO_MAX           128             /* report bytes per exchange, as the amv_hl_* transport */
#define READY_BIT        BIT0
#define FAILED_BIT       BIT1

/* generated/openmouse.bc, embedded by CMakeLists.txt */
extern const uint8_t bc_start[] asm("_binary_openmouse_bc_start");
extern const uint8_t bc_end[]   asm("_binary_openmouse_bc_end");

typedef struct {
    bool used;
    const amv_js_transport_t *t;
    void *h;
} session_t;

typedef enum { OP_OPEN, OP_PROBE, OP_SET, OP_CLOSE } op_t;

typedef struct {
    op_t op;
    int sid;
    uint8_t id;
    uint16_t value;
    const amv_js_transport_t *t;
    void *h;
    uint16_t vid, pid;
    const char *name;
    const amv_js_iface_t *ifs;
    int n;
    char *brand;
    size_t brand_cap;
    amv_js_status_t *st;
    esp_err_t err;
    int result;
    SemaphoreHandle_t done;
} req_t;

static JSRuntime *s_rt;
static JSContext *s_ctx;
static JSValue s_amv;                 /* globalThis.amv, set by js/bridge.js */
static session_t s_sess[MAX_SESSIONS + 1];   /* index = session id; 0 unused */
static QueueHandle_t s_q;
static EventGroupHandle_t s_ev;
static TaskHandle_t s_task;
static int64_t s_t0;
static size_t s_used, s_peak;
static uint32_t s_load_ms;

/* ---------- allocator: PSRAM, counted ---------- */

static size_t usable(const void *p) { return p ? heap_caps_get_allocated_size((void *)p) : 0; }

static void count_add(size_t n)
{
    s_used += n;
    if (s_used > s_peak) s_peak = s_used;
}

static void *js_calloc_cb(void *opaque, size_t count, size_t size)
{
    void *p = heap_caps_calloc(count, size, HEAP_CAPS);
    count_add(usable(p));
    return p;
}

static void *js_malloc_cb(void *opaque, size_t size)
{
    void *p = heap_caps_malloc(size, HEAP_CAPS);
    count_add(usable(p));
    return p;
}

static void js_free_cb(void *opaque, void *p)
{
    if (!p) return;
    s_used -= usable(p);
    heap_caps_free(p);
}

static void *js_realloc_cb(void *opaque, void *p, size_t size)
{
    if (!size) { js_free_cb(opaque, p); return NULL; }
    size_t old = usable(p);
    void *np = heap_caps_realloc(p, size, HEAP_CAPS);
    if (np) { s_used -= old; count_add(usable(np)); }
    return np;
}

static size_t js_usable_cb(const void *p) { return usable(p); }

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
    ESP_LOGW(TAG, "%s: %s%s%s", what, msg ? msg : "(no message)", stack ? "\n" : "", stack ? stack : "");
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
    return JS_NewFloat64(c, (double)((esp_timer_get_time() - s_t0) / 1000));
}

static JSValue n_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t lvl = 1;
    if (argc > 0) JS_ToInt32(c, &lvl, argv[0]);
    const char *s = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    if (!s) return JS_UNDEFINED;
    if (lvl >= 2) ESP_LOGW("js", "%s", s);
    else if (lvl == 1) ESP_LOGI("js", "%s", s);
    else ESP_LOGD("js", "%s", s);
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
    if (p && off + len <= total) esp_fill_random(p + off, len);
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
    esp_err_t e = s->t->send(s->h, x, (uint8_t)type, (uint8_t)id, d, (uint16_t)len);
    if (e != ESP_OK)
        return JS_ThrowInternalError(c, "Failed to write the %s report (%s).", type == 3 ? "feature" : "output", esp_err_to_name(e));
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
    esp_err_t e = s->t->recv_feature(s->h, x, (uint8_t)id, buf, (uint16_t)len, &got);
    if (e != ESP_OK) return JS_ThrowInternalError(c, "Failed to receive the feature report (%s).", esp_err_to_name(e));
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
    JSValue r = call_amv("pump", 0, NULL);
    int32_t ms = -1;
    if (JS_IsException(r)) log_exception("pump");
    else JS_ToInt32(s_ctx, &ms, r);
    JS_FreeValue(s_ctx, r);
    return ms;
}

/* Takes `v`; drives the engine until a promise settles. Returns the value, or JS_EXCEPTION
 * with the exception pending (a rejection, or a timeout). */
static JSValue await_value(JSValue v, int timeout_ms)
{
    if (JS_IsException(v)) return v;
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        run_jobs();
        if (!JS_IsPromise(v)) return v;
        JSPromiseStateEnum st = JS_PromiseState(s_ctx, v);
        if (st != JS_PROMISE_PENDING) {
            JSValue r = JS_PromiseResult(s_ctx, v);
            JS_FreeValue(s_ctx, v);
            if (st == JS_PROMISE_FULFILLED) return r;
            return JS_Throw(s_ctx, r);
        }
        deliver_inputs();
        int wait = pump_timers();
        run_jobs();
        if (JS_PromiseState(s_ctx, v) != JS_PROMISE_PENDING) continue;
        if (esp_timer_get_time() > end) {
            JS_FreeValue(s_ctx, v);
            return JS_ThrowInternalError(s_ctx, "timed out after %d ms", timeout_ms);
        }
        /* Nothing runnable: sleep one tick (1 ms) unless a timer is due sooner than that. */
        if (wait != 0) vTaskDelay(1);
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

static void read_status(JSValueConst o, amv_js_status_t *st)
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

/* ---------- requests (engine task) ---------- */

static void do_open(req_t *r)
{
    int sid = 0;
    for (int i = 1; i <= MAX_SESSIONS; i++) if (!s_sess[i].used) { sid = i; break; }
    if (!sid) { r->err = ESP_ERR_NO_MEM; return; }
    s_sess[sid] = (session_t){ .used = true, .t = r->t, .h = r->h };

    JSValue ifs = JS_NewArray(s_ctx);
    for (int i = 0; i < r->n; i++) {
        JSValue o = JS_NewObject(s_ctx);
        JS_SetPropertyStr(s_ctx, o, "x", JS_NewInt32(s_ctx, r->ifs[i].x));
        JS_SetPropertyStr(s_ctx, o, "desc", JS_NewArrayBufferCopy(s_ctx, r->ifs[i].desc, r->ifs[i].desc_len));
        JS_SetPropertyUint32(s_ctx, ifs, (uint32_t)i, o);
    }
    JSValue args[5] = { JS_NewInt32(s_ctx, sid), JS_NewInt32(s_ctx, r->vid), JS_NewInt32(s_ctx, r->pid),
                        JS_NewString(s_ctx, r->name ? r->name : ""), ifs };
    JSValue res = call_amv("open", 5, args);
    JS_FreeValue(s_ctx, args[3]);
    JS_FreeValue(s_ctx, args[4]);
    if (JS_IsException(res)) {
        log_exception("open");
        r->err = ESP_FAIL;
    } else if (JS_IsNull(res) || JS_IsUndefined(res)) {
        r->err = ESP_ERR_NOT_SUPPORTED;
    } else {
        if (r->brand) get_str(res, "brand", r->brand, r->brand_cap);
        r->result = sid;
        r->err = ESP_OK;
    }
    JS_FreeValue(s_ctx, res);
    if (r->err != ESP_OK) s_sess[sid].used = false;
}

static void do_call(req_t *r)
{
    if (r->sid < 1 || r->sid > MAX_SESSIONS || !s_sess[r->sid].used) { r->err = ESP_ERR_INVALID_STATE; return; }
    JSValue res;
    if (r->op == OP_PROBE) {
        JSValue a[1] = { JS_NewInt32(s_ctx, r->sid) };
        res = call_amv("probe", 1, a);
    } else if (r->op == OP_SET) {
        JSValue a[3] = { JS_NewInt32(s_ctx, r->sid), JS_NewInt32(s_ctx, r->id), JS_NewInt32(s_ctx, r->value) };
        res = call_amv("set", 3, a);
    } else {
        JSValue a[1] = { JS_NewInt32(s_ctx, r->sid) };
        res = call_amv("close", 1, a);
    }
    res = await_value(res, CALL_TIMEOUT_MS);
    if (JS_IsException(res)) {
        log_exception(r->op == OP_PROBE ? "readStatus" : r->op == OP_SET ? "write" : "close");
        r->err = ESP_FAIL;
    } else {
        if (r->st && JS_IsObject(res)) read_status(res, r->st);
        r->err = ESP_OK;
    }
    JS_FreeValue(s_ctx, res);
    if (r->op == OP_CLOSE) {
        s_sess[r->sid].used = false;
        JS_RunGC(s_rt);
    }
}

static esp_err_t load_bundle(void)
{
    s_rt = JS_NewRuntime2(&s_mf, NULL);
    if (!s_rt) return ESP_ERR_NO_MEM;
    JS_SetMaxStackSize(s_rt, JS_STACK_LIMIT);
    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx) return ESP_ERR_NO_MEM;

    JSValue g = JS_GetGlobalObject(s_ctx);
    JSValue n = JS_NewObject(s_ctx);
    JS_SetPropertyFunctionList(s_ctx, n, s_natives, sizeof s_natives / sizeof s_natives[0]);
    JS_SetPropertyStr(s_ctx, g, "__amv", n);

    JSValue m = JS_ReadObject(s_ctx, bc_start, (size_t)(bc_end - bc_start), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(m)) { log_exception("read bytecode"); JS_FreeValue(s_ctx, g); return ESP_FAIL; }
    if (JS_ResolveModule(s_ctx, m) < 0) { log_exception("resolve"); JS_FreeValue(s_ctx, g); return ESP_FAIL; }
    JSValue r = await_value(JS_EvalFunction(s_ctx, m), LOAD_TIMEOUT_MS);
    if (JS_IsException(r)) { log_exception("evaluate"); JS_FreeValue(s_ctx, g); return ESP_FAIL; }
    JS_FreeValue(s_ctx, r);

    s_amv = JS_GetPropertyStr(s_ctx, g, "amv");
    JS_FreeValue(s_ctx, g);
    if (!JS_IsObject(s_amv)) { ESP_LOGE(TAG, "the bundle did not define globalThis.amv"); return ESP_FAIL; }
    JS_RunGC(s_rt);
    return ESP_OK;
}

static void js_task(void *arg)
{
    int64_t t = esp_timer_get_time();
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t e = load_bundle();
    s_load_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "OpenMouse bundle failed to load (%s); mice stay generic", esp_err_to_name(e));
        xEventGroupSetBits(s_ev, FAILED_BIT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OpenMouse bundle loaded in %u ms: bytecode %u KB, engine heap %u KB (peak %u KB, PSRAM used %u KB), "
             "internal RAM free %u KB, task stack free %u B",
             (unsigned)s_load_ms, (unsigned)((bc_end - bc_start) / 1024), (unsigned)(s_used / 1024), (unsigned)(s_peak / 1024),
             (unsigned)((psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xEventGroupSetBits(s_ev, READY_BIT);

    for (;;) {
        req_t *r;
        xQueueReceive(s_q, &r, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        size_t peak0 = s_peak;
        s_peak = s_used;
        deliver_inputs();
        run_jobs();
        if (r->op == OP_OPEN) do_open(r);
        else do_call(r);
        static const char *const names[] = { "open", "readStatus", "write", "close" };
        ESP_LOGI(TAG, "%s (session %d): %s in %u ms; heap %u KB, peak during call %u KB, stack free %u B", names[r->op],
                 r->op == OP_OPEN ? r->result : r->sid, r->err == ESP_OK ? "ok" : esp_err_to_name(r->err),
                 (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)(s_used / 1024), (unsigned)(s_peak / 1024),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (peak0 > s_peak) s_peak = peak0;
        xSemaphoreGive(r->done);
    }
}

/* ---------- public API (any task but the engine's) ---------- */

static esp_err_t submit(req_t *r)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    EventBits_t b = xEventGroupWaitBits(s_ev, READY_BIT | FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(LOAD_TIMEOUT_MS));
    if (b & FAILED_BIT) return ESP_ERR_NOT_SUPPORTED;
    if (!(b & READY_BIT)) return ESP_ERR_TIMEOUT;
    r->done = xSemaphoreCreateBinary();
    if (!r->done) return ESP_ERR_NO_MEM;
    r->err = ESP_FAIL;
    xQueueSend(s_q, &r, portMAX_DELAY);
    xSemaphoreTake(r->done, portMAX_DELAY);
    vSemaphoreDelete(r->done);
    return r->err;
}

esp_err_t amv_js_start(void)
{
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        ESP_LOGE(TAG, "no PSRAM: the OpenMouse engine needs it (CONFIG_SPIRAM)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_t0 = esp_timer_get_time();
    s_q = xQueueCreate(4, sizeof(req_t *));
    s_ev = xEventGroupCreate();
    if (!s_q || !s_ev) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(js_task, "amv_js", TASK_STACK, NULL, 4, &s_task, 1) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

int amv_js_open(const amv_js_transport_t *t, void *h, uint16_t vid, uint16_t pid, const char *name,
                const amv_js_iface_t *ifs, int n, char *brand, size_t brand_cap)
{
    req_t r = { .op = OP_OPEN, .t = t, .h = h, .vid = vid, .pid = pid, .name = name, .ifs = ifs, .n = n,
                .brand = brand, .brand_cap = brand_cap };
    if (brand && brand_cap) brand[0] = 0;
    return submit(&r) == ESP_OK ? r.result : 0;
}

esp_err_t amv_js_probe(int sid, amv_js_status_t *out)
{
    req_t r = { .op = OP_PROBE, .sid = sid, .st = out };
    return submit(&r);
}

esp_err_t amv_js_set(int sid, uint8_t id, uint16_t value, amv_js_status_t *out)
{
    req_t r = { .op = OP_SET, .sid = sid, .id = id, .value = value, .st = out };
    return submit(&r);
}

void amv_js_close(int sid)
{
    req_t r = { .op = OP_CLOSE, .sid = sid };
    submit(&r);
}

void amv_js_get_stats(amv_js_stats_t *out)
{
    memset(out, 0, sizeof *out);
    out->ready = s_ev && (xEventGroupGetBits(s_ev) & READY_BIT);
    out->load_ms = s_load_ms;
    out->bytecode_bytes = (size_t)(bc_end - bc_start);
    out->heap_used = s_used;
    out->heap_peak = s_peak;
    out->stack_free = s_task && out->ready ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
    for (int i = 1; i <= MAX_SESSIONS; i++) out->sessions += s_sess[i].used;
}
