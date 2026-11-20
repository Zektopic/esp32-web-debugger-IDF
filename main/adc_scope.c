#include "adc_scope.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bench.h"
#include "gpio_mgr.h"
#include "web_server.h"

#if SOC_ADC_SUPPORTED
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "adc_scope";

#define SAMPLE_PERIOD_MS 40
#define SAMPLES_PER_MSG  5

#if SOC_ADC_SUPPORTED

#define SCOPE_ATTEN   ADC_ATTEN_DB_12
#define SCOPE_BITWIDTH ADC_BITWIDTH_12

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_channel;
static int                       s_gpio = -1;
static SemaphoreHandle_t         s_lock;

static void cali_delete(void)
{
    if (!s_cali) return;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(s_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(s_cali);
#endif
    s_cali = NULL;
}

static void cali_create(adc_channel_t chan)
{
    cali_delete();
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1, .chan = chan, .atten = SCOPE_ATTEN, .bitwidth = SCOPE_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    (void)chan;
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1, .atten = SCOPE_ATTEN, .bitwidth = SCOPE_BITWIDTH,
    };
#if CONFIG_IDF_TARGET_ESP32
    /* Original ESP32 only: without a Vref burnt into eFuse assume the typical 1100 mV. */
    adc_cali_line_fitting_efuse_val_t source = ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF;
    adc_cali_scheme_line_fitting_check_efuse(&source);
    if (source == ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF) cfg.default_vref = 1100;
#endif
    if (adc_cali_create_scheme_line_fitting(&cfg, &s_cali) != ESP_OK) s_cali = NULL;
#else
    (void)chan;
#endif
    if (!s_cali) ESP_LOGW(TAG, "no ADC calibration scheme, voltages are approximate");
}

static bool gpio_to_adc1(int gpio, adc_channel_t *chan)
{
    adc_unit_t unit;
    return adc_oneshot_io_to_channel(gpio, &unit, chan) == ESP_OK && unit == ADC_UNIT_1;
}

void adc_scope_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed");
        s_adc = NULL;
        return;
    }
    /* Start on the first usable ADC1 pin. */
    for (int i = 0; i < gpio_mgr_count(); i++) {
        if (adc_scope_select(gpio_mgr_gpio_at(i)) == ESP_OK) break;
    }
}

cJSON *adc_scope_pins_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < gpio_mgr_count(); i++) {
        adc_channel_t c;
        if (gpio_to_adc1(gpio_mgr_gpio_at(i), &c)) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(gpio_mgr_gpio_at(i)));
        }
    }
    return arr;
}

esp_err_t adc_scope_select(int gpio)
{
    adc_channel_t chan;
    if (!s_adc || !gpio_mgr_is_usable(gpio) || !gpio_to_adc1(gpio, &chan)) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_oneshot_chan_cfg_t ccfg = { .bitwidth = SCOPE_BITWIDTH, .atten = SCOPE_ATTEN };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = adc_oneshot_config_channel(s_adc, chan, &ccfg);
    if (err == ESP_OK) {
        cali_create(chan);
        s_channel = chan;
        s_gpio = gpio;
    }
    xSemaphoreGive(s_lock);
    return err;
}

int adc_scope_selected(void) { return s_gpio; }

esp_err_t adc_scope_read_mv(int *mv, bool *calibrated)
{
    if (!s_adc || s_gpio < 0) return ESP_ERR_INVALID_STATE;
    int raw = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = adc_oneshot_read(s_adc, s_channel, &raw);
    if (err == ESP_OK) {
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, mv) == ESP_OK) {
            if (calibrated) *calibrated = true;
        } else {
            *mv = raw * 3100 / 4095; /* rough 12 dB full-scale estimate */
            if (calibrated) *calibrated = false;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void scope_task(void *arg)
{
    int samples[SAMPLES_PER_MSG];
    int n = 0;
    bool cal = false;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        if (!web_server_has_clients() || bench_is_running()) {
            n = 0;
            continue;
        }
        int mv;
        if (adc_scope_read_mv(&mv, &cal) != ESP_OK) continue;
        samples[n++] = mv;
        if (n < SAMPLES_PER_MSG) continue;

        char buf[160];
        int len = snprintf(buf, sizeof(buf),
                           "{\"scope\":{\"pin\":%d,\"dt\":%d,\"cal\":%s,\"mv\":[",
                           s_gpio, SAMPLE_PERIOD_MS, cal ? "true" : "false");
        for (int i = 0; i < n; i++) {
            len += snprintf(buf + len, sizeof(buf) - len, "%s%d", i ? "," : "", samples[i]);
        }
        snprintf(buf + len, sizeof(buf) - len, "]}}");
        web_server_broadcast(buf);
        n = 0;
    }
}

void adc_scope_start_task(void)
{
    if (!s_adc) return;
    xTaskCreate(scope_task, "scope", 3072, NULL, 4, NULL);
}

#else /* !SOC_ADC_SUPPORTED */

void adc_scope_init(void) {}
cJSON *adc_scope_pins_json(void) { return cJSON_CreateArray(); }
esp_err_t adc_scope_select(int gpio) { (void)gpio; return ESP_ERR_NOT_SUPPORTED; }
int adc_scope_selected(void) { return -1; }
esp_err_t adc_scope_read_mv(int *mv, bool *calibrated) { (void)mv; (void)calibrated; return ESP_ERR_NOT_SUPPORTED; }
void adc_scope_start_task(void) {}

#endif
