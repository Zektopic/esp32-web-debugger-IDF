/* CPU benchmark: portable integer / floating point / memory-bound kernels,
 * run on every core. Kernels are calibrated to ~TARGET_US each so the same
 * code gives meaningful numbers from a 96 MHz C2 to a 240 MHz dual-core S3. */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TARGET_US 80000
#define REPEATS   3

static volatile uint32_t s_sink; /* defeats dead-code elimination */

/* ------------------------------------------------------------------ kernels */

#define KERNEL __attribute__((noinline)) static uint32_t

KERNEL k_int_alu(uint32_t n)
{
    uint32_t x = 0x12345678u, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        acc += x + i;
    }
    return acc;
}

KERNEL k_int_muldiv(uint32_t n)
{
    uint32_t x = 1, acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        acc += (x >> 3) / ((i & 0xFF) + 1);
    }
    return acc;
}

KERNEL k_float32(uint32_t n)
{
    float a = 1.0f, b = 0.5f;
    for (uint32_t i = 0; i < n; i++) {
        a = a * 1.0000001f + b;
        b = b * 0.9999999f + 1e-7f * a;
    }
    return (uint32_t)(int32_t)(a + b);
}

KERNEL k_float64(uint32_t n)
{
    double a = 1.0, b = 0.5;
    for (uint32_t i = 0; i < n; i++) {
        a = a * 1.0000001 + b;
        b = b * 0.9999999 + 1e-7 * a;
    }
    return (uint32_t)(int64_t)(a + b);
}

/* n = number of 16x16 integer matrix products (4096 multiply-accumulates each) */
KERNEL k_matmul(uint32_t n)
{
    int32_t A[16][16], B[16][16], C[16][16];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            A[i][j] = i * 3 + j;
            B[i][j] = i - j * 2;
        }
    }
    uint32_t acc = 0;
    for (uint32_t r = 0; r < n; r++) {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                int32_t s = 0;
                for (int k = 0; k < 16; k++) s += A[i][k] * B[k][j];
                C[i][j] = s;
            }
        }
        A[0][0] = C[r & 15][(r >> 4) & 15] & 0xFF; /* keep the dependency chain alive */
        acc += (uint32_t)C[15][15];
    }
    return acc;
}

/* n = bytes through a bitwise CRC-32 */
KERNEL k_crc32(uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint8_t)(i * 31u);
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* n = shell sorts of 256 pseudo-random integers */
KERNEL k_sort(uint32_t n)
{
    int32_t a[256];
    uint32_t seed = 7, acc = 0;
    for (uint32_t r = 0; r < n; r++) {
        for (int i = 0; i < 256; i++) {
            seed = seed * 1103515245u + 12345u;
            a[i] = (int32_t)(seed >> 8);
        }
        for (int gap = 109; gap > 0; gap = (gap == 109) ? 41 : (gap == 41) ? 19 : (gap == 19) ? 5 : (gap == 5) ? 1 : 0) {
            for (int i = gap; i < 256; i++) {
                int32_t t = a[i];
                int j = i;
                while (j >= gap && a[j - gap] > t) {
                    a[j] = a[j - gap];
                    j -= gap;
                }
                a[j] = t;
            }
        }
        acc += (uint32_t)a[128];
    }
    return acc;
}

typedef struct {
    const char *name;
    uint32_t (*fn)(uint32_t);
    double scale;      /* work units per iteration, in millions */
    const char *unit;
} kernel_t;

static const kernel_t s_kernels[] = {
    { "Integer ALU",         k_int_alu,    1e-6,     "M iter/s" },
    { "Integer mul/div",     k_int_muldiv, 1e-6,     "M iter/s" },
    { "Float32 (FPU or soft)", k_float32,  1e-6,     "M iter/s" },
    { "Float64 (software)",  k_float64,    1e-6,     "M iter/s" },
    { "Matrix 16x16 MAC",    k_matmul,     4096e-6,  "M MAC/s" },
    { "CRC-32 (bitwise)",    k_crc32,      1e-6,     "MB/s" },
    { "Shell sort 256",      k_sort,       1e-3,     "k sorts/s" },
};
#define NUM_KERNELS (sizeof(s_kernels) / sizeof(s_kernels[0]))

/* Finds an iteration count worth ~TARGET_US, then returns the best of REPEATS
 * runs expressed as iterations per second. */
static double measure(uint32_t (*fn)(uint32_t))
{
    uint32_t n = 16;
    int64_t dt;
    for (;;) {
        int64_t t0 = bench_now_us();
        s_sink += fn(n);
        dt = bench_now_us() - t0;
        if (dt >= 4000 || n >= (1u << 30)) break;
        n *= 2;
    }
    if (dt < 1) dt = 1;
    n = (uint32_t)((double)n * TARGET_US / (double)dt);
    if (n < 1) n = 1;

    double best = 0;
    for (int r = 0; r < REPEATS; r++) {
        int64_t t0 = bench_now_us();
        s_sink += fn(n);
        int64_t t = bench_now_us() - t0;
        if (t < 1) t = 1;
        double rate = (double)n * 1e6 / (double)t;
        if (rate > best) best = rate;
        vTaskDelay(1); /* let the idle task (and the watchdog) breathe */
    }
    return best;
}

/* ---------------------------------------------------------- per-core worker */

typedef struct {
    TaskHandle_t waiter;
    double rate[NUM_KERNELS];
    int core;
    int kernel_only; /* -1 = all kernels, else index (used for the parallel run) */
} worker_arg_t;

static void worker(void *p)
{
    worker_arg_t *a = p;
    for (size_t k = 0; k < NUM_KERNELS; k++) {
        if (a->kernel_only >= 0 && (int)k != a->kernel_only) continue;
        a->rate[k] = measure(s_kernels[k].fn);
    }
    xTaskNotifyGive(a->waiter);
    vTaskDelete(NULL);
}

static void run_on_cores(worker_arg_t *args, int count, int first_core)
{
    for (int i = 0; i < count; i++) {
        args[i].waiter = xTaskGetCurrentTaskHandle();
        args[i].core = first_core + i;
        xTaskCreatePinnedToCore(worker, "bench_cpu", 6144, &args[i], configMAX_PRIORITIES - 3,
                                NULL, first_core + i);
    }
    /* pdFALSE: take one notification at a time, several workers may finish together */
    for (int i = 0; i < count; i++) ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
}

/* Compares the cycle counter with the wall clock to expose the real CPU clock. */
static double measured_mhz(void)
{
    uint32_t c0 = esp_cpu_get_cycle_count();
    int64_t t0 = bench_now_us();
    while (bench_now_us() - t0 < 100000) { }
    uint32_t cycles = esp_cpu_get_cycle_count() - c0;
    int64_t dt = bench_now_us() - t0;
    return (double)cycles / (double)dt;
}

static void run(bench_ctx_t *ctx)
{
    const int cores = SOC_CPU_CORES_NUM;
    static worker_arg_t args[SOC_CPU_CORES_NUM];
    memset(args, 0, sizeof(args));
    for (int i = 0; i < cores; i++) args[i].kernel_only = -1;

    bench_metric(ctx, "Configured clock", esp_rom_get_cpu_ticks_per_us(), "MHz");
    bench_metric(ctx, "Measured clock (cycle counter)", measured_mhz(), "MHz");

    /* Each core on its own, one after the other. */
    for (int c = 0; c < cores; c++) {
        run_on_cores(&args[c], 1, c);
    }

    for (size_t k = 0; k < NUM_KERNELS; k++) {
        for (int c = 0; c < cores; c++) {
            char name[48];
            if (cores > 1) snprintf(name, sizeof(name), "%s [core %d]", s_kernels[k].name, c);
            else snprintf(name, sizeof(name), "%s", s_kernels[k].name);
            bench_metric(ctx, name, args[c].rate[k] * s_kernels[k].scale, s_kernels[k].unit);
        }
    }

    double mhz = esp_rom_get_cpu_ticks_per_us();
    bench_metric(ctx, "Integer ALU per MHz", args[0].rate[0] * 1e-6 / mhz * 1000.0, "k iter/s per MHz");
    bench_metric(ctx, "Float32 per MHz", args[0].rate[2] * 1e-6 / mhz * 1000.0, "k iter/s per MHz");

    /* Both cores at once: shows how well the second core scales. */
    if (cores > 1) {
        worker_arg_t par[SOC_CPU_CORES_NUM];
        memset(par, 0, sizeof(par));
        for (int i = 0; i < cores; i++) par[i].kernel_only = 0;
        run_on_cores(par, cores, 0);
        double sum = 0;
        for (int c = 0; c < cores; c++) sum += par[c].rate[0];
        bench_metric(ctx, "Integer ALU, all cores in parallel", sum * 1e-6, "M iter/s");
        bench_metric(ctx, "Multi-core scaling", sum / args[0].rate[0], "x");
    }

    bench_note(ctx, "Kernels are always compiled with -O2, whatever the project optimisation level.");
    bench_note(ctx, "Wi-Fi and web server keep running, so results can vary by a few percent.");
}

const bench_def_t bench_def_cpu = {
    .id = "cpu",
    .name = "CPU",
    .group = "Compute",
    .desc = "Integer, FPU/soft-float, matrix, CRC and sort kernels on every core; measures the real clock.",
    .available = NULL,
    .run = run,
};
