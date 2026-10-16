#include "bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct bench_ctx {
    cJSON *metrics;
    cJSON *notes;
    cJSON *params;
    const char *status; /* "ok" | "skip" | "fail" */
};

typedef struct {
    char   id[16];
    cJSON *params;
} request_t;

static const bench_def_t *const s_registry[] = {
    &bench_def_cpu,   &bench_def_ram,  &bench_def_flash,  &bench_def_crypto,
    &bench_def_rtos,  &bench_def_gpio, &bench_def_uart,   &bench_def_i2c,
    &bench_def_led,   &bench_def_ulp,  &bench_def_thermal, &bench_def_wifi,
};
#define NUM_BENCH (sizeof(s_registry) / sizeof(s_registry[0]))

static bench_sink_t     s_sink;
static QueueHandle_t    s_queue;
static volatile bool    s_running;
static SemaphoreHandle_t s_results_lock;
static cJSON           *s_results;

/* ---------------------------------------------------------- ctx helpers */

void bench_metric(bench_ctx_t *c, const char *name, double value, const char *unit)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "n", name);
    cJSON_AddNumberToObject(m, "v", value);
    if (unit && *unit) cJSON_AddStringToObject(m, "u", unit);
    cJSON_AddItemToArray(c->metrics, m);
}

void bench_text(bench_ctx_t *c, const char *name, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "n", name);
    cJSON_AddStringToObject(m, "s", buf);
    cJSON_AddItemToArray(c->metrics, m);
}

static void add_note(bench_ctx_t *c, const char *fmt, va_list ap)
{
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    cJSON_AddItemToArray(c->notes, cJSON_CreateString(buf));
}

void bench_note(bench_ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_note(c, fmt, ap);
    va_end(ap);
}

void bench_fail(bench_ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_note(c, fmt, ap);
    va_end(ap);
    c->status = "fail";
}

void bench_skip(bench_ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_note(c, fmt, ap);
    va_end(ap);
    c->status = "skip";
}

int bench_param_int(const bench_ctx_t *c, const char *key, int def)
{
    const cJSON *p = c->params ? cJSON_GetObjectItemCaseSensitive(c->params, key) : NULL;
    if (cJSON_IsNumber(p)) return p->valueint;
    if (cJSON_IsString(p) && p->valuestring[0]) return atoi(p->valuestring);
    return def;
}

const char *bench_param_str(const bench_ctx_t *c, const char *key, const char *def)
{
    const cJSON *p = c->params ? cJSON_GetObjectItemCaseSensitive(c->params, key) : NULL;
    return (cJSON_IsString(p) && p->valuestring[0]) ? p->valuestring : def;
}

/* ---------------------------------------------------------------- runner */

static void emit(cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    if (s) {
        if (s_sink) s_sink(s);
        cJSON_free(s);
    }
}

static void emit_event(const char *event, const char *id, int index, int total)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *b = cJSON_AddObjectToObject(root, "bench");
    cJSON_AddStringToObject(b, "event", event);
    if (id) cJSON_AddStringToObject(b, "id", id);
    if (total) {
        cJSON_AddNumberToObject(b, "index", index);
        cJSON_AddNumberToObject(b, "total", total);
    }
    emit(root);
    cJSON_Delete(root);
}

static const bench_def_t *find_def(const char *id)
{
    for (size_t i = 0; i < NUM_BENCH; i++) {
        if (strcmp(s_registry[i]->id, id) == 0) return s_registry[i];
    }
    return NULL;
}

static void run_one(const bench_def_t *def, cJSON *params, int index, int total)
{
    emit_event("start", def->id, index, total);

    bench_ctx_t ctx = {
        .metrics = cJSON_CreateArray(),
        .notes = cJSON_CreateArray(),
        .params = params,
        .status = "ok",
    };

    const char *reason = NULL;
    int64_t t0 = bench_now_us();
    if (def->available && !def->available(&reason)) {
        bench_skip(&ctx, "%s", reason ? reason : "not available");
    } else {
        def->run(&ctx);
    }
    int64_t elapsed_ms = (bench_now_us() - t0) / 1000;

    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "id", def->id);
    cJSON_AddStringToObject(res, "name", def->name);
    cJSON_AddStringToObject(res, "status", ctx.status);
    cJSON_AddNumberToObject(res, "duration_ms", (double)elapsed_ms);
    cJSON_AddItemToObject(res, "metrics", ctx.metrics);
    cJSON_AddItemToObject(res, "notes", ctx.notes);

    /* Keep a copy for late joiners and the REST endpoint. */
    xSemaphoreTake(s_results_lock, portMAX_DELAY);
    cJSON_DeleteItemFromObject(s_results, def->id);
    cJSON_AddItemToObject(s_results, def->id, cJSON_Duplicate(res, true));
    xSemaphoreGive(s_results_lock);

    cJSON *msg = cJSON_CreateObject();
    cJSON *b = cJSON_AddObjectToObject(msg, "bench");
    cJSON_AddStringToObject(b, "event", "done");
    cJSON_AddStringToObject(b, "id", def->id);
    cJSON_AddItemToObject(b, "result", res);
    char *line = cJSON_PrintUnformatted(res);
    if (line) {
        printf("BENCH_RESULT %s\n", line);
        cJSON_free(line);
    }
    emit(msg);
    cJSON_Delete(msg);
}

static void bench_task(void *arg)
{
    request_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        s_running = true;

        if (strcmp(req.id, "all") == 0) {
            for (size_t i = 0; i < NUM_BENCH; i++) {
                run_one(s_registry[i], req.params, (int)i + 1, (int)NUM_BENCH);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            const bench_def_t *def = find_def(req.id);
            if (def) run_one(def, req.params, 1, 1);
        }
        emit_event("finished", NULL, 0, 0);

        cJSON_Delete(req.params);
        s_running = false;
    }
}

void bench_init(bench_sink_t sink)
{
    s_sink = sink;
    s_results = cJSON_CreateObject();
    s_results_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(2, sizeof(request_t));
    xTaskCreate(bench_task, "bench", 12288, NULL, 5, NULL);
}

bool bench_is_running(void) { return s_running; }

esp_err_t bench_request(const char *id, cJSON *params)
{
    request_t req = { .params = params };
    strlcpy(req.id, id, sizeof(req.id));
    if (strcmp(req.id, "all") != 0 && !find_def(req.id)) {
        cJSON_Delete(params);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_running || xQueueSend(s_queue, &req, 0) != pdTRUE) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

cJSON *bench_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < NUM_BENCH; i++) {
        const bench_def_t *d = s_registry[i];
        const char *reason = NULL;
        bool ok = !d->available || d->available(&reason);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", d->id);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddStringToObject(o, "group", d->group);
        cJSON_AddStringToObject(o, "desc", d->desc);
        cJSON_AddBoolToObject(o, "available", ok);
        if (!ok && reason) cJSON_AddStringToObject(o, "reason", reason);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *bench_results_json(void)
{
    xSemaphoreTake(s_results_lock, portMAX_DELAY);
    cJSON *copy = cJSON_Duplicate(s_results, true);
    xSemaphoreGive(s_results_lock);
    return copy;
}
