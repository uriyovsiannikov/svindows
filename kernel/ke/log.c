/*
 * ke/log.c - the kernel logger (KeLog / DbgPrint) and bugcheck.
 *
 * Formats with the Rtl engine into a fixed stack buffer and pushes the result
 * to the HAL console. Long lines are truncated to the buffer size.
 */
#include <ntos/ke.h>
#include <ntos/hal.h>
#include <ntos/rtl.h>

#define KE_LOG_BUFFER_SIZE 512

void KeLogInit(void)
{
    HalInitializeConsole();
}

int KeVLog(const char *fmt, va_list ap)
{
    char buffer[KE_LOG_BUFFER_SIZE];
    int n = RtlFormatV(buffer, sizeof(buffer), fmt, ap);
    HalConsoleWrite(buffer);
    return n;
}

int KeLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = KeVLog(fmt, ap);
    va_end(ap);
    return n;
}

NORETURN void KeBugCheck(ULONG code, const char *message)
{
    __cli();

    HalVgaSetColor(VGA_COLOR(VGA_WHITE, VGA_BLUE));
    KeLog("\n*** STOP: 0x%08x\n", code);
    if (message)
        KeLog("*** %s\n", message);
    KeLog("*** The system has halted.\n");

    for (;;)
        __halt();
}
