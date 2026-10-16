#include "services.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bench.h"
#include "buses.h"
#include "gpio_mgr.h"
#include "temp_sensor.h"
#include "web_server.h"
#include "wifi_mgr.h"

#define IO_PERIOD_MS        50
#define TELEMETRY_PERIOD_MS 1000
#define WIFI_EVERY_N        5

/* Polls inputs and forwards received UART bytes. Runs even without clients so
 * bounce counters keep counting; it only talks to the network when needed. */
static void io_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(IO_PERIOD_MS));
        if (bench_is_running()) continue; /* benchmarks own the pins and the CPU */

        bool changed = gpio_mgr_poll();
        bool clients = web_server_has_clients();
        if (changed && clients) {
            char *s = gpio_mgr_snapshot_json();
            if (s) {
                web_server_broadcast(s);
                cJSON_free(s);
            }
        }

        char rx[96];
        int n = buses_uart_read(rx, sizeof(rx) - 1);
        if (n > 0 && clients) {
            for (int i = 0; i < n; i++) {
                unsigned char c = (unsigned char)rx[i];
                if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c >= 0x7F) rx[i] = '.';
            }
            rx[n] = '\0';
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "uart", rx);
            char *s = cJSON_PrintUnformatted(o);
            if (s) {
                web_server_broadcast(s);
                cJSON_free(s);
            }
            cJSON_Delete(o);
        }
    }
}

static void telemetry_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    unsigned tick = 0;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
        tick++;
        if (!web_server_has_clients() || bench_is_running()) continue;

        cJSON *root = cJSON_CreateObject();
        cJSON *sys = cJSON_AddObjectToObject(root, "sys");
        cJSON_AddNumberToObject(sys, "uptime", (double)(esp_timer_get_time() / 1000000));
        cJSON_AddNumberToObject(sys, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(sys, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(sys, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
            cJSON_AddNumberToObject(sys, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        cJSON_AddNumberToObject(sys, "cpu_mhz", esp_rom_get_cpu_ticks_per_us());
        float t;
        if (temp_sensor_read_c(&t) == ESP_OK) cJSON_AddNumberToObject(sys, "temp", (double)((int)(t * 10)) / 10.0);
        if (tick % WIFI_EVERY_N == 1) cJSON_AddItemToObject(root, "wifi", wifi_mgr_status_json());

        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            web_server_broadcast(s);
            cJSON_free(s);
        }
        cJSON_Delete(root);
    }
}

void services_start(void)
{
    xTaskCreate(io_task, "io_poll", 3072, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 3, NULL);
}
