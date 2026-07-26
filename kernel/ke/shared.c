/*
 * ke/shared.c - KUSER_SHARED_DATA, the read-only page user mode reads directly.
 *
 * Windows maps a shared page at the fixed user address 0x7FFE0000 that the
 * kernel keeps up to date (tick count, system time, ...) so ring 3 can read
 * those values without a system call. We reproduce it: the page is mapped
 * read-only into user space, and the kernel updates it through the direct map
 * on every clock tick. kernel32's GetTickCount/GetSystemTimeAsFileTime read it
 * exactly the way the real ones do.
 */
#include <ntos/ke.h>
#include <ntos/mm.h>
#include <ntos/rtl.h>

#define KUSER_SHARED_DATA_USER_VA 0x000000007FFE0000ULL

/* Field offsets within the page (match the Windows KUSER_SHARED_DATA layout). */
#define KUSD_TickCountMultiplier 0x004 /* ULONG                              */
#define KUSD_SystemTime          0x014 /* KSYSTEM_TIME (Low, High1, High2)   */
#define KUSD_TickCount           0x320 /* KSYSTEM_TIME                       */

/* One tick is 10 ms (the PIT runs at 100 Hz). GetTickCount computes
 * (TickCount * Multiplier) >> 24, so the multiplier is (ms per tick) << 24. */
#define MS_PER_TICK      10u
#define HUNDRED_NS_PER_TICK (MS_PER_TICK * 10000ULL) /* 10 ms in 100 ns units */

static volatile UINT8 *g_kusd; /* kernel (direct-map) alias of the page */

static ALWAYS_INLINE void wr32(UINT32 off, UINT32 v)
{
    *(volatile UINT32 *)(g_kusd + off) = v;
}

void KeInitializeSharedData(void)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS)
        return;

    /* Read-only in user space (no PTE_WRITE); the kernel writes via the
     * direct map, which is writable. */
    MmMapPage(KUSER_SHARED_DATA_USER_VA, pa, PTE_USER);
    g_kusd = (volatile UINT8 *)MmPhysToVirt(pa);
    memset((void *)g_kusd, 0, PAGE_SIZE);

    wr32(KUSD_TickCountMultiplier, MS_PER_TICK << 24);
    KeUpdateSharedData(KeGetTickCount());

    KeLog("[ke]   KUSER_SHARED_DATA mapped at 0x%lx\n",
          (unsigned long)KUSER_SHARED_DATA_USER_VA);
}

/* Called from the clock tick with the current 64-bit tick count. A KSYSTEM_TIME
 * is written High1, Low, High2 so a lock-free reader that sees High1 == High2
 * has a consistent pair. */
void KeUpdateSharedData(UINT64 ticks)
{
    if (!g_kusd)
        return;

    UINT32 tlow = (UINT32)ticks;
    UINT32 thigh = (UINT32)(ticks >> 32);
    wr32(KUSD_TickCount + 0x4, thigh);       /* High1Time */
    wr32(KUSD_TickCount + 0x0, tlow);        /* LowPart   */
    wr32(KUSD_TickCount + 0x8, thigh);       /* High2Time */

    /* System time as 100 ns units since boot (a stand-in for real wall time). */
    UINT64 systime = ticks * HUNDRED_NS_PER_TICK;
    UINT32 slow = (UINT32)systime;
    UINT32 shigh = (UINT32)(systime >> 32);
    wr32(KUSD_SystemTime + 0x4, shigh);
    wr32(KUSD_SystemTime + 0x0, slow);
    wr32(KUSD_SystemTime + 0x8, shigh);
}
