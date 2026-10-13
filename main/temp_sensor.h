#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* On-die temperature sensor. Not present on every chip (e.g. the original
 * ESP32 has none), in which case everything returns ESP_ERR_NOT_SUPPORTED. */
void      temp_sensor_init(void);
bool      temp_sensor_available(void);
esp_err_t temp_sensor_read_c(float *celsius);
