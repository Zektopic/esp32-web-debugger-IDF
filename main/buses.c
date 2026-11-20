#include "buses.h"

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "buses";

static i2c_master_bus_handle_t s_i2c;

void buses_init(void)
{
    /* UART */
    const uart_config_t ucfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_driver_install(BUS_UART_PORT, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(BUS_UART_PORT, &ucfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(BUS_UART_PORT, CONFIG_WEBDBG_UART_TX_GPIO,
                                               CONFIG_WEBDBG_UART_RX_GPIO,
                                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* I2C (new master driver) */
    i2c_master_bus_config_t bcfg = {
        .i2c_port = -1, /* any free controller */
        .sda_io_num = CONFIG_WEBDBG_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_WEBDBG_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bcfg, &s_i2c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        s_i2c = NULL;
    }
}

int buses_uart_write(const char *data, size_t len)
{
    return uart_write_bytes(BUS_UART_PORT, data, len);
}

int buses_uart_read(char *out, size_t max)
{
    size_t avail = 0;
    uart_get_buffered_data_len(BUS_UART_PORT, &avail);
    if (avail == 0) return 0;
    if (avail > max) avail = max;
    int n = uart_read_bytes(BUS_UART_PORT, out, avail, 0);
    return n < 0 ? 0 : n;
}

esp_err_t buses_uart_set_baud(uint32_t baud)
{
    if (baud < 300 || baud > 5000000) return ESP_ERR_INVALID_ARG;
    return uart_set_baudrate(BUS_UART_PORT, baud);
}

int buses_i2c_scan(uint8_t *addrs, size_t max)
{
    if (!s_i2c) return -ESP_ERR_INVALID_STATE;
    int found = 0;
    for (uint8_t addr = 1; addr < 127 && (size_t)found < max; addr++) {
        if (i2c_master_probe(s_i2c, addr, 20) == ESP_OK) {
            addrs[found++] = addr;
        }
    }
    return found;
}
