#include "temp_sensor.h"

#include "soc/soc_caps.h"
#include "esp_log.h"

#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"

static const char *TAG = "temp";
static temperature_sensor_handle_t s_sensor;

void temp_sensor_init(void)
{
    /* The widest range keeps the sensor usable from a cold start to a hot soak;
     * the driver picks the best hardware range inside it. */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
    if (err == ESP_OK) err = temperature_sensor_enable(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor init failed: %s", esp_err_to_name(err));
        s_sensor = NULL;
    }
}

bool temp_sensor_available(void) { return s_sensor != NULL; }

esp_err_t temp_sensor_read_c(float *celsius)
{
    if (!s_sensor) return ESP_ERR_NOT_SUPPORTED;
    return temperature_sensor_get_celsius(s_sensor, celsius);
}

#else

void temp_sensor_init(void) {}
bool temp_sensor_available(void) { return false; }
esp_err_t temp_sensor_read_c(float *celsius) { (void)celsius; return ESP_ERR_NOT_SUPPORTED; }

#endif
