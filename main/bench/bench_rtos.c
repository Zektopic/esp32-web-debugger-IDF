/* RTOS benchmark: context-switch cost, queue throughput, mutex and heap
 * allocator speed, and esp_timer periodic-callback jitter. */
#include "bench.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "soc/soc_caps.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---- context switch: two tasks on one core hand a notification back and forth ---- */

#define PINGPONG_ROUNDS 5000

typedef struct {
    TaskHandle_t peer;
    TaskHandle_t done;
    int rounds;
    bool starter;
} pp_arg_t;

static void pingpong(void *p)
{
    pp_arg_t *a = p;
    /* Wait for the go signal. Decrement (not clear) the count: the peer may
     * already have sent its first ping on top of it. */
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    for (int i = 0; i < a->rounds; i++) {
        if (a->starter) {
            xTaskNotifyGive(a->peer);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            xTaskNotifyGive(a->peer);
        }
    }
    if (a->starter) xTaskNotifyGive(a->done);
    vTaskDelete(NULL);
}

static double context_switch_us(void)
{
    pp_arg_t a = { .rounds = PINGPONG_ROUNDS, .starter = true };
    pp_arg_t b = { .rounds = PINGPONG_ROUNDS, .starter = false };
    TaskHandle_t ta, tb;
    a.done = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(pingpong, "ppA", 2048, &a, configMAX_PRIORITIES - 3, &ta, 0);
    xTaskCreatePinnedToCore(pingpong, "ppB", 2048, &b, configMAX_PRIORITIES - 3, &tb, 0);
    a.peer = tb;
    b.peer = ta;
    int64_t t0 = bench_now_us();
    xTaskNotifyGive(ta); /* release both: B waits first, A starts the chain */
    xTaskNotifyGive(tb);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int64_t dt = bench_now_us() - t0;
    return (double)dt / (PINGPONG_ROUNDS * 2.0); /* two switches per round trip */
}

/* ---- queue throughput between two cores (or one) ---- */

#define QUEUE_ITEMS 20000

typedef struct {
    QueueHandle_t q;
    TaskHandle_t done;
} q_arg_t;

static void producer(void *p)
{
    q_arg_t *a = p;
    for (uint32_t i = 0; i < QUEUE_ITEMS; i++) xQueueSend(a->q, &i, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void consumer(void *p)
{
    q_arg_t *a = p;
    uint32_t v;
    for (int i = 0; i < QUEUE_ITEMS; i++) xQueueReceive(a->q, &v, portMAX_DELAY);
    xTaskNotifyGive(a->done);
    vTaskDelete(NULL);
}

static double queue_msgs_per_s(void)
{
    q_arg_t a = { .q = xQueueCreate(16, sizeof(uint32_t)), .done = xTaskGetCurrentTaskHandle() };
    if (!a.q) return 0;
    int other = SOC_CPU_CORES_NUM > 1 ? 1 : 0;
    int64_t t0 = bench_now_us();
    xTaskCreatePinnedToCore(consumer, "qcons", 2048, &a, configMAX_PRIORITIES - 3, NULL, 0);
    xTaskCreatePinnedToCore(producer, "qprod", 2048, &a, configMAX_PRIORITIES - 3, NULL, other);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int64_t dt = bench_now_us() - t0;
    vQueueDelete(a.q);
    return QUEUE_ITEMS * 1e6 / (double)dt;
}

/* ---- mutex + heap ---- */

static double mutex_mops(void)
{
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (!m) return 0;
    const int n = 100000;
    int64_t t0 = bench_now_us();
    for (int i = 0; i < n; i++) {
        xSemaphoreTake(m, portMAX_DELAY);
        xSemaphoreGive(m);
    }
    int64_t dt = bench_now_us() - t0;
    vSemaphoreDelete(m);
    return (double)n / (double)dt; /* take+give pairs per microsecond == millions per second */
}

static double heap_ops_per_s(size_t size)
{
    enum { N = 256 };
    void *ptrs[N];
    memset(ptrs, 0, sizeof(ptrs));
    const int rounds = 20;
    int64_t t0 = bench_now_us();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < N; i++) ptrs[i] = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        for (int i = 0; i < N; i++) heap_caps_free(ptrs[i]);
    }
    int64_t dt = bench_now_us() - t0;
    return rounds * N * 2.0 * 1e6 / (double)dt; /* malloc + free */
}

/* ---- esp_timer jitter ---- */

#define JITTER_SAMPLES 500
#define JITTER_PERIOD_US 1000

typedef struct {
    int64_t stamps[JITTER_SAMPLES];
    volatile int n;
    TaskHandle_t waiter;
} jitter_t;

static void jitter_cb(void *p)
{
    jitter_t *j = p;
    if (j->n < JITTER_SAMPLES) {
        j->stamps[j->n++] = esp_timer_get_time();
        if (j->n == JITTER_SAMPLES) xTaskNotifyGive(j->waiter);
    }
}

static bool timer_jitter(double *mean_us, double *max_dev_us, double *rms_us)
{
    jitter_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!j) return false;
    j->waiter = xTaskGetCurrentTaskHandle();
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = jitter_cb, .arg = j, .name = "bench_jitter" };
    if (esp_timer_create(&args, &t) != ESP_OK) {
        heap_caps_free(j);
        return false;
    }
    esp_timer_start_periodic(t, JITTER_PERIOD_US);
    bool ok = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000)) > 0;
    esp_timer_stop(t);
    esp_timer_delete(t);
    if (ok) {
        double sum = 0, max_dev = 0, sq = 0;
        for (int i = 1; i < JITTER_SAMPLES; i++) {
            double d = (double)(j->stamps[i] - j->stamps[i - 1]);
            sum += d;
            double dev = fabs(d - JITTER_PERIOD_US);
            if (dev > max_dev) max_dev = dev;
            sq += (d - JITTER_PERIOD_US) * (d - JITTER_PERIOD_US);
        }
        *mean_us = sum / (JITTER_SAMPLES - 1);
        *max_dev_us = max_dev;
        *rms_us = sqrt(sq / (JITTER_SAMPLES - 1));
    }
    heap_caps_free(j);
    return ok;
}

static void run(bench_ctx_t *ctx)
{
    bench_metric(ctx, "FreeRTOS tick rate", configTICK_RATE_HZ, "Hz");
    bench_metric(ctx, "Context switch (task <-> task)", context_switch_us(), "us");
    bench_metric(ctx, "Queue throughput", queue_msgs_per_s() / 1000.0, "k msg/s");
    bench_metric(ctx, "Mutex take+give", mutex_mops(), "M pairs/s");
    bench_metric(ctx, "Heap malloc+free (64 B)", heap_ops_per_s(64) / 1000.0, "k ops/s");
    bench_metric(ctx, "Heap malloc+free (1 KB)", heap_ops_per_s(1024) / 1000.0, "k ops/s");

    double mean, maxdev, rms;
    if (timer_jitter(&mean, &maxdev, &rms)) {
        bench_metric(ctx, "esp_timer 1 ms period (mean)", mean, "us");
        bench_metric(ctx, "esp_timer jitter (RMS)", rms, "us");
        bench_metric(ctx, "esp_timer jitter (worst case)", maxdev, "us");
    } else {
        bench_note(ctx, "esp_timer jitter test did not complete.");
    }
    bench_note(ctx, "Wi-Fi activity adds jitter; run with the station disconnected for a clean floor.");
}

const bench_def_t bench_def_rtos = {
    .id = "rtos",
    .name = "RTOS & heap",
    .group = "Compute",
    .desc = "Context-switch time, inter-core queue throughput, mutex and malloc speed, esp_timer jitter.",
    .available = NULL,
    .run = run,
};
