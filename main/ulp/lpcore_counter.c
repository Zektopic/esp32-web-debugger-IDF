/* LP-core program (ESP32-C6): same protocol as the ULP-RISC-V one. */
#include <stdint.h>
#include "ulp_lp_core_utils.h"

volatile uint32_t counter;
volatile uint32_t spin;
volatile uint32_t mode;
volatile uint32_t magic;

int main(void)
{
    magic = 0xB0DE;
    while (1) {
        if (mode == 0) {
            ulp_lp_core_delay_us(1000);
            counter++;
        } else {
            spin++;
        }
    }
    return 0;
}
