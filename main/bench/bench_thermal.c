/* Thermal benchmark: on-die temperature at idle, then while every core is
 * fully loaded, so you can see how fast the chip heats up. */
#include "bench.h"

#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "temp_sensor.h"

static volatile bool s_stop;
static volatile uint32_t s_sink;

static void load_task(void *arg)
{
    uint32_t x = 1;
    TaskHandle_t waiter = arg;
    while (!s_stop) {
        /* ~10 ms of integer work, then 1 tick of sleep so the idle task still runs */
        int64_t until = bench_now_us() + 10000;
        while (bench_now_us() < until) {
            for (int i = 0; i < 200; i++) x = x * 1664525u + 1013904223u;
        }
        vTaskDelay(1);
    }
    s_sink += x;
    xTaskNotifyGive(waiter);
    vTaskDelete(NULL);
}

static bool available(const char **reason)
{
    if (temp_sensor_available()) return true;
    *reason = "This chip has no on-die temperature sensor (or it failed to initialise).";
    return false;
}

static float read_avg(int n)
{
    float sum = 0;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        float t;
        if (temp_sensor_read_c(&t) == ESP_OK) {
            sum += t;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ok ? sum / ok : -273.15f;
}

static void run(bench_ctx_t *ctx)
{
    int secs = bench_param_int(ctx, "seconds", 8);
    if (secs < 2) secs = 2;
    if (secs > 30) secs = 30;

    float idle = read_avg(5);
    bench_metric(ctx, "Idle temperature", idle, "C");

    s_stop = false;
    for (int c = 0; c < SOC_CPU_CORES_NUM; c++) {
        xTaskCreatePinnedToCore(load_task, "thermal_load", 2048, xTaskGetCurrentTaskHandle(), 2, NULL, c);
    }

    float peak = idle, last = idle;
    for (int s = 0; s < secs * 2; s++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        float t;
        if (temp_sensor_read_c(&t) == ESP_OK) {
            last = t;
            if (t > peak) peak = t;
        }
    }

    s_stop = true;
    for (int c = 0; c < SOC_CPU_CORES_NUM; c++) ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

    bench_metric(ctx, "Load duration", secs, "s");
    bench_metric(ctx, "Temperature under load (end)", last, "C");
    bench_metric(ctx, "Peak temperature", peak, "C");
    bench_metric(ctx, "Rise under load", last - idle, "C");
    bench_metric(ctx, "Heating rate", (last - idle) / secs, "C/s");

    vTaskDelay(pdMS_TO_TICKS(1000));
    bench_metric(ctx, "Temperature 1 s after load", read_avg(3), "C");
    bench_note(ctx, "On-die sensor: good for trends, absolute accuracy is only a few degrees.");
}

const bench_def_t bench_def_thermal = {
    .id = "thermal",
    .name = "CPU temperature",
    .group = "Thermal",
    .desc = "Reads the on-die temperature sensor at idle, under all-core load and during cool-down.",
    .available = available,
    .run = run,
};
