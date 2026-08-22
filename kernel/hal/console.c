/*
 * hal/console.c - the unified early console.
 *
 * Fans each character out to serial and the early VGA text console. The linear
 * framebuffer is intentionally not a kernel log target: it belongs to the
 * future Windows-compatible USER/GDI stack and explorer.exe.
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
