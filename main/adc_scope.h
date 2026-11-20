#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* Oscilloscope-style ADC sampling on any ADC1-capable pin of the running chip. */
void adc_scope_init(void);

/* GPIOs that can be sampled (ADC1 only: ADC2 is unusable while Wi-Fi runs on some chips). */
cJSON *adc_scope_pins_json(void);

esp_err_t adc_scope_select(int gpio);
int       adc_scope_selected(void);

/* One calibrated reading in millivolts (blocks briefly). */
esp_err_t adc_scope_read_mv(int *mv, bool *calibrated);

/* Sampling task: batches samples and pushes {"scope":{...}} to WebSocket clients. */
void adc_scope_start_task(void);
