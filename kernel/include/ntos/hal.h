/*
 * ntos/hal.h - Hardware Abstraction Layer.
 *
 * Port I/O primitives plus the early console devices (serial + VGA text mode)
 * that back the kernel log before a real display stack exists.
 */
#ifndef _NTOS_HAL_H_
#define _NTOS_HAL_H_

#include <nt/ntdef.h>

/* ------------------------------------------------------------------ */
/* x86 port I/O                                                       */
/* ------------------------------------------------------------------ */

static ALWAYS_INLINE void __outbyte(UINT16 port, UINT8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static ALWAYS_INLINE UINT8 __inbyte(UINT16 port)
{
    UINT8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static ALWAYS_INLINE void __outword(UINT16 port, UINT16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static ALWAYS_INLINE UINT16 __inword(UINT16 port)
{
    UINT16 value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static ALWAYS_INLINE void __outdword(UINT16 port, UINT32 value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static ALWAYS_INLINE UINT32 __indword(UINT16 port)
{
    UINT32 value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Short delay by touching an unused port (classic OS-dev io_wait). */
static ALWAYS_INLINE void __iodelay(void)
{
    __outbyte(0x80, 0);
}

/* ------------------------------------------------------------------ */
/* CPU control helpers                                                */
/* ------------------------------------------------------------------ */

static ALWAYS_INLINE void __cli(void) { __asm__ volatile("cli"); }
static ALWAYS_INLINE void __sti(void) { __asm__ volatile("sti"); }
static ALWAYS_INLINE void __halt(void) { __asm__ volatile("hlt"); }

static ALWAYS_INLINE void __pause(void) { __asm__ volatile("pause"); }

/* ------------------------------------------------------------------ */
/* Serial console (16550 UART on COM1)                                */
/* ------------------------------------------------------------------ */

void    HalInitializeSerial(void);
void    HalSerialPutChar(char c);

/* ------------------------------------------------------------------ */
/* VGA text-mode console (80x25 at 0xB8000)                           */
/* ------------------------------------------------------------------ */

/* Text attribute byte: low nibble foreground, high nibble background. */
#define VGA_COLOR(fg, bg) ((UINT8)((fg) | ((bg) << 4)))
#define VGA_BLACK   0
#define VGA_BLUE    1
#define VGA_GREEN   2
#define VGA_CYAN    3
#define VGA_RED     4
#define VGA_MAGENTA 5
#define VGA_BROWN   6
#define VGA_LGRAY   7
#define VGA_DGRAY   8
#define VGA_LBLUE   9
#define VGA_LGREEN  10
#define VGA_LCYAN   11
#define VGA_LRED    12
#define VGA_LMAGENTA 13
#define VGA_YELLOW  14
#define VGA_WHITE   15

void HalInitializeVga(void);
void HalVgaRelocate(void);
void HalVgaPutChar(char c);
void HalVgaSetColor(UINT8 attribute);
void HalVgaClear(void);

/* ------------------------------------------------------------------ */
/* Unified early console (fans a character out to serial + VGA)       */
/* ------------------------------------------------------------------ */

void HalInitializeConsole(void);
void HalConsolePutChar(char c);
void HalConsoleWrite(const char *s);

/* ------------------------------------------------------------------ */
/* Interrupt controller (8259 PIC) and IRQ dispatch                   */
/* ------------------------------------------------------------------ */

/* Hardware IRQs are remapped to CPU vectors IRQ_BASE_VECTOR .. +15. */
#define IRQ_BASE_VECTOR 32

typedef void (*HAL_IRQ_HANDLER)(void);

void HalInitializePic(void);
void HalMaskIrq(UINT8 irq);
void HalUnmaskIrq(UINT8 irq);
void HalSendEoi(UINT8 irq);
void HalRegisterIrqHandler(UINT8 irq, HAL_IRQ_HANDLER handler);

/* Called by the trap dispatcher for vectors in the IRQ range. Sends the EOI
 * (before running the handler, so a handler that context-switches away still
 * unblocks the PIC) and invokes any registered handler. */
void HalDispatchIrq(UINT8 irq);

/* ------------------------------------------------------------------ */
/* Programmable Interval Timer (8254 PIT), channel 0 -> IRQ0          */
/* ------------------------------------------------------------------ */

void HalInitializeTimer(UINT32 hz);

#endif /* _NTOS_HAL_H_ */
