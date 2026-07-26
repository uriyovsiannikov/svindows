/*
 * hal/keyboard.c - PS/2 keyboard driver (IRQ1).
 *
 * Reads set-1 scancodes from the 8042 data port on each IRQ1, tracks the shift
 * state, translates make codes to ASCII, and pushes characters into a small
 * ring buffer that KbdReadChar drains.
 */
#include <ntos/input.h>
#include <ntos/hal.h>
#include "ps2.h"

/* US-QWERTY set-1 make-code -> ASCII, unshifted and shifted. Index by the
 * low 7 bits of the scancode; 0 means "no character". */
static const char kbd_map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
};

static const char kbd_map_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
};

#define KBD_RING 64
static volatile char kbd_ring[KBD_RING];
static volatile UINT32 kbd_head, kbd_tail; /* head == tail => empty */

static BOOLEAN kbd_shift;
static BOOLEAN kbd_caps;

static void kbd_push(char c)
{
    UINT32 next = (kbd_head + 1) % KBD_RING;
    if (next != kbd_tail) { /* drop on overflow */
        kbd_ring[kbd_head] = c;
        kbd_head = next;
    }
}

static void kbd_irq(void)
{
    UINT8 sc = __inbyte(PS2_DATA);

    /* 0xE0 introduces an extended code; we ignore the follow-up byte for now. */
    if (sc == 0xE0)
        return;

    BOOLEAN release = (sc & 0x80) != 0;
    UINT8 code = sc & 0x7F;

    /* Shift make/break. */
    if (code == 0x2A || code == 0x36) {
        kbd_shift = !release;
        return;
    }
    if (release)
        return;

    /* Caps lock toggles on make. */
    if (code == 0x3A) {
        kbd_caps = !kbd_caps;
        return;
    }

    char c = kbd_shift ? kbd_map_shift[code] : kbd_map[code];
    if (!c)
        return;

    /* Caps lock affects letters only, and XORs with shift. */
    if (kbd_caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (kbd_caps && c >= 'A' && c <= 'Z' && !kbd_shift)
        c = (char)(c - 'A' + 'a');

    kbd_push(c);
}

void HalInitializeKeyboard(void)
{
    Ps2ControllerInit();

    HalRegisterIrqHandler(1, kbd_irq);
    HalUnmaskIrq(1);
}

BOOLEAN KbdDataAvailable(void)
{
    return (BOOLEAN)(kbd_head != kbd_tail);
}

char KbdReadChar(void)
{
    if (kbd_head == kbd_tail)
        return 0;
    char c = kbd_ring[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_RING;
    return c;
}
