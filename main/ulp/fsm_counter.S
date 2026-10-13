/* ULP-FSM program (ESP32 / S2 / S3 in FSM mode).
 *
 * The coprocessor is woken by its own timer; each wake-up runs this code once:
 * it bumps `counter` and writes a marker, then halts until the next wake-up.
 * The main CPU compares `counter` with the elapsed time to check the ULP
 * timer, and `magic` proves the program really executed. */
#include "sdkconfig.h"
#include "soc/soc_ulp.h"

	.bss
	.global counter
counter:
	.long 0
	.global magic
magic:
	.long 0

	.text
	.global entry
entry:
	move r3, counter
	ld r0, r3, 0
	add r0, r0, 1
	st r0, r3, 0

	move r3, magic
	move r0, 0xA5A5
	st r0, r3, 0
	halt
