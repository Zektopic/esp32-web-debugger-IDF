/* GPIO benchmark. Uses the pin's own output->input path (GPIO_MODE_INPUT_OUTPUT)
 * so no wiring is needed. Everything is opt-in per pin because it drives the
 * pin: pick one that has nothing attached, or enable the full sweep. */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio_mgr.h"

#define LATENCY_SAMPLES 200

static volatile uint32_t s_isr_cycles;
static volatile bool     s_isr_fired;

static void IRAM_ATTR edge_isr(void *arg)
{
    s_isr_cycles = esp_cpu_get_cycle_count();
    s_isr_fired = true;
}

static void pin_tests(bench_ctx_t *ctx, int pin)
{
    char name[64];

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);

    /* 1. loopback levels */
    int errors = 0;
    for (int i = 0; i < 100; i++) {
        gpio_set_level(pin, i & 1);
        if (gpio_get_level(pin) != (i & 1)) errors++;
    }
    snprintf(name, sizeof(name), "GPIO%d output->input loopback", pin);
    bench_text(ctx, name, "%s", errors ? "FAIL (pin held by external circuit?)" : "PASS");
    if (errors) bench_fail(ctx, "GPIO%d did not follow its own output (%d/100 errors)", pin, errors);

    /* 2. toggle rate through the driver API */
    const int toggles = 200000;
    int64_t t0 = bench_now_us();
    for (int i = 0; i < toggles; i++) gpio_set_level(pin, i & 1);
    int64_t dt = bench_now_us() - t0;
    bench_metric(ctx, "gpio_set_level toggle rate", toggles / (double)dt, "MHz");

    int64_t t1 = bench_now_us();
    volatile int sink = 0;
    for (int i = 0; i < toggles; i++) sink += gpio_get_level(pin);
    dt = bench_now_us() - t1;
    bench_metric(ctx, "gpio_get_level read rate", toggles / (double)dt, "MHz");

    /* 3. interrupt latency: write -> ISR entry, measured in CPU cycles */
    gpio_set_level(pin, 0);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        bench_note(ctx, "GPIO ISR service unavailable: %s", esp_err_to_name(err));
        return;
    }
    gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(pin, edge_isr, NULL);
    gpio_intr_enable(pin);

    uint32_t min_c = UINT32_MAX, max_c = 0;
    uint64_t sum = 0;
    int got = 0;
    for (int i = 0; i < LATENCY_SAMPLES; i++) {
        s_isr_fired = false;
        uint32_t c0 = esp_cpu_get_cycle_count();
        gpio_set_level(pin, (i + 1) & 1);
        int64_t wait0 = bench_now_us();
        while (!s_isr_fired && bench_now_us() - wait0 < 2000) { }
        if (!s_isr_fired) continue;
        uint32_t c = s_isr_cycles - c0;
        if (c < min_c) min_c = c;
        if (c > max_c) max_c = c;
        sum += c;
        got++;
        vTaskDelay(1);
    }
    gpio_intr_disable(pin);
    gpio_isr_handler_remove(pin);

    double mhz = esp_rom_get_cpu_ticks_per_us();
    if (got > 0) {
        bench_metric(ctx, "Interrupt latency (avg)", (double)sum / got / mhz, "us");
        bench_metric(ctx, "Interrupt latency (best)", min_c / mhz, "us");
        bench_metric(ctx, "Interrupt latency (worst)", max_c / mhz, "us");
    } else {
        bench_note(ctx, "No GPIO interrupts were received.");
    }
}

/* Visits every manageable pin; reports the ones that cannot hold a level. */
static void sweep(bench_ctx_t *ctx)
{
    char bad[96] = "";
    int tested = 0, failed = 0;
    for (int i = 0; i < gpio_mgr_count(); i++) {
        int pin = gpio_mgr_gpio_at(i);
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) continue;
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
        int errs = 0;
        for (int k = 0; k < 8; k++) {
            gpio_set_level(pin, k & 1);
            esp_rom_delay_us(5);
            if (gpio_get_level(pin) != (k & 1)) errs++;
        }
        tested++;
        if (errs) {
            failed++;
            size_t l = strlen(bad);
            snprintf(bad + l, sizeof(bad) - l, "%sGPIO%d", l ? ", " : "", pin);
        }
        gpio_mgr_set_mode(pin, PIN_MODE_INPUT); /* restore the manager's view */
    }
    bench_text(ctx, "Pin sweep", "%d tested, %d failed%s%s", tested, failed, failed ? ": " : "", bad);
    if (failed) bench_fail(ctx, "%d pin(s) did not follow their output", failed);
}

static void run(bench_ctx_t *ctx)
{
    int pin = bench_param_int(ctx, "pin", -1);
    const char *sweep_s = bench_param_str(ctx, "sweep", "0");
    bool do_sweep = strcmp(sweep_s, "1") == 0 || strcmp(sweep_s, "true") == 0;

    if (pin < 0 && !do_sweep) {
        bench_skip(ctx, "Choose a free GPIO (nothing attached) or enable the sweep; the test drives the pin.");
        return;
    }
    if (pin >= 0) {
        if (!gpio_mgr_is_usable(pin) || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            bench_skip(ctx, "GPIO%d is reserved or input-only on this chip.", pin);
        } else {
            pin_tests(ctx, pin);
            gpio_mgr_set_mode(pin, PIN_MODE_INPUT);
        }
    }
    if (do_sweep) sweep(ctx);
}

const bench_def_t bench_def_gpio = {
    .id = "gpio",
    .name = "GPIO",
    .group = "Peripherals",
    .desc = "Loopback check, toggle/read rate and interrupt latency on a chosen pin; optional sweep of every pin. Drives the pin!",
    .available = NULL,
    .run = run,
};
