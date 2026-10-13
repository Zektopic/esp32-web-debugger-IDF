#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

#define BUS_UART_PORT UART_NUM_1

void buses_init(void);

/* UART terminal */
int       buses_uart_write(const char *data, size_t len);
/* Drains whatever is buffered (non-blocking); returns number of bytes copied. */
int       buses_uart_read(char *out, size_t max);
esp_err_t buses_uart_set_baud(uint32_t baud);

/* I2C scanner: fills addrs with responding 7-bit addresses, returns the count
 * or a negative esp_err_t if the bus is not available. */
int buses_i2c_scan(uint8_t *addrs, size_t max);
