/*
 * io/ata.c - a minimal polled ATA (IDE) PIO driver for the primary bus master.
 *
 * Enough to read sectors with 28-bit LBA by polling the status register - no
 * interrupts, no DMA. This is the block device the FAT driver sits on.
 */
#include <ntos/io.h>
#include <ntos/hal.h>
#include <ntos/ke.h>

#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

#define ATA_REG_DATA     (ATA_IO_BASE + 0)
#define ATA_REG_ERROR    (ATA_IO_BASE + 1)
#define ATA_REG_SECCOUNT (ATA_IO_BASE + 2)
#define ATA_REG_LBA0     (ATA_IO_BASE + 3)
#define ATA_REG_LBA1     (ATA_IO_BASE + 4)
#define ATA_REG_LBA2     (ATA_IO_BASE + 5)
#define ATA_REG_DRIVE    (ATA_IO_BASE + 6)
#define ATA_REG_COMMAND  (ATA_IO_BASE + 7)
#define ATA_REG_STATUS   (ATA_IO_BASE + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_IDENTIFY 0xEC

static void ata_io_wait(void)
{
    /* Reading the alternate status port four times burns ~400 ns. */
    for (int i = 0; i < 4; i++)
        (void)__inbyte(ATA_CTRL_BASE);
}

static BOOLEAN ata_wait_not_busy(void)
{
    for (UINT32 spin = 0; spin < 10000000u; spin++)
        if (!(__inbyte(ATA_REG_STATUS) & ATA_SR_BSY))
            return TRUE;
    return FALSE;
}

/* Wait until the drive is ready to transfer a block (DRQ), or an error. */
static BOOLEAN ata_wait_drq(void)
{
    for (UINT32 spin = 0; spin < 10000000u; spin++) {
        UINT8 s = __inbyte(ATA_REG_STATUS);
        if (s & ATA_SR_ERR)
            return FALSE;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
            return TRUE;
    }
    return FALSE;
}

static void ata_read_data(void *buffer)
{
    UINT32 count = ATA_SECTOR_SIZE / 2; /* 256 words */
    __asm__ volatile("rep insw"
                     : "+D"(buffer), "+c"(count)
                     : "d"((UINT16)ATA_REG_DATA)
                     : "memory");
}

BOOLEAN AtaReadSectors(UINT32 lba, UINT8 count, void *buffer)
{
    if (count == 0)
        return FALSE;
    if (!ata_wait_not_busy())
        return FALSE;

    /* Select master drive, LBA mode, top 4 LBA bits. */
    __outbyte(ATA_REG_DRIVE, (UINT8)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_wait();

    __outbyte(ATA_REG_SECCOUNT, count);
    __outbyte(ATA_REG_LBA0, (UINT8)(lba & 0xFF));
    __outbyte(ATA_REG_LBA1, (UINT8)((lba >> 8) & 0xFF));
    __outbyte(ATA_REG_LBA2, (UINT8)((lba >> 16) & 0xFF));
    __outbyte(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    UINT8 *out = (UINT8 *)buffer;
    for (UINT8 i = 0; i < count; i++) {
        if (!ata_wait_drq())
            return FALSE;
        ata_read_data(out);
        out += ATA_SECTOR_SIZE;
    }
    return TRUE;
}

BOOLEAN AtaInitialize(void)
{
    /* Probe: a floating bus reads 0xFF. Anything else means a drive responded. */
    UINT8 status = __inbyte(ATA_REG_STATUS);
    if (status == 0xFF) {
        KeLog("[io]   ata: no drive on the primary bus\n");
        return FALSE;
    }
    KeLog("[io]   ata: primary master present (status 0x%02x)\n", status);
    return TRUE;
}
