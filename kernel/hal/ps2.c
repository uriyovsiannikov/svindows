/*
 * hal/ps2.c - the 8042 PS/2 controller.
 *
 * Low-level bring-up shared by the keyboard and mouse drivers: reset the
 * config byte so both ports and both IRQs are enabled, and provide the small
 * read/write handshake helpers the two drivers use.
 */
#include "ps2.h"
#include <ntos/hal.h>

void Ps2WaitInput(void)
{
    /* Wait until the controller's input buffer is empty (bit 1 clear). */
    for (int i = 0; i < 100000; i++)
        if (!(__inbyte(PS2_STATUS) & PS2_STATUS_INPUT_FULL))
            return;
}

void Ps2WaitOutput(void)
{
    /* Wait until there is a byte to read (bit 0 set). */
    for (int i = 0; i < 100000; i++)
        if (__inbyte(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL)
            return;
}

static void ps2_command(UINT8 command)
{
    Ps2WaitInput();
    __outbyte(PS2_COMMAND, command);
}

static void ps2_write_data(UINT8 data)
{
    Ps2WaitInput();
    __outbyte(PS2_DATA, data);
}

static UINT8 ps2_read_data(void)
{
    Ps2WaitOutput();
    return __inbyte(PS2_DATA);
}

UINT8 Ps2MouseCommand(UINT8 command)
{
    ps2_command(PS2_CMD_WRITE_AUX); /* route the next data byte to the mouse */
    ps2_write_data(command);
    return ps2_read_data();         /* the mouse's ACK (0xFA) */
}

void Ps2ControllerInit(void)
{
    static BOOLEAN done = FALSE;
    if (done)
        return;
    done = TRUE;

    /* Disable both ports so the devices don't chatter during setup. */
    ps2_command(PS2_CMD_DISABLE_KBD);
    ps2_command(PS2_CMD_DISABLE_AUX);

    /* Flush any stale byte from the output buffer. */
    (void)__inbyte(PS2_DATA);

    /* Read, adjust, and write back the config byte: enable both IRQs and make
     * sure both clocks are running (clear the "disabled" bits). */
    ps2_command(PS2_CMD_READ_CONFIG);
    UINT8 config = ps2_read_data();
    config |= PS2_CONFIG_KBD_IRQ | PS2_CONFIG_AUX_IRQ;
    config &= (UINT8)~(PS2_CONFIG_KBD_CLOCK | PS2_CONFIG_AUX_CLOCK);
    ps2_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    /* Re-enable both ports. */
    ps2_command(PS2_CMD_ENABLE_KBD);
    ps2_command(PS2_CMD_ENABLE_AUX);
}
