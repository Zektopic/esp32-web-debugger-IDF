set(srcs
    "main.c"
    "caps.c"
    "gpio_mgr.c"
    "adc_scope.c"
    "buses.c"
    "temp_sensor.c"
    "wifi_mgr.c"
    "web_server.c"
    "services.c"
    "bench/bench.c"
    "bench/bench_cpu.c"
    "bench/bench_ram.c"
    "bench/bench_flash.c"
    "bench/bench_crypto.c"
    "bench/bench_rtos.c"
    "bench/bench_gpio.c"
    "bench/bench_uart.c"
    "bench/bench_i2c.c"
    "bench/bench_led.c"
    "bench/bench_ulp.c"
    "bench/bench_thermal.c"
    "bench/bench_wifi.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "bench"
    REQUIRES
        driver esp_adc esp_timer nvs_flash esp_netif esp_wifi esp_event esp_http_server
        spiffs json mbedtls lwip esp_partition spi_flash esp_app_format bootloader_support
        esp_hw_support esp_system soc ulp
)

# Benchmark kernels must be built for speed whatever the project-wide
# optimisation level is, otherwise results would not be comparable.
set_source_files_properties(
    "bench/bench_cpu.c" "bench/bench_ram.c" "bench/bench_crypto.c" "bench/bench_rtos.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# Low-power coprocessor program used by the ULP benchmark. The flavour is
# selected by sdkconfig.defaults.<target> (FSM on ESP32, RISC-V on S2/S3,
# LP core on C6); chips without one skip this block entirely.
if(CONFIG_ULP_COPROC_ENABLED)
    set(ulp_app_name ulp_${COMPONENT_NAME})
    if(CONFIG_ULP_COPROC_TYPE_FSM)
        set(ulp_sources "ulp/fsm_counter.S")
    elseif(CONFIG_ULP_COPROC_TYPE_RISCV)
        set(ulp_sources "ulp/riscv_counter.c")
    elseif(CONFIG_ULP_COPROC_TYPE_LP_CORE)
        set(ulp_sources "ulp/lpcore_counter.c")
    endif()
    set(ulp_exp_dep_srcs "bench/bench_ulp.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
endif()
