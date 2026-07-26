/*
 * hal/pic.c - the 8259A programmable interrupt controllers and IRQ dispatch.
 *
 * The two cascaded PICs deliver hardware IRQs 0..15. Their default vectors
 * (0x08..0x0F) collide with CPU exceptions, so we remap them to 0x20..0x2F
 * (IRQ_BASE_VECTOR). All lines start masked; drivers unmask the ones they use.
 */
#include <ntos/hal.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11 /* begin init, expect ICW4 */
#define ICW4_8086 0x01 /* 8086/88 mode            */
#define PIC_EOI   0x20 /* end-of-interrupt        */

static HAL_IRQ_HANDLER g_irq_handlers[16];

void HalInitializePic(void)
{
    /* Start the initialization sequence on both chips. */
    __outbyte(PIC1_CMD, ICW1_INIT); __iodelay();
    __outbyte(PIC2_CMD, ICW1_INIT); __iodelay();

    /* ICW2: vector offsets - master at 0x20, slave at 0x28. */
    __outbyte(PIC1_DATA, IRQ_BASE_VECTOR);      __iodelay();
    __outbyte(PIC2_DATA, IRQ_BASE_VECTOR + 8);  __iodelay();

    /* ICW3: tell master a slave is on IRQ2; tell slave its cascade identity. */
    __outbyte(PIC1_DATA, 0x04); __iodelay();
    __outbyte(PIC2_DATA, 0x02); __iodelay();

    /* ICW4: 8086 mode. */
    __outbyte(PIC1_DATA, ICW4_8086); __iodelay();
    __outbyte(PIC2_DATA, ICW4_8086); __iodelay();

    /* Mask everything for now. */
    __outbyte(PIC1_DATA, 0xFF);
    __outbyte(PIC2_DATA, 0xFF);

    for (int i = 0; i < 16; i++)
        g_irq_handlers[i] = NULL;
}

void HalSendEoi(UINT8 irq)
{
    if (irq >= 8)
        __outbyte(PIC2_CMD, PIC_EOI);
    __outbyte(PIC1_CMD, PIC_EOI);
}

void HalMaskIrq(UINT8 irq)
{
    UINT16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    UINT8 bit = (UINT8)(irq < 8 ? irq : irq - 8);
    __outbyte(port, (UINT8)(__inbyte(port) | (1u << bit)));
}

void HalUnmaskIrq(UINT8 irq)
{
    UINT16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    UINT8 bit = (UINT8)(irq < 8 ? irq : irq - 8);
    __outbyte(port, (UINT8)(__inbyte(port) & ~(1u << bit)));

    /* Unmasking any slave line also requires the cascade line (IRQ2) open. */
    if (irq >= 8)
        __outbyte(PIC1_DATA, (UINT8)(__inbyte(PIC1_DATA) & ~(1u << 2)));
}

void HalRegisterIrqHandler(UINT8 irq, HAL_IRQ_HANDLER handler)
{
    if (irq < 16)
        g_irq_handlers[irq] = handler;
}

void HalDispatchIrq(UINT8 irq)
{
    /* Acknowledge first: a handler may context-switch and not return for a
     * long time, and the PIC must be free to deliver the next interrupt. */
    HalSendEoi(irq);

    if (irq < 16 && g_irq_handlers[irq])
        g_irq_handlers[irq]();
}
