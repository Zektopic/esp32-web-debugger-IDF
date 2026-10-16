/* ULP / LP-core benchmark: loads a tiny program into the low-power
 * coprocessor, lets it run for a while and checks from the main CPU that it
 * executed and kept time. The coprocessor flavour (FSM, RISC-V, LP core) is
 * picked by the target's sdkconfig.defaults.<target>. */
#include "bench.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ULP_COPROC_ENABLED

#include "ulp_main.h" /* generated from the ULP program: ulp_counter, ulp_magic, ... */

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");

#define RUN_MS 500

#if CONFIG_ULP_COPROC_TYPE_FSM
#include "ulp.h"
#include "soc/rtc_cntl_reg.h"
#define ULP_KIND "ULP-FSM"
#elif CONFIG_ULP_COPROC_TYPE_RISCV
#include "ulp_riscv.h"
#define ULP_KIND "ULP-RISC-V"
#elif CONFIG_ULP_COPROC_TYPE_LP_CORE
#include "ulp_lp_core.h"
#define ULP_KIND "LP core"
#endif

static esp_err_t ulp_start(void)
{
    size_t size = ulp_main_bin_end - ulp_main_bin_start;
#if CONFIG_ULP_COPROC_TYPE_FSM
    esp_err_t err = ulp_load_binary(0, ulp_main_bin_start, size / sizeof(uint32_t));
    if (err != ESP_OK) return err;
    ulp_set_wakeup_period(0, 1000); /* one run per millisecond */
    return ulp_run(&ulp_entry - RTC_SLOW_MEM);
#elif CONFIG_ULP_COPROC_TYPE_RISCV
    esp_err_t err = ulp_riscv_load_binary(ulp_main_bin_start, size);
    if (err != ESP_OK) return err;
    ulp_mode = 0;
    ulp_set_wakeup_period(0, 20000);
    return ulp_riscv_run();
#elif CONFIG_ULP_COPROC_TYPE_LP_CORE
    esp_err_t err = ulp_lp_core_load_binary(ulp_main_bin_start, size);
    if (err != ESP_OK) return err;
    ulp_mode = 0;
    ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
    return ulp_lp_core_run(&cfg);
#endif
}

static void ulp_halt(void)
{
#if CONFIG_ULP_COPROC_TYPE_FSM
    ulp_timer_stop();
#elif CONFIG_ULP_COPROC_TYPE_RISCV
    ulp_riscv_timer_stop();
    ulp_riscv_halt();
#elif CONFIG_ULP_COPROC_TYPE_LP_CORE
    ulp_lp_core_stop();
#endif
}

static void run(bench_ctx_t *ctx)
{
    esp_err_t err = ulp_start();
    if (err != ESP_OK) {
        bench_fail(ctx, "could not start the coprocessor: %s", esp_err_to_name(err));
        return;
    }
    bench_text(ctx, "Coprocessor", "%s, program %u bytes", ULP_KIND, (unsigned)(ulp_main_bin_end - ulp_main_bin_start));

    int64_t t0 = bench_now_us();
    vTaskDelay(pdMS_TO_TICKS(RUN_MS));
    double elapsed_ms = (bench_now_us() - t0) / 1000.0;

#if CONFIG_ULP_COPROC_TYPE_FSM
    uint32_t ticks = ulp_counter & 0xFFFF;
    bool alive = (ulp_magic & 0xFFFF) == 0xA5A5;
#else
    uint32_t ticks = ulp_counter;
    bool alive = ulp_magic == 0xB0DE;
#endif
    bench_text(ctx, "Program executed", "%s", alive ? "PASS (marker seen in RTC memory)" : "FAIL");
    if (!alive) bench_fail(ctx, "coprocessor did not run (no marker in shared memory)");

    bench_metric(ctx, "1 ms ticks counted", ticks, "ticks");
    bench_metric(ctx, "Elapsed time (main CPU)", elapsed_ms, "ms");
    if (elapsed_ms > 0) {
        bench_metric(ctx, "Coprocessor timekeeping", ticks / elapsed_ms * 100.0, "% of real time");
    }

#if CONFIG_ULP_COPROC_TYPE_RISCV || CONFIG_ULP_COPROC_TYPE_LP_CORE
    /* Switch the program to its free-running loop and measure the loop rate. */
    uint32_t spin0 = ulp_spin;
    ulp_mode = 1;
    t0 = bench_now_us();
    vTaskDelay(pdMS_TO_TICKS(300));
    double dt = (double)(bench_now_us() - t0) / 1e6;
    bench_metric(ctx, "Free-running loop rate", (ulp_spin - spin0) / dt / 1e6, "M loops/s");
#endif

    ulp_halt();
}

static bool available(const char **reason)
{
    (void)reason;
    return true;
}

#else /* ULP not enabled in this build */

static bool available(const char **reason)
{
#if SOC_ULP_SUPPORTED || SOC_LP_CORE_SUPPORTED
    *reason = "The ULP/LP core is supported by this chip but disabled in this build (CONFIG_ULP_COPROC_ENABLED).";
#else
    *reason = "This chip has no ULP / LP coprocessor.";
#endif
    return false;
}

static void run(bench_ctx_t *ctx) { (void)ctx; }

#endif

const bench_def_t bench_def_ulp = {
    .id = "ulp",
    .name = "ULP / LP coprocessor",
    .group = "Peripherals",
    .desc = "Loads a program into the low-power coprocessor and verifies it runs and keeps time (FSM, RISC-V or LP core, depending on the chip).",
    .available = available,
    .run = run,
};
