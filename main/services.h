#pragma once

/* Background tasks: GPIO input polling (debounce + bounce counters), UART RX
 * push to the web UI and the 1 Hz system/Wi-Fi telemetry. */
void services_start(void);
