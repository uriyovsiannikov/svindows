/*
 * hal/serial.c - 16550 UART driver for COM1.
 *
 * This is the kernel's primary log sink: under QEMU the emulated COM1 is wired
 * to stdout (`-serial stdio`), so everything the kernel prints shows up in the
 * host terminal even before there is any display driver.
 */
#include <ntos/hal.h>

#define COM1_BASE 0x3F8

/* Register offsets from the port base. */
#define UART_DATA        0 /* DLAB=0: data; DLAB=1: divisor low  */
#define UART_IER         1 /* DLAB=0: interrupt enable; DLAB=1: divisor high */
#define UART_FCR         2 /* write: FIFO control */
#define UART_LCR         3 /* line control */
#define UART_MCR         4 /* modem control */
#define UART_LSR         5 /* line status */

#define LSR_THR_EMPTY    0x20 /* transmit holding register empty */

static BOOLEAN g_serial_ready = FALSE;

void HalInitializeSerial(void)
{
    __outbyte(COM1_BASE + UART_IER, 0x00); /* disable interrupts           */
    __outbyte(COM1_BASE + UART_LCR, 0x80); /* enable DLAB to set baud rate */
    __outbyte(COM1_BASE + UART_DATA, 0x01);/* divisor low  = 1 (115200 baud) */
    __outbyte(COM1_BASE + UART_IER, 0x00); /* divisor high = 0             */
    __outbyte(COM1_BASE + UART_LCR, 0x03); /* 8 bits, no parity, one stop  */
    __outbyte(COM1_BASE + UART_FCR, 0xC7); /* enable + clear FIFOs, 14-byte */
    __outbyte(COM1_BASE + UART_MCR, 0x0B); /* DTR, RTS, OUT2 (irq gate)    */

    g_serial_ready = TRUE;
}

static void serial_wait_tx(void)
{
    while ((__inbyte(COM1_BASE + UART_LSR) & LSR_THR_EMPTY) == 0)
        __pause();
}

void HalSerialPutChar(char c)
{
    if (!g_serial_ready)
        return;

    if (c == '\n') {
        serial_wait_tx();
        __outbyte(COM1_BASE + UART_DATA, '\r');
    }
    serial_wait_tx();
    __outbyte(COM1_BASE + UART_DATA, (UINT8)c);
}
