/* LED benchmark: a PWM fade on a plain LED (LEDC) and a colour sequence on an
 * addressable WS2812 LED (RMT). Both are visual checks; the RMT part also
 * verifies that the frame really takes as long as the WS2812 timing demands. */
#include "bench.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio_mgr.h"

#if SOC_RMT_SUPPORTED
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#endif

/* ------------------------------------------------------------ plain LED */

static void led_fade(bench_ctx_t *ctx, int gpio)
{
    if (!gpio_mgr_is_usable(gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        bench_note(ctx, "GPIO%d is reserved (flash/console/bus) or input-only here, it cannot drive an LED.", gpio);
        return;
    }
    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BENCH_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ccfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BENCH_LEDC_CHANNEL,
        .timer_sel = BENCH_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_timer_config(&tcfg) != ESP_OK || ledc_channel_config(&ccfg) != ESP_OK) {
        bench_fail(ctx, "LEDC setup failed for GPIO%d", gpio);
        return;
    }

    int64_t worst = 0;
    for (int dir = 0; dir < 2; dir++) {
        for (int step = 0; step <= 64; step++) {
            uint32_t duty = (dir == 0 ? step : 64 - step) * 1023 / 64;
            int64_t t0 = bench_now_us();
            ledc_set_duty(LEDC_LOW_SPEED_MODE, BENCH_LEDC_CHANNEL, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, BENCH_LEDC_CHANNEL);
            int64_t dt = bench_now_us() - t0;
            if (dt > worst) worst = dt;
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, BENCH_LEDC_CHANNEL, 0);
    gpio_reset_pin(gpio);
    if (gpio_mgr_is_usable(gpio)) gpio_mgr_set_mode(gpio, PIN_MODE_INPUT);

    bench_text(ctx, "Plain LED fade", "GPIO%d: 2 s PWM breathe @ 5 kHz / 10-bit (watch the LED)", gpio);
    bench_metric(ctx, "LEDC duty update (worst)", (double)worst, "us");
}

/* ------------------------------------------------------------- WS2812 */

#if SOC_RMT_SUPPORTED

#define RMT_RES_HZ 10000000 /* 0.1 us ticks */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws_encoder_t;

static size_t ws_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *data,
                        size_t size, rmt_encode_state_t *ret_state)
{
    ws_encoder_t *e = __containerof(encoder, ws_encoder_t, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET, state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    switch (e->state) {
    case 0:
        encoded += e->bytes_encoder->encode(e->bytes_encoder, channel, data, size, &session);
        if (session & RMT_ENCODING_COMPLETE) e->state = 1;
        if (session & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* fall through */
    case 1:
        encoded += e->copy_encoder->encode(e->copy_encoder, channel, &e->reset_code, sizeof(e->reset_code), &session);
        if (session & RMT_ENCODING_COMPLETE) {
            e->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session & RMT_ENCODING_MEM_FULL) state |= RMT_ENCODING_MEM_FULL;
    }
out:
    *ret_state = state;
    return encoded;
}

static esp_err_t ws_reset(rmt_encoder_t *encoder)
{
    ws_encoder_t *e = __containerof(encoder, ws_encoder_t, base);
    rmt_encoder_reset(e->bytes_encoder);
    rmt_encoder_reset(e->copy_encoder);
    e->state = 0;
    return ESP_OK;
}

static esp_err_t ws_del(rmt_encoder_t *encoder)
{
    ws_encoder_t *e = __containerof(encoder, ws_encoder_t, base);
    rmt_del_encoder(e->bytes_encoder);
    rmt_del_encoder(e->copy_encoder);
    free(e);
    return ESP_OK;
}

static esp_err_t ws_encoder_new(rmt_encoder_handle_t *out)
{
    ws_encoder_t *e = calloc(1, sizeof(*e));
    if (!e) return ESP_ERR_NO_MEM;
    e->base.encode = ws_encode;
    e->base.reset = ws_reset;
    e->base.del = ws_del;

    const rmt_bytes_encoder_config_t bcfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 }, /* 0.3 us high, 0.9 us low */
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 }, /* 0.9 us high, 0.3 us low */
        .flags.msb_first = 1,
    };
    const rmt_copy_encoder_config_t ccfg = {};
    esp_err_t err = rmt_new_bytes_encoder(&bcfg, &e->bytes_encoder);
    if (err == ESP_OK) err = rmt_new_copy_encoder(&ccfg, &e->copy_encoder);
    if (err != ESP_OK) {
        if (e->bytes_encoder) rmt_del_encoder(e->bytes_encoder);
        free(e);
        return err;
    }
    uint32_t reset_ticks = RMT_RES_HZ / 1000000 * 60 / 2; /* 60 us low, split over two halves */
    e->reset_code = (rmt_symbol_word_t){ .level0 = 0, .duration0 = reset_ticks, .level1 = 0, .duration1 = reset_ticks };
    *out = &e->base;
    return ESP_OK;
}

static void rgb_sequence(bench_ctx_t *ctx, int gpio, int pixels)
{
    if (!gpio_mgr_is_usable(gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        bench_note(ctx, "GPIO%d is reserved (flash/console/bus) or input-only here, it cannot drive a WS2812.", gpio);
        return;
    }
    rmt_channel_handle_t chan = NULL;
    rmt_encoder_handle_t enc = NULL;
    uint8_t *frame = calloc(pixels, 3);
    if (!frame) return;

    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &chan);
    if (err == ESP_OK) err = ws_encoder_new(&enc);
    if (err == ESP_OK) err = rmt_enable(chan);
    if (err != ESP_OK) {
        bench_fail(ctx, "RMT setup failed on GPIO%d: %s", gpio, esp_err_to_name(err));
        goto cleanup;
    }

    static const uint8_t colours[][3] = { /* GRB order */
        { 0, 24, 0 }, { 24, 0, 0 }, { 0, 0, 24 }, { 16, 16, 16 }, { 0, 0, 0 },
    };
    const rmt_transmit_config_t tx = { .loop_count = 0 };
    int64_t worst = 0, best = INT64_MAX;
    for (size_t c = 0; c < sizeof(colours) / sizeof(colours[0]); c++) {
        for (int p = 0; p < pixels; p++) memcpy(&frame[p * 3], colours[c], 3);
        int64_t t0 = bench_now_us();
        err = rmt_transmit(chan, enc, frame, pixels * 3, &tx);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(chan, 1000);
        int64_t dt = bench_now_us() - t0;
        if (err != ESP_OK) {
            bench_fail(ctx, "RMT transmit failed: %s", esp_err_to_name(err));
            break;
        }
        if (dt > worst) worst = dt;
        if (dt < best) best = dt;
        vTaskDelay(pdMS_TO_TICKS(350));
    }

    if (err == ESP_OK) {
        double expected = pixels * 24 * 1.25 + 60.0; /* us */
        bench_text(ctx, "WS2812 sequence", "GPIO%d: red, green, blue, white, off on %d pixel(s) (watch the LED)", gpio, pixels);
        bench_metric(ctx, "Frame time (best)", (double)best, "us");
        bench_metric(ctx, "Frame time (theoretical)", expected, "us");
        bench_metric(ctx, "RMT bit rate", pixels * 24.0 / ((double)best / 1e6) / 1000.0, "kbit/s");
    }

cleanup:
    if (chan) {
        rmt_disable(chan);
        rmt_del_channel(chan);
    }
    if (enc) rmt_del_encoder(enc);
    free(frame);
    gpio_reset_pin(gpio);
    if (gpio_mgr_is_usable(gpio)) gpio_mgr_set_mode(gpio, PIN_MODE_INPUT);
}

#endif /* SOC_RMT_SUPPORTED */

static void run(bench_ctx_t *ctx)
{
    int led = bench_param_int(ctx, "led_gpio", CONFIG_WEBDBG_BENCH_LED_GPIO);
    int rgb = bench_param_int(ctx, "rgb_gpio", CONFIG_WEBDBG_BENCH_RGB_GPIO);
    int pixels = bench_param_int(ctx, "pixels", 1);
    if (pixels < 1) pixels = 1;
    if (pixels > 64) pixels = 64;

    if (led < 0 && rgb < 0) {
        bench_skip(ctx, "No LED GPIO configured. Enter the pin of your LED (and/or WS2812) and run again.");
        return;
    }
    if (led >= 0) led_fade(ctx, led);
    if (rgb >= 0) {
#if SOC_RMT_SUPPORTED
        rgb_sequence(ctx, rgb, pixels);
#else
        bench_note(ctx, "This chip has no RMT peripheral, so the WS2812 test is unavailable.");
#endif
    }
}

const bench_def_t bench_def_led = {
    .id = "led",
    .name = "LEDs",
    .group = "Peripherals",
    .desc = "PWM-fades a plain LED (LEDC) and drives a WS2812 RGB LED (RMT) through a colour sequence. Visual check plus timing.",
    .available = NULL,
    .run = run,
};
