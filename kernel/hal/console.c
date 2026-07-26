/*
 * hal/console.c - the unified early console.
 *
 * Fans each character out to both the serial port and the VGA text screen so
 * the kernel log is visible whether you are watching QEMU's serial stdout or
 * its graphical window.
 */
#include <ntos/hal.h>

void HalInitializeConsole(void)
{
    HalInitializeSerial();
    HalInitializeVga();
}

void HalConsolePutChar(char c)
{
    HalSerialPutChar(c);
    HalVgaPutChar(c);
}

void HalConsoleWrite(const char *s)
{
    while (*s)
        HalConsolePutChar(*s++);
}
