/* ESP32 Web Debugger - pure ESP-IDF.
 *
 * One firmware for every ESP32 family member that ESP-IDF supports. What the
 * chip can do (pins, ADC, temperature sensor, ULP, crypto blocks, ...) is
 * discovered at build time from the SOC capability macros, so the web UI and
 * the benchmark suite adapt themselves to the silicon they run on. */
#include "sdkconfig.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "adc_scope.h"
#include "bench.h"
#include "buses.h"
#include "caps.h"
#include "gpio_mgr.h"
#include "services.h"
#include "temp_sensor.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

static void bench_sink(const char *json)
{
    web_server_broadcast(json);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s): the web UI will be unavailable", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Web Debugger on %s, %d MHz", caps_chip_name(), (int)esp_rom_get_cpu_ticks_per_us());

    init_nvs();
    mount_spiffs();

    gpio_mgr_init();
    buses_init();
    temp_sensor_init();
    adc_scope_init();

    bench_init(bench_sink);
    services_start();
    adc_scope_start_task();

#if CONFIG_WEBDBG_ENABLE_WIFI
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(web_server_start());
#endif

#if CONFIG_WEBDBG_BENCH_RUN_ON_BOOT
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let Wi-Fi and the console settle */
    bench_request("all", NULL);
#endif
}
