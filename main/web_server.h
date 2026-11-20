#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* HTTP + WebSocket server: serves the UI from SPIFFS, a small JSON API
 * (/api/info, /api/bench) and the /ws control/telemetry channel. */
esp_err_t web_server_start(void);

/* True if at least one WebSocket client is connected. */
bool web_server_has_clients(void);

/* Sends a text frame to every WebSocket client. Safe to call from any task;
 * a no-op when the server is not running. */
void web_server_broadcast(const char *json);
