/* UART benchmark: internal loopback (TX->RX inside the peripheral), so no
 * wiring is needed. Checks data integrity and effective throughput at
 * several baud rates. */
#include "bench.h"

#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buses.h"

#define PATTERN_BYTES 512

static void run(bench_ctx_t *ctx)
{
    static const uint32_t bauds[] = { 115200, 921600, 3000000 };
    uint32_t saved = 115200;
    uart_get_baudrate(BUS_UART_PORT, &saved);

    uint8_t tx[PATTERN_BYTES], rx[PATTERN_BYTES];
    for (int i = 0; i < PATTERN_BYTES; i++) tx[i] = (uint8_t)(i * 7 + 3);

    if (uart_set_loop_back(BUS_UART_PORT, true) != ESP_OK) {
        bench_skip(ctx, "UART internal loopback not available.");
        return;
    }

    for (size_t b = 0; b < sizeof(bauds) / sizeof(bauds[0]); b++) {
        char name[48];
        if (uart_set_baudrate(BUS_UART_PORT, bauds[b]) != ESP_OK) continue;
        uint32_t actual = 0;
        uart_get_baudrate(BUS_UART_PORT, &actual);
        uart_flush_input(BUS_UART_PORT);

        int64_t t0 = bench_now_us();
        uart_write_bytes(BUS_UART_PORT, tx, PATTERN_BYTES);
        int got = 0;
        while (got < PATTERN_BYTES && bench_now_us() - t0 < 1500000) {
            int n = uart_read_bytes(BUS_UART_PORT, rx + got, PATTERN_BYTES - got, pdMS_TO_TICKS(20));
            if (n > 0) got += n;
        }
        int64_t dt = bench_now_us() - t0;

        bool ok = got == PATTERN_BYTES && memcmp(tx, rx, PATTERN_BYTES) == 0;
        snprintf(name, sizeof(name), "%u baud loopback", (unsigned)bauds[b]);
        bench_text(ctx, name, "%s (%d/%d bytes)", ok ? "PASS" : "FAIL", got, PATTERN_BYTES);
        if (ok) {
            snprintf(name, sizeof(name), "%u baud effective rate", (unsigned)bauds[b]);
            bench_metric(ctx, name, got * 10.0 * 1e6 / (double)dt / 1000.0, "kbit/s");
            if (actual != bauds[b]) {
                snprintf(name, sizeof(name), "%u baud divider error", (unsigned)bauds[b]);
                bench_metric(ctx, name, ((double)actual - bauds[b]) * 100.0 / bauds[b], "%");
            }
        } else {
            bench_fail(ctx, "UART loopback failed at %u baud (%d/%d bytes)", (unsigned)bauds[b], got, PATTERN_BYTES);
            if (got == 0 && b == 0) bench_note(ctx, "Nothing came back. Real chips loop TX to RX internally; emulators such as QEMU do not.");
        }
    }

    uart_set_loop_back(BUS_UART_PORT, false);
    uart_set_baudrate(BUS_UART_PORT, saved);
    uart_flush_input(BUS_UART_PORT);
}

const bench_def_t bench_def_uart = {
    .id = "uart",
    .name = "UART",
    .group = "Peripherals",
    .desc = "Internal TX->RX loopback at 115200 / 921600 / 3000000 baud: integrity and effective throughput.",
    .available = NULL,
    .run = run,
};
