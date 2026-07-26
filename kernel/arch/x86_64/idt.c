/*
 * arch/x86_64/idt.c - the Interrupt Descriptor Table.
 *
 * All 256 vectors are wired to the assembly stubs in isr.asm (whose addresses
 * are exported via isr_stub_table). Every gate is a 64-bit interrupt gate in
 * the kernel code segment, so interrupts are masked on entry. The handful of
 * catastrophic faults run on the IST1 stack from the TSS.
 */
#include <nt/ntdef.h>
#include <ntos/ke.h>

#define IDT_ENTRIES 256
#define SEL_KCODE   0x08

/* 64-bit interrupt/trap gate descriptor. */
typedef struct PACKED _IDT_ENTRY {
    UINT16 offset_low;
    UINT16 selector;
    UINT8  ist;        /* bits 0..2: IST index (0 = none) */
    UINT8  type_attr;  /* P | DPL | gate type */
    UINT16 offset_mid;
    UINT32 offset_high;
    UINT32 reserved;
} IDT_ENTRY;

typedef struct PACKED _DTR {
    UINT16 limit;
    UINT64 base;
} DTR;

static IDT_ENTRY g_idt[IDT_ENTRIES] ALIGNED(16);

/* Provided by isr.asm: 256 stub entry points. */
extern void *isr_stub_table[IDT_ENTRIES];
extern void KiLoadIdtr(const void *idtr);

static void idt_set_gate(int vector, void *handler, UINT8 ist, UINT8 type_attr)
{
    UINT64 addr = (UINT64)handler;
    g_idt[vector].offset_low  = (UINT16)(addr & 0xFFFF);
    g_idt[vector].selector    = SEL_KCODE;
    g_idt[vector].ist         = ist & 0x7;
    g_idt[vector].type_attr   = type_attr;
    g_idt[vector].offset_mid  = (UINT16)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (UINT32)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved    = 0;
}

void KeInitializeIdt(void)
{
    for (int v = 0; v < IDT_ENTRIES; v++) {
        /* 0x8E = present, DPL 0, 64-bit interrupt gate. */
        UINT8 ist = 0;

        /* Run NMI (#2), double fault (#8), and machine check (#18) on IST1 so
         * they survive a trashed kernel stack. */
        if (v == 2 || v == 8 || v == 18)
            ist = 1;

        idt_set_gate(v, isr_stub_table[v], ist, 0x8E);
    }

    DTR idtr = { .limit = sizeof(g_idt) - 1, .base = (UINT64)&g_idt };
    KiLoadIdtr(&idtr);
}
