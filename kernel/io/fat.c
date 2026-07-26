/*
 * io/fat.c - a read-only FAT32 driver.
 *
 * Parses the BPB, walks the root directory for an 8.3 name, and follows the
 * cluster chain to read a file into a pool buffer. Just enough to load the
 * executables off a disk image.
 */
#include <ntos/io.h>
#include <ntos/ex.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

/* BIOS Parameter Block (FAT32). */
typedef struct PACKED _FAT_BPB {
    UINT8  Jump[3];
    UINT8  OemName[8];
    UINT16 BytesPerSector;      /* 0x0B */
    UINT8  SectorsPerCluster;   /* 0x0D */
    UINT16 ReservedSectors;     /* 0x0E */
    UINT8  NumFats;             /* 0x10 */
    UINT16 RootEntryCount;      /* 0x11 (0 for FAT32) */
    UINT16 TotalSectors16;      /* 0x13 */
    UINT8  Media;               /* 0x15 */
    UINT16 FatSize16;           /* 0x16 (0 for FAT32) */
    UINT16 SectorsPerTrack;     /* 0x18 */
    UINT16 NumHeads;            /* 0x1A */
    UINT32 HiddenSectors;       /* 0x1C */
    UINT32 TotalSectors32;      /* 0x20 */
    UINT32 FatSize32;           /* 0x24 */
    UINT16 ExtFlags;            /* 0x28 */
    UINT16 FsVersion;           /* 0x2A */
    UINT32 RootCluster;         /* 0x2C */
} FAT_BPB;

/* 8.3 directory entry. */
typedef struct PACKED _FAT_DIRENT {
    UINT8  Name[11];
    UINT8  Attr;
    UINT8  NtReserved;
    UINT8  CreateTimeTenth;
    UINT16 CreateTime;
    UINT16 CreateDate;
    UINT16 AccessDate;
    UINT16 FirstClusterHigh;
    UINT16 WriteTime;
    UINT16 WriteDate;
    UINT16 FirstClusterLow;
    UINT32 FileSize;
} FAT_DIRENT;

#define ATTR_LONG_NAME 0x0F
#define FAT_EOC        0x0FFFFFF8u

static struct {
    BOOLEAN Mounted;
    UINT32  FatStartLba;
    UINT32  DataStartLba;
    UINT32  SectorsPerCluster;
    UINT32  BytesPerCluster;
    UINT32  RootCluster;
} g_fat;

static UINT32 cluster_to_lba(UINT32 cluster)
{
    return g_fat.DataStartLba + (cluster - 2) * g_fat.SectorsPerCluster;
}

/* Read the next cluster in the chain from the FAT. */
static UINT32 fat_next_cluster(UINT32 cluster)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    UINT32 fat_offset = cluster * 4;
    UINT32 lba = g_fat.FatStartLba + fat_offset / ATA_SECTOR_SIZE;
    UINT32 in_sector = fat_offset % ATA_SECTOR_SIZE;

    if (!AtaReadSectors(lba, 1, sector))
        return FAT_EOC;
    UINT32 value = *(UINT32 *)&sector[in_sector];
    return value & 0x0FFFFFFFu;
}

/* Convert "name.ext" to the 11-byte padded 8.3 form ("NAME    EXT"). */
static void to_83(const char *name, UINT8 out[11])
{
    for (int i = 0; i < 11; i++)
        out[i] = ' ';

    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i++] = (UINT8)c;
    }
    while (*name && *name != '.')
        name++;
    if (*name == '.') {
        name++;
        int j = 8;
        while (*name && j < 11) {
            char c = *name++;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            out[j++] = (UINT8)c;
        }
    }
}

NTSTATUS FatMount(void)
{
    UINT8 sector[ATA_SECTOR_SIZE];
    if (!AtaReadSectors(0, 1, sector))
        return STATUS_DEVICE_NOT_READY;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return STATUS_UNSUCCESSFUL; /* no boot signature */

    const FAT_BPB *bpb = (const FAT_BPB *)sector;
    if (bpb->BytesPerSector != ATA_SECTOR_SIZE || bpb->FatSize16 != 0)
        return STATUS_NOT_SUPPORTED; /* expect a 512-byte-sector FAT32 volume */

    g_fat.SectorsPerCluster = bpb->SectorsPerCluster;
    g_fat.BytesPerCluster = bpb->SectorsPerCluster * ATA_SECTOR_SIZE;
    g_fat.FatStartLba = bpb->ReservedSectors;
    g_fat.DataStartLba = bpb->ReservedSectors + bpb->NumFats * bpb->FatSize32;
    g_fat.RootCluster = bpb->RootCluster;
    g_fat.Mounted = TRUE;

    KeLog("[io]   fat32 mounted: %u sec/cluster, fat@%u data@%u root=cluster %u\n",
          (unsigned)g_fat.SectorsPerCluster, (unsigned)g_fat.FatStartLba,
          (unsigned)g_fat.DataStartLba, (unsigned)g_fat.RootCluster);
    return STATUS_SUCCESS;
}

/* Search the directory chain starting at `dir_cluster` for `name83`. */
static BOOLEAN fat_find(UINT32 dir_cluster, const UINT8 name83[11],
                        UINT32 *out_cluster, UINT32 *out_size)
{
    UINT8 *cluster_buf = ExAllocatePoolWithTag(NonPagedPool,
                                               g_fat.BytesPerCluster, 'taF');
    if (!cluster_buf)
        return FALSE;

    BOOLEAN found = FALSE;
    UINT32 cluster = dir_cluster;
    while (cluster < FAT_EOC && !found) {
        if (!AtaReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, cluster_buf))
            break;

        FAT_DIRENT *ents = (FAT_DIRENT *)cluster_buf;
        UINT32 n = g_fat.BytesPerCluster / sizeof(FAT_DIRENT);
        for (UINT32 i = 0; i < n; i++) {
            FAT_DIRENT *e = &ents[i];
            if (e->Name[0] == 0x00)
                goto done; /* end of directory */
            if (e->Name[0] == 0xE5 || e->Attr == ATTR_LONG_NAME)
                continue;
            if (memcmp(e->Name, name83, 11) == 0) {
                *out_cluster = ((UINT32)e->FirstClusterHigh << 16) |
                               e->FirstClusterLow;
                *out_size = e->FileSize;
                found = TRUE;
                break;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
done:
    ExFreePool(cluster_buf);
    return found;
}

NTSTATUS FatLoadFile(const char *name, void **out_buffer, SIZE_T *out_size)
{
    if (!g_fat.Mounted)
        return STATUS_DEVICE_NOT_READY;

    UINT8 name83[11];
    to_83(name, name83);

    UINT32 cluster, size;
    if (!fat_find(g_fat.RootCluster, name83, &cluster, &size))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    void *buffer = ExAllocatePoolWithTag(NonPagedPool, size ? size : 1, 'liF');
    if (!buffer)
        return STATUS_NO_MEMORY;

    UINT8 *chunk = ExAllocatePoolWithTag(NonPagedPool, g_fat.BytesPerCluster,
                                         'taF');
    if (!chunk) {
        ExFreePool(buffer);
        return STATUS_NO_MEMORY;
    }

    SIZE_T pos = 0, remaining = size;
    while (cluster < FAT_EOC && remaining > 0) {
        if (!AtaReadSectors(cluster_to_lba(cluster),
                            (UINT8)g_fat.SectorsPerCluster, chunk)) {
            ExFreePool(chunk);
            ExFreePool(buffer);
            return STATUS_DEVICE_NOT_READY;
        }
        SIZE_T n = remaining < g_fat.BytesPerCluster ? remaining
                                                     : g_fat.BytesPerCluster;
        memcpy((UINT8 *)buffer + pos, chunk, n);
        pos += n;
        remaining -= n;
        cluster = fat_next_cluster(cluster);
    }

    ExFreePool(chunk);
    *out_buffer = buffer;
    *out_size = size;
    KeLog("[io]   loaded '%s' (%lu bytes) from disk\n", name, (unsigned long)size);
    return STATUS_SUCCESS;
}

NTSTATUS IoInitialize(void)
{
    /* File/console objects need Ob but not the disk, so set them up regardless. */
    IoInitializeObjects();

    if (!AtaInitialize())
        return STATUS_NO_SUCH_DEVICE;
    return FatMount();
}
