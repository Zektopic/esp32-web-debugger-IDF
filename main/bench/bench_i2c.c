/* I2C benchmark: bus scan with timing. Without any device attached it still
 * exercises the controller (every address must NACK cleanly). */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "buses.h"

static void run(bench_ctx_t *ctx)
{
    uint8_t addrs[126];
    int64_t t0 = bench_now_us();
    int n = buses_i2c_scan(addrs, sizeof(addrs));
    int64_t dt = bench_now_us() - t0;

    if (n < 0) {
        bench_fail(ctx, "I2C bus is not available (pin conflict?)");
        return;
    }
    bench_text(ctx, "Pins", "SDA GPIO%d, SCL GPIO%d", CONFIG_WEBDBG_I2C_SDA_GPIO, CONFIG_WEBDBG_I2C_SCL_GPIO);
    bench_metric(ctx, "Full scan (0x01-0x7E)", dt / 1000.0, "ms");
    bench_metric(ctx, "Time per address", dt / 126.0, "us");
    bench_metric(ctx, "Devices found", n, "");

    char list[128] = "";
    size_t l = 0;
    for (int i = 0; i < n && l < sizeof(list) - 6; i++) {
        l += snprintf(list + l, sizeof(list) - l, "%s0x%02X", i ? ", " : "", addrs[i]);
    }
    if (n) bench_text(ctx, "Addresses", "%s", list);
    else bench_note(ctx, "No devices answered; connect a sensor/display (with pull-ups) to see addresses.");
}

const bench_def_t bench_def_i2c = {
    .id = "i2c",
    .name = "I2C",
    .group = "Peripherals",
    .desc = "Scans the I2C bus and reports timing and responding addresses.",
    .available = NULL,
    .run = run,
};
