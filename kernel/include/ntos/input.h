/*
 * ntos/input.h - PS/2 keyboard and mouse input.
 *
 * The 8042 "PS/2" controller multiplexes a keyboard (IRQ1) and an auxiliary
 * device, the mouse (IRQ12). Both drivers turn hardware bytes into higher-level
 * events: the keyboard into ASCII characters in a ring buffer, the mouse into a
 * clamped cursor position plus button state.
 */
#ifndef _NTOS_INPUT_H_
#define _NTOS_INPUT_H_

#include <nt/ntdef.h>

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */

void    HalInitializeKeyboard(void);

/* Non-blocking: return the next typed character, or 0 if none is queued. */
char    KbdReadChar(void);
BOOLEAN KbdDataAvailable(void);

/* ------------------------------------------------------------------ */
/* Mouse                                                              */
/* ------------------------------------------------------------------ */

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

typedef struct _MOUSE_STATE {
    INT32   X;          /* cursor position, clamped to the screen */
    INT32   Y;
    UINT8   Buttons;    /* MOUSE_BUTTON_* bitmask                  */
    volatile UINT32 Seq; /* bumped on every change (poll for motion) */
} MOUSE_STATE;

extern MOUSE_STATE MouseState;

void HalInitializeMouse(void);

/* Called once the framebuffer geometry is known, to seat the cursor and set
 * the clamp bounds. */
void HalMouseSetBounds(INT32 width, INT32 height);

#endif /* _NTOS_INPUT_H_ */
