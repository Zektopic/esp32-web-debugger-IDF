#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

/* Wi-Fi in AP+STA mode; the station only connects if an SSID is configured. */
esp_err_t wifi_mgr_start(void);

bool wifi_mgr_sta_connected(void);
/* Dotted-quad strings; return false when not available. */
bool wifi_mgr_sta_ip(char *out, size_t len);
bool wifi_mgr_sta_gateway(char *out, size_t len);
bool wifi_mgr_ap_ip(char *out, size_t len);

/* {"mode":..,"ap":{..},"sta":{..}} - caller owns the object. */
cJSON *wifi_mgr_status_json(void);
