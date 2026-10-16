/* ULP-RISC-V program (ESP32-S2 / S3).
 *
 * mode 0: increments `counter` once per millisecond (ULP delay loop), which
 *         lets the main CPU judge the coprocessor clock.
 * mode 1: spins as fast as possible incrementing `spin`, which gives the loop
 *         rate of the coprocessor. */
#include <stdint.h>
#include "ulp_riscv.h"
#include "ulp_riscv_utils.h"

volatile uint32_t counter;
volatile uint32_t spin;
volatile uint32_t mode;
volatile uint32_t magic;

int main(void)
{
    magic = 0xB0DE;
    while (1) {
        if (mode == 0) {
            ulp_riscv_delay_cycles(ULP_RISCV_CYCLES_PER_MS);
            counter++;
        } else {
            spin++;
        }
    }
    return 0;
}
