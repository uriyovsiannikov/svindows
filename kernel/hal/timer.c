/*
 * hal/timer.c - the 8254 Programmable Interval Timer (channel 0 -> IRQ0).
 *
 * Channel 0 is programmed as a rate generator (mode 2) so it raises IRQ0 at a
 * steady frequency. That interrupt is the heartbeat the scheduler uses to
 * preempt threads.
 */
#include <ntos/hal.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQUENCY 1193182u /* input clock, Hz */

void HalInitializeTimer(UINT32 hz)
{
    if (hz == 0)
        hz = 100;

    UINT32 divisor = PIT_FREQUENCY / hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    /* Command: channel 0, access lo+hi byte, mode 2 (rate generator), binary. */
    __outbyte(PIT_COMMAND, 0x34);
    __outbyte(PIT_CHANNEL0, (UINT8)(divisor & 0xFF));
    __outbyte(PIT_CHANNEL0, (UINT8)((divisor >> 8) & 0xFF));

    HalUnmaskIrq(0);
}
