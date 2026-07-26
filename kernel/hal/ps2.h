/*
 * hal/ps2.h - private helpers for the 8042 PS/2 controller.
 *
 * Shared between the keyboard (keyboard.c) and mouse (mouse.c) drivers, which
 * both sit behind the same controller. Not a public kernel interface.
 */
#ifndef _HAL_PS2_H_
#define _HAL_PS2_H_

#include <nt/ntdef.h>

/* 8042 I/O ports. */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64 /* read: status register  */
#define PS2_COMMAND 0x64 /* write: command register */

/* Status register bits. */
#define PS2_STATUS_OUTPUT_FULL 0x01 /* data waiting to be read from 0x60   */
#define PS2_STATUS_INPUT_FULL  0x02 /* controller not ready for a write    */
#define PS2_STATUS_AUX_DATA    0x20 /* byte in 0x60 came from the aux (mouse) */

/* Controller commands (written to 0x64). */
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_AUX   0xA7
#define PS2_CMD_ENABLE_AUX    0xA8
#define PS2_CMD_DISABLE_KBD   0xAD
#define PS2_CMD_ENABLE_KBD    0xAE
#define PS2_CMD_WRITE_AUX     0xD4 /* next byte to 0x60 is routed to the mouse */

/* Config byte bits. */
#define PS2_CONFIG_KBD_IRQ   0x01
#define PS2_CONFIG_AUX_IRQ   0x02
#define PS2_CONFIG_KBD_CLOCK 0x10 /* 1 = disabled */
#define PS2_CONFIG_AUX_CLOCK 0x20 /* 1 = disabled */

/* Bring up the controller (both ports enabled, both IRQs enabled). Idempotent;
 * the first driver to initialize calls it. */
void Ps2ControllerInit(void);

/* Spin until the controller can accept a write / has a byte to read. */
void Ps2WaitInput(void);   /* input buffer empty -> safe to write */
void Ps2WaitOutput(void);  /* output buffer full  -> safe to read */

/* Send a command byte to the mouse and return its ACK (0xFA on success). */
UINT8 Ps2MouseCommand(UINT8 command);

#endif /* _HAL_PS2_H_ */
