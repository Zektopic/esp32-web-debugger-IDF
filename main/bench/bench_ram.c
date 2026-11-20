/* RAM benchmark: bandwidth, random-access latency and an integrity test for
 * internal RAM and (when present) external PSRAM, plus heap statistics. */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIN_TIME_US 100000

static volatile uint32_t s_sink;

typedef void (*pass_fn)(uint32_t *a, uint32_t *b, size_t words);

static void pass_write(uint32_t *a, uint32_t *b, size_t w)
{
    (void)b;
    volatile uint32_t *p = a;
    for (size_t i = 0; i < w; i++) p[i] = (uint32_t)i;
}

static void pass_read(uint32_t *a, uint32_t *b, size_t w)
{
    (void)b;
    volatile uint32_t *p = a;
    uint32_t acc = 0;
    for (size_t i = 0; i < w; i++) acc += p[i];
    s_sink += acc;
}

static void pass_copy(uint32_t *a, uint32_t *b, size_t w) { memcpy(b, a, w * 4); }
static void pass_set(uint32_t *a, uint32_t *b, size_t w) { (void)b; memset(a, 0x5A, w * 4); }

/* Doubles the number of passes until the run lasts MIN_TIME_US; returns MB/s (decimal). */
static double bandwidth(pass_fn fn, uint32_t *a, uint32_t *b, size_t bytes)
{
    uint32_t passes = 1;
    int64_t dt;
    for (;;) {
        int64_t t0 = bench_now_us();
        for (uint32_t p = 0; p < passes; p++) fn(a, b, bytes / 4);
        dt = bench_now_us() - t0;
        if (dt >= MIN_TIME_US || passes >= (1u << 20)) break;
        passes *= 2;
    }
    vTaskDelay(1);
    return (double)bytes * passes / (double)(dt < 1 ? 1 : dt);
}

static double random_read_ns(uint32_t *a, size_t words)
{
    volatile uint32_t *p = a;
    uint32_t x = 12345, acc = 0;
    const uint32_t reads = 200000;
    int64_t t0 = bench_now_us();
    for (uint32_t i = 0; i < reads; i++) {
        x = x * 1664525u + 1013904223u;
        acc += p[(x >> 8) % words];
    }
    int64_t dt = bench_now_us() - t0;
    s_sink += acc;
    return (double)dt * 1000.0 / reads;
}

/* Fixed patterns + address-in-address. Returns number of mismatching words. */
static size_t integrity(uint32_t *a, size_t words)
{
    static const uint32_t pats[] = { 0x00000000u, 0xFFFFFFFFu, 0xAAAAAAAAu, 0x55555555u };
    volatile uint32_t *p = a;
    size_t errors = 0;
    for (size_t k = 0; k < sizeof(pats) / sizeof(pats[0]); k++) {
        for (size_t i = 0; i < words; i++) p[i] = pats[k];
        for (size_t i = 0; i < words; i++) if (p[i] != pats[k]) errors++;
    }
    for (size_t i = 0; i < words; i++) p[i] = (uint32_t)(uintptr_t)&p[i] ^ 0xA5A5A5A5u;
    for (size_t i = 0; i < words; i++) if (p[i] != ((uint32_t)(uintptr_t)&p[i] ^ 0xA5A5A5A5u)) errors++;
    return errors;
}

static void test_region(bench_ctx_t *ctx, const char *label, uint32_t caps, size_t want_bytes, size_t test_bytes)
{
    size_t largest = heap_caps_get_largest_free_block(caps);
    size_t bytes = want_bytes;
    if (bytes > largest / 3) bytes = (largest / 3) & ~3u;
    if (bytes < 4096) {
        bench_note(ctx, "%s: not enough free memory to test (largest block %u B)", label, (unsigned)largest);
        return;
    }
    uint32_t *a = heap_caps_malloc(bytes, caps | MALLOC_CAP_8BIT);
    uint32_t *b = heap_caps_malloc(bytes, caps | MALLOC_CAP_8BIT);
    if (!a || !b) {
        heap_caps_free(a);
        heap_caps_free(b);
        bench_note(ctx, "%s: allocation failed", label);
        return;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s block size", label);
    bench_metric(ctx, name, bytes / 1024.0, "KB");

    struct { const char *what; pass_fn fn; } tests[] = {
        { "sequential write", pass_write }, { "sequential read", pass_read },
        { "memcpy", pass_copy }, { "memset", pass_set },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        snprintf(name, sizeof(name), "%s %s", label, tests[i].what);
        bench_metric(ctx, name, bandwidth(tests[i].fn, a, b, bytes), "MB/s");
    }
    snprintf(name, sizeof(name), "%s random read latency", label);
    bench_metric(ctx, name, random_read_ns(a, bytes / 4), "ns/access");

    size_t t = test_bytes < bytes ? test_bytes : bytes;
    size_t errs = integrity(a, t / 4);
    snprintf(name, sizeof(name), "%s integrity test", label);
    bench_text(ctx, name, "%s (%u KB tested)", errs ? "FAIL" : "PASS", (unsigned)(t / 1024));
    if (errs) bench_fail(ctx, "%s: %u mismatching words", label, (unsigned)errs);

    heap_caps_free(a);
    heap_caps_free(b);
}

static void run(bench_ctx_t *ctx)
{
    bench_metric(ctx, "Internal heap free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0, "KB");
    bench_metric(ctx, "Internal heap minimum ever free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0, "KB");
    bench_metric(ctx, "Internal largest free block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0, "KB");
    bench_metric(ctx, "DMA-capable free", heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024.0, "KB");

    test_region(ctx, "Internal RAM", MALLOC_CAP_INTERNAL, 32 * 1024, 32 * 1024);

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        bench_metric(ctx, "PSRAM total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024.0, "KB");
        bench_metric(ctx, "PSRAM free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0, "KB");
        /* Larger than the cache so the numbers reflect the external bus. */
        test_region(ctx, "PSRAM", MALLOC_CAP_SPIRAM, 512 * 1024, 256 * 1024);
    } else {
        bench_note(ctx, "No PSRAM detected (enable it in menuconfig on boards that have it).");
    }
}

const bench_def_t bench_def_ram = {
    .id = "ram",
    .name = "RAM",
    .group = "Memory",
    .desc = "Heap statistics, read/write/copy bandwidth, random-access latency and a pattern integrity test (internal RAM and PSRAM).",
    .available = NULL,
    .run = run,
};
