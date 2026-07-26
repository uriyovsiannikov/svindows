/*
 * arch/x86_64/gdt.c - the runtime GDT and TSS.
 *
 * Replaces the minimal boot GDT with a full one: null, kernel code/data, user
 * code/data, and a 64-bit Task State Segment. The TSS carries the stack the CPU
 * switches to for ring-0 entry (RSP0) and a dedicated interrupt stack (IST1)
 * used by the most dangerous faults, so a corrupted kernel stack can't take the
 * fault handler down with it.
 */
#include <nt/ntdef.h>
#include <ntos/ke.h>

/* Segment selectors. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B /* index 3, RPL 3 */
#define SEL_UDATA 0x23 /* index 4, RPL 3 */
#define SEL_TSS   0x28 /* index 5 (spans indices 5 and 6) */

/* 64-bit Task State Segment. */
typedef struct PACKED _TSS64 {
    UINT32 reserved0;
    UINT64 rsp0;
    UINT64 rsp1;
    UINT64 rsp2;
    UINT64 reserved1;
    UINT64 ist[7];      /* ist[0] == IST1 */
    UINT64 reserved2;
    UINT16 reserved3;
    UINT16 iomap_base;
} TSS64;

/* GDTR/IDTR image handed to lgdt/lidt. */
typedef struct PACKED _DTR {
    UINT16 limit;
    UINT64 base;
} DTR;

/* The GDT: 5 code/data descriptors + a 16-byte (two-slot) TSS descriptor. */
static UINT64 g_gdt[7];
static TSS64  g_tss ALIGNED(16);

/* Interrupt stacks. Kept simple and static until Mm can allocate them. */
static UINT8 g_ist1_stack[0x4000] ALIGNED(16); /* 16 KiB for critical faults */
static UINT8 g_rsp0_stack[0x4000] ALIGNED(16); /* 16 KiB for ring-0 entry     */

extern void KiLoadGdtr(const void *gdtr);
extern void KiLoadTr(UINT16 selector);

static void gdt_set_tss(int slot, UINT64 base, UINT32 limit)
{
    UINT64 low = 0;
    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;                    /* present, type = available 64-bit TSS */
    low |= ((UINT64)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    g_gdt[slot] = low;
    g_gdt[slot + 1] = (base >> 32) & 0xFFFFFFFFULL; /* upper 32 bits of base */
}

void KeInitializeGdt(void)
{
    g_gdt[0] = 0;                       /* null */
    g_gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel code: P, S, exec/read, L=1 */
    g_gdt[2] = 0x00AF92000000FFFFULL;   /* kernel data: P, S, read/write     */
    g_gdt[3] = 0x00AFFA000000FFFFULL;   /* user code:   DPL 3, exec/read     */
    g_gdt[4] = 0x00AFF2000000FFFFULL;   /* user data:   DPL 3, read/write    */

    /* Point the TSS's stacks at the top of their (downward-growing) buffers. */
    for (int i = 0; i < 7; i++)
        g_tss.ist[i] = 0;
    g_tss.rsp0 = (UINT64)(g_rsp0_stack + sizeof(g_rsp0_stack));
    g_tss.rsp1 = 0;
    g_tss.rsp2 = 0;
    g_tss.ist[0] = (UINT64)(g_ist1_stack + sizeof(g_ist1_stack)); /* IST1 */
    g_tss.iomap_base = sizeof(TSS64);   /* no I/O permission bitmap */

    gdt_set_tss(5, (UINT64)&g_tss, sizeof(TSS64) - 1);

    DTR gdtr = { .limit = sizeof(g_gdt) - 1, .base = (UINT64)&g_gdt };
    KiLoadGdtr(&gdtr);
    KiLoadTr(SEL_TSS);
}
