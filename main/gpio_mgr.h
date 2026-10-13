#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/ledc.h"
#include "cJSON.h"

/* The highest LEDC timer/channel is kept out of the PWM pool so the LED
 * benchmark can always use it. */
#define BENCH_LEDC_TIMER   ((ledc_timer_t)(LEDC_TIMER_MAX - 1))
#define BENCH_LEDC_CHANNEL ((ledc_channel_t)(LEDC_CHANNEL_MAX - 1))

typedef enum {
    PIN_MODE_INPUT = 0,
    PIN_MODE_OUTPUT,
    PIN_MODE_PWM,
} pin_mode_t;

/* Builds the table of pins that are safe to poke on the running chip
 * (flash/PSRAM, console, USB and the UART/I2C bus pins are excluded). */
void gpio_mgr_init(void);

int  gpio_mgr_count(void);
int  gpio_mgr_gpio_at(int index);
bool gpio_mgr_is_usable(int gpio);

esp_err_t gpio_mgr_set_mode(int gpio, pin_mode_t mode);
esp_err_t gpio_mgr_write(int gpio, bool level);
/* freq in Hz, duty in percent (0..100); switches the pin to PWM mode */
esp_err_t gpio_mgr_set_pwm(int gpio, uint32_t freq_hz, uint32_t duty_pct);

/* Debounced input sampling; returns true if any input changed. */
bool gpio_mgr_poll(void);

/* {"pins":[...]} - caller frees with cJSON_free / free */
char *gpio_mgr_snapshot_json(void);

/* True if the pin is one of the bus pins the manager deliberately hides. */
bool gpio_mgr_is_bus_pin(int gpio);
