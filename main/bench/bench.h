#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "cJSON.h"

/* ------------------------------------------------------------------------
 * Benchmark framework.
 *
 * A benchmark is a `bench_def_t` with a run() callback. run() records
 * metrics through the bench_* helpers below; the framework times it, turns the
 * outcome into a JSON result and hands it to the output sink (WebSocket + serial).
 * Only one benchmark runs at a time, on a dedicated worker task.
 * ---------------------------------------------------------------------- */

typedef struct bench_ctx bench_ctx_t;

typedef struct {
    const char *id;     /* short stable identifier used by the UI/API, e.g. "cpu" */
    const char *name;   /* display name */
    const char *group;  /* Compute | Memory | Security | Connectivity | Peripherals | Thermal */
    const char *desc;
    /* NULL = always available. Otherwise return false and set *reason. */
    bool (*available)(const char **reason);
    void (*run)(bench_ctx_t *ctx);
} bench_def_t;

/* ---- helpers for benchmark implementations ---- */
void bench_metric(bench_ctx_t *c, const char *name, double value, const char *unit);
void bench_text(bench_ctx_t *c, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void bench_note(bench_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bench_fail(bench_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bench_skip(bench_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

int         bench_param_int(const bench_ctx_t *c, const char *key, int def);
const char *bench_param_str(const bench_ctx_t *c, const char *key, const char *def);

static inline int64_t bench_now_us(void) { return esp_timer_get_time(); }

/* ---- runner API ---- */

/* Receives every progress/result message as a JSON string (called from the worker task). */
typedef void (*bench_sink_t)(const char *json);

void   bench_init(bench_sink_t sink);
bool   bench_is_running(void);
/* id = benchmark id or "all". params (optional) is consumed. */
esp_err_t bench_request(const char *id, cJSON *params);
/* [{id,name,group,desc,available,reason}] */
cJSON *bench_list_json(void);
/* {"<id>": result, ...} of everything that ran since boot */
cJSON *bench_results_json(void);

/* ---- registry (implemented by the individual bench_*.c files) ---- */
extern const bench_def_t bench_def_cpu;
extern const bench_def_t bench_def_ram;
extern const bench_def_t bench_def_flash;
extern const bench_def_t bench_def_crypto;
extern const bench_def_t bench_def_rtos;
extern const bench_def_t bench_def_gpio;
extern const bench_def_t bench_def_uart;
extern const bench_def_t bench_def_i2c;
extern const bench_def_t bench_def_led;
extern const bench_def_t bench_def_ulp;
extern const bench_def_t bench_def_thermal;
extern const bench_def_t bench_def_wifi;
