/*
 * hal/mouse.c - PS/2 mouse driver (IRQ12).
 *
 * Enables the auxiliary device on the 8042 controller, then decodes the
 * standard 3-byte movement packets into a screen-clamped cursor position and a
 * button bitmask. Consumers poll MouseState (watching MouseState.Seq for
 * motion) and draw the cursor.
 */
#include <ntos/input.h>
#include <ntos/hal.h>
#include "ps2.h"

MOUSE_STATE MouseState;

static INT32 mouse_max_x = 1023;
static INT32 mouse_max_y = 767;

/* Packet reassembly. */
static UINT8 pkt[3];
static UINT8 pkt_index;

void HalMouseSetBounds(INT32 width, INT32 height)
{
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    MouseState.X = width / 2;
    MouseState.Y = height / 2;
    MouseState.Seq++;
}

static void mouse_irq(void)
{
    UINT8 byte = __inbyte(PS2_DATA);

    /* Byte 0 always has bit 3 set; use it to resynchronize if we lose framing. */
    if (pkt_index == 0 && !(byte & 0x08))
        return;

    pkt[pkt_index++] = byte;
    if (pkt_index < 3)
        return;
    pkt_index = 0;

    UINT8 flags = pkt[0];

    /* Discard packets with an overflow bit set (garbage movement). */
    if (flags & 0xC0)
        return;

    /* 9-bit signed deltas: byte 1/2 with the sign bit from the flags. */
    INT32 dx = (INT32)pkt[1] - ((flags & 0x10) ? 256 : 0);
    INT32 dy = (INT32)pkt[2] - ((flags & 0x20) ? 256 : 0);

    INT32 x = MouseState.X + dx;
    INT32 y = MouseState.Y - dy; /* PS/2 Y is positive-up; screen is positive-down */

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > mouse_max_x) x = mouse_max_x;
    if (y > mouse_max_y) y = mouse_max_y;

    MouseState.X = x;
    MouseState.Y = y;
    MouseState.Buttons = flags & 0x07;
    MouseState.Seq++;
}

void HalInitializeMouse(void)
{
    Ps2ControllerInit();

    /* Set defaults, then enable data reporting so the mouse streams packets. */
    Ps2MouseCommand(0xF6); /* set defaults */
    Ps2MouseCommand(0xF4); /* enable data reporting */

    HalRegisterIrqHandler(12, mouse_irq);
    HalUnmaskIrq(12);
}
