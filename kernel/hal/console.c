/*
 * hal/console.c - the unified early console.
 *
 * Fans each character out to the serial port plus a screen backend: VGA text
 * mode early on, or the framebuffer text console once graphics are up.
 */
#include <ntos/hal.h>
#include <ntos/gfx.h>
#include <ntos/ke.h>

static BOOLEAN g_use_framebuffer = FALSE;

void HalInitializeConsole(void)
{
    HalInitializeSerial();
    HalInitializeVga();
}

/* Switch the on-screen backend from VGA text mode to the framebuffer console. */
void HalConsoleUseFramebuffer(void)
{
    g_use_framebuffer = TRUE;
}

void HalConsolePutChar(char c)
{
    HalSerialPutChar(c);

    if (g_use_framebuffer) {
        /* Lift the mouse cursor across the draw (and any scroll it triggers) so
         * it isn't smeared into the console. Masked so the InputWorker thread
         * can't repaint the cursor mid-update. */
        UINT64 flags = KiIrqSave();
        GfxHideCursor();
        GfxConsolePutChar(c);
        GfxShowCursor();
        KiIrqRestore(flags);
    } else {
        HalVgaPutChar(c);
    }
}

void HalConsoleWrite(const char *s)
{
    while (*s)
        HalConsolePutChar(*s++);
}
