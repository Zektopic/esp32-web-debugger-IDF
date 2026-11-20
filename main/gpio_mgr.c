#include "gpio_mgr.h"

#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "gpio_mgr";

#define PWM_RESOLUTION     LEDC_TIMER_10_BIT
#define PWM_MAX_DUTY       ((1u << 10) - 1)
#define PWM_MIN_FREQ_HZ    1
#define PWM_MAX_FREQ_HZ    40000
#define DEBOUNCE_MS        20
#define PWM_TIMERS         (LEDC_TIMER_MAX - 1)
#define PWM_CHANNELS       (LEDC_CHANNEL_MAX - 1)

typedef struct {
    int      gpio;
    pin_mode_t mode;
    bool     level;
    bool     input_only;
    bool     strapping;
    uint32_t bounce;
    int64_t  last_change_ms;
    uint32_t pwm_freq;
    uint8_t  pwm_duty;
    int8_t   pwm_channel;
    int8_t   pwm_timer;
} pin_t;

static pin_t s_pins[SOC_GPIO_PIN_COUNT];
static int   s_count;
static SemaphoreHandle_t s_lock;

static struct { uint32_t freq; int refs; } s_timers[PWM_TIMERS > 0 ? PWM_TIMERS : 1];
static int8_t s_channel_owner[PWM_CHANNELS > 0 ? PWM_CHANNELS : 1];

/* ---------------------------------------------------------------- pin policy */

bool gpio_mgr_is_bus_pin(int p)
{
    return p == CONFIG_WEBDBG_UART_TX_GPIO || p == CONFIG_WEBDBG_UART_RX_GPIO ||
           p == CONFIG_WEBDBG_I2C_SDA_GPIO || p == CONFIG_WEBDBG_I2C_SCL_GPIO;
}

/* Pins that must never be touched: SPI flash / PSRAM, console UART, USB. */
static bool is_reserved(int p)
{
#if CONFIG_IDF_TARGET_ESP32
    if (p >= 6 && p <= 11) return true;            /* SPI flash */
    if (p == 1 || p == 3) return true;             /* UART0 console */
#if CONFIG_SPIRAM
    if (p == 16 || p == 17) return true;           /* PSRAM (WROVER) */
#endif
#elif CONFIG_IDF_TARGET_ESP32S2
    if (p >= 26 && p <= 32) return true;           /* SPI flash / PSRAM */
    if (p == 43 || p == 44) return true;           /* UART0 console */
    if (p == 19 || p == 20) return true;           /* USB D-/D+ */
#elif CONFIG_IDF_TARGET_ESP32S3
    if (p >= 26 && p <= 32) return true;           /* SPI flash / PSRAM */
#if CONFIG_SPIRAM_MODE_OCT || CONFIG_ESPTOOLPY_OCT_FLASH
    if (p >= 33 && p <= 37) return true;           /* octal flash / PSRAM */
#endif
    if (p == 43 || p == 44) return true;           /* UART0 console */
    if (p == 19 || p == 20) return true;           /* USB D-/D+ */
#elif CONFIG_IDF_TARGET_ESP32C3
    if (p >= 12 && p <= 17) return true;           /* SPI flash */
    if (p == 20 || p == 21) return true;           /* UART0 console */
    if (p == 18 || p == 19) return true;           /* USB D-/D+ */
#elif CONFIG_IDF_TARGET_ESP32C2
    if (p >= 12 && p <= 17) return true;           /* SPI flash */
    if (p == 19 || p == 20) return true;           /* UART0 console */
#elif CONFIG_IDF_TARGET_ESP32C6
    if (p >= 24 && p <= 30) return true;           /* SPI flash */
    if (p == 16 || p == 17) return true;           /* UART0 console */
    if (p == 12 || p == 13) return true;           /* USB D-/D+ */
#elif CONFIG_IDF_TARGET_ESP32H2
    if (p >= 15 && p <= 21) return true;           /* SPI flash */
    if (p == 23 || p == 24) return true;           /* UART0 console */
    if (p == 26 || p == 27) return true;           /* USB D-/D+ */
#endif
    return false;
}

static bool is_strapping(int p)
{
#if CONFIG_IDF_TARGET_ESP32
    return p == 0 || p == 2 || p == 5 || p == 12 || p == 15;
#elif CONFIG_IDF_TARGET_ESP32S2
    return p == 0 || p == 45 || p == 46;
#elif CONFIG_IDF_TARGET_ESP32S3
    return p == 0 || p == 3 || p == 45 || p == 46;
#elif CONFIG_IDF_TARGET_ESP32C3
    return p == 2 || p == 8 || p == 9;
#elif CONFIG_IDF_TARGET_ESP32C2
    return p == 8 || p == 9;
#elif CONFIG_IDF_TARGET_ESP32C6
    return p == 8 || p == 9 || p == 15;
#elif CONFIG_IDF_TARGET_ESP32H2
    return p == 8 || p == 9 || p == 25;
#else
    return false;
#endif
}

/* ------------------------------------------------------------------ helpers */

static pin_t *find_pin(int gpio)
{
    for (int i = 0; i < s_count; i++) {
        if (s_pins[i].gpio == gpio) return &s_pins[i];
    }
    return NULL;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void gpio_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(s_channel_owner, -1, sizeof(s_channel_owner));

    for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
        if (!GPIO_IS_VALID_GPIO(p) || is_reserved(p) || gpio_mgr_is_bus_pin(p)) continue;

        pin_t *pin = &s_pins[s_count++];
        pin->gpio = p;
        pin->mode = PIN_MODE_INPUT;
        pin->input_only = !GPIO_IS_VALID_OUTPUT_GPIO(p);
        pin->strapping = is_strapping(p);
        pin->pwm_channel = -1;
        pin->pwm_timer = -1;

        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_INPUT);
        pin->level = gpio_get_level(p);
        pin->last_change_ms = now_ms();
    }
    ESP_LOGI(TAG, "%d usable GPIOs on %s", s_count, CONFIG_IDF_TARGET);
}

int gpio_mgr_count(void) { return s_count; }
int gpio_mgr_gpio_at(int i) { return (i >= 0 && i < s_count) ? s_pins[i].gpio : -1; }
bool gpio_mgr_is_usable(int gpio) { return find_pin(gpio) != NULL; }

/* ---------------------------------------------------------------------- PWM */

static void pwm_release(pin_t *pin)
{
    if (pin->pwm_channel >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pin->pwm_channel, 0);
        s_channel_owner[pin->pwm_channel] = -1;
    }
    if (pin->pwm_timer >= 0 && s_timers[pin->pwm_timer].refs > 0) {
        s_timers[pin->pwm_timer].refs--;
    }
    pin->pwm_channel = -1;
    pin->pwm_timer = -1;
    pin->pwm_freq = 0;
    pin->pwm_duty = 0;
}

static esp_err_t set_mode_locked(pin_t *pin, pin_mode_t mode)
{
    if (mode == PIN_MODE_OUTPUT && pin->input_only) return ESP_ERR_NOT_SUPPORTED;

    if (pin->mode == PIN_MODE_PWM) pwm_release(pin);
    gpio_reset_pin(pin->gpio);

    switch (mode) {
    case PIN_MODE_INPUT:
        gpio_set_direction(pin->gpio, GPIO_MODE_INPUT);
        pin->level = gpio_get_level(pin->gpio);
        pin->last_change_ms = now_ms();
        break;
    case PIN_MODE_OUTPUT:
        gpio_set_direction(pin->gpio, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(pin->gpio, 0);
        pin->level = false;
        break;
    case PIN_MODE_PWM:
        break; /* configured by the caller */
    }
    pin->mode = mode;
    return ESP_OK;
}

esp_err_t gpio_mgr_set_mode(int gpio, pin_mode_t mode)
{
    pin_t *pin = find_pin(gpio);
    if (!pin || mode == PIN_MODE_PWM) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = set_mode_locked(pin, mode);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gpio_mgr_write(int gpio, bool level)
{
    pin_t *pin = find_pin(gpio);
    if (!pin) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pin->mode == PIN_MODE_OUTPUT) {
        gpio_set_level(gpio, level);
        pin->level = level;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gpio_mgr_set_pwm(int gpio, uint32_t freq_hz, uint32_t duty_pct)
{
    pin_t *pin = find_pin(gpio);
    if (!pin || pin->input_only) return ESP_ERR_INVALID_ARG;
    if (freq_hz < PWM_MIN_FREQ_HZ || freq_hz > PWM_MAX_FREQ_HZ) return ESP_ERR_INVALID_ARG;
    if (duty_pct > 100) duty_pct = 100;
    if (PWM_TIMERS < 1 || PWM_CHANNELS < 1) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err = ESP_OK;
    uint32_t duty = (duty_pct * PWM_MAX_DUTY) / 100;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Same frequency, already running: only the duty changes. */
    if (pin->mode == PIN_MODE_PWM && pin->pwm_freq == freq_hz) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pin->pwm_channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)pin->pwm_channel);
        pin->pwm_duty = duty_pct;
        goto out;
    }

    err = set_mode_locked(pin, PIN_MODE_INPUT); /* releases any previous PWM state */
    if (err != ESP_OK) goto out;

    /* Share a timer between pins using the same frequency, else grab a free one. */
    int timer = -1;
    for (int t = 0; t < PWM_TIMERS; t++) {
        if (s_timers[t].refs > 0 && s_timers[t].freq == freq_hz) { timer = t; break; }
    }
    if (timer < 0) {
        for (int t = 0; t < PWM_TIMERS; t++) {
            if (s_timers[t].refs == 0) { timer = t; break; }
        }
        if (timer < 0) { err = ESP_ERR_NO_MEM; goto out; }
        ledc_timer_config_t tcfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = PWM_RESOLUTION,
            .timer_num = (ledc_timer_t)timer,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        err = ledc_timer_config(&tcfg);
        if (err != ESP_OK) goto out;
        s_timers[timer].freq = freq_hz;
    }

    int channel = -1;
    for (int c = 0; c < PWM_CHANNELS; c++) {
        if (s_channel_owner[c] < 0) { channel = c; break; }
    }
    if (channel < 0) { err = ESP_ERR_NO_MEM; goto out; }

    ledc_channel_config_t ccfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)channel,
        .timer_sel = (ledc_timer_t)timer,
        .duty = duty,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) goto out;

    s_timers[timer].refs++;
    s_channel_owner[channel] = (int8_t)(pin - s_pins);
    pin->mode = PIN_MODE_PWM;
    pin->pwm_timer = (int8_t)timer;
    pin->pwm_channel = (int8_t)channel;
    pin->pwm_freq = freq_hz;
    pin->pwm_duty = (uint8_t)duty_pct;
    pin->level = false;

out:
    xSemaphoreGive(s_lock);
    return err;
}

/* ------------------------------------------------------------------ polling */

bool gpio_mgr_poll(void)
{
    bool changed = false;
    int64_t now = now_ms();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        pin_t *pin = &s_pins[i];
        if (pin->mode != PIN_MODE_INPUT) continue;
        bool level = gpio_get_level(pin->gpio);
        if (level != pin->level && (now - pin->last_change_ms) > DEBOUNCE_MS) {
            pin->level = level;
            pin->bounce++;
            pin->last_change_ms = now;
            changed = true;
        }
    }
    xSemaphoreGive(s_lock);
    return changed;
}

char *gpio_mgr_snapshot_json(void)
{
    static const char *const mode_names[] = { "INPUT", "OUTPUT", "PWM" };
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "pins");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const pin_t *pin = &s_pins[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "pin", pin->gpio);
        cJSON_AddStringToObject(o, "mode", mode_names[pin->mode]);
        cJSON_AddNumberToObject(o, "state", pin->level ? 1 : 0);
        cJSON_AddNumberToObject(o, "bounce", pin->bounce);
        if (pin->mode == PIN_MODE_PWM) {
            cJSON_AddNumberToObject(o, "freq", pin->pwm_freq);
            cJSON_AddNumberToObject(o, "duty", pin->pwm_duty);
        }
        if (pin->input_only) cJSON_AddTrueToObject(o, "input_only");
        if (pin->strapping) cJSON_AddTrueToObject(o, "strapping");
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_lock);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}
