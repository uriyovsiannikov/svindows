/*
 * ex/pool.c - the kernel pool allocator (ExAllocatePool / ExFreePool).
 *
 * A first-fit heap over a single virtually-contiguous arena at
 * MM_KERNEL_HEAP_BASE. The arena is committed on demand: when no free block is
 * large enough, more pages are pulled from the PMM and mapped onto the end of
 * the arena. Blocks are tracked by an address-ordered doubly linked list, so a
 * freed block coalesces with whichever physical neighbours are also free.
 */
#include <ntos/ex.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

/* Per-block header. Kept 16-byte sized so the payload that follows is
 * 16-aligned. `requested` is what the caller asked for; the bytes between
 * requested and size hold a guard pattern, so a write that runs off the end of
 * a payload is caught and attributed to its owner's tag. */
typedef struct _POOL_NODE {
    struct _POOL_NODE *prev; /* address-order neighbour (lower) */
    struct _POOL_NODE *next; /* address-order neighbour (higher) */
    SIZE_T             size; /* payload bytes, excluding this header */
    SIZE_T             requested; /* bytes the caller asked for */
    UINT32             free;
    UINT32             tag;
    UINT32             magic;
    UINT32             reserved;
} POOL_NODE;

#define POOL_HDR   sizeof(POOL_NODE)
#define POOL_ALIGN 16
#define POOL_MIN_PAYLOAD 16
#define POOL_GROW_CHUNK  0x10000 /* commit 64 KiB at a time */
#define POOL_MAGIC       0x4C4F4F50u /* 'POOL' */
#define POOL_GUARD       16          /* guard bytes past every payload */
#define POOL_GUARD_BYTE  0xAB

static POOL_NODE *g_head;
static POOL_NODE *g_tail;
static UINT64     g_heap_end;   /* first uncommitted virtual address */
static SIZE_T     g_bytes_in_use;
static UINT8      g_pool_broken; /* corruption reported; stop re-reporting */

static ALWAYS_INLINE POOL_NODE *payload_to_node(void *p)
{
    return (POOL_NODE *)((UINT8 *)p - POOL_HDR);
}

static void pool_arm_guard(POOL_NODE *n)
{
    UINT8 *p = (UINT8 *)n + POOL_HDR;
    for (SIZE_T i = n->requested; i < n->size; i++)
        p[i] = POOL_GUARD_BYTE;
}

static BOOLEAN pool_guard_intact(POOL_NODE *n)
{
    const UINT8 *p = (const UINT8 *)n + POOL_HDR;
    for (SIZE_T i = n->requested; i < n->size; i++)
        if (p[i] != POOL_GUARD_BYTE)
            return FALSE;
    return TRUE;
}

static void pool_describe(const char *label, POOL_NODE *n)
{
    if (!n) {
        KeLog("[ex]   %s: (none)\n", label);
        return;
    }
    UINT32 tag = n->tag;
    KeLog("[ex]   %s: node 0x%lx size %lu req %lu %s tag '%c%c%c%c'\n",
          label, (unsigned long)(UINT64)n, (unsigned long)n->size,
          (unsigned long)n->requested, n->free ? "free" : "used",
          (char)(tag & 0xff ? tag & 0xff : '.'),
          (char)((tag >> 8) & 0xff ? (tag >> 8) & 0xff : '.'),
          (char)((tag >> 16) & 0xff ? (tag >> 16) & 0xff : '.'),
          (char)((tag >> 24) & 0xff ? (tag >> 24) & 0xff : '.'));
}

/* Walk the address-ordered list and check every invariant the allocator
 * depends on. Blocks tile [MM_KERNEL_HEAP_BASE, g_heap_end) exactly, so a
 * broken tiling points straight at the block that was overrun. The walk is
 * O(blocks), so it runs periodically rather than on every call; the O(1) guard
 * check in ExFreePool is what usually names the culprit first. */
#define POOL_VALIDATE_EVERY 64

static void pool_validate(const char *where)
{
    if (g_pool_broken)
        return;

    POOL_NODE *prev = NULL;
    UINT64 addr = MM_KERNEL_HEAP_BASE;
    UINT32 index = 0;

    for (POOL_NODE *n = g_head; n; prev = n, n = n->next, index++) {
        const char *bad = NULL;
        if ((UINT64)n != addr)
            bad = "block is not where the previous block ends";
        else if (n->magic != POOL_MAGIC)
            bad = "header magic destroyed";
        else if (n->prev != prev)
            bad = "back link broken";
        else if (n->size == 0 || (UINT64)n + POOL_HDR + n->size > g_heap_end)
            bad = "payload size outside the arena";
        else if (!n->free && n->requested > n->size)
            bad = "requested size larger than the block";
        else if (!n->free && !pool_guard_intact(n))
            bad = "payload overrun into the guard bytes";

        if (bad) {
            g_pool_broken = 1;
            KeLog("[ex]   POOL CORRUPTION at %s: %s (block #%lu, expected "
                  "0x%lx, arena end 0x%lx)\n",
                  where, bad, (unsigned long)index, (unsigned long)addr,
                  (unsigned long)g_heap_end);
            pool_describe("victim   ", n);
            pool_describe("previous ", prev);
            if (prev && prev->prev)
                pool_describe("before   ", prev->prev);
            return;
        }
        addr = (UINT64)n + POOL_HDR + n->size;
    }

    if (addr != g_heap_end) {
        g_pool_broken = 1;
        KeLog("[ex]   POOL CORRUPTION at %s: list ends at 0x%lx, arena ends at "
              "0x%lx (%lu blocks walked)\n", where, (unsigned long)addr,
              (unsigned long)g_heap_end, (unsigned long)index);
        pool_describe("last     ", prev);
    }
}

/* Commit at least `need` payload bytes at the end of the arena. */
static BOOLEAN pool_grow(SIZE_T need)
{
    SIZE_T want = ALIGN_UP_BY(need + POOL_HDR, POOL_GROW_CHUNK);
    UINT64 start = g_heap_end;

    for (UINT64 off = 0; off < want; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return FALSE;
        if (!MmMapPage(start + off, pa, PTE_WRITE))
            return FALSE;
    }
    g_heap_end += want;
    KeLog("[ex]   grow +%lu KiB -> arena %lu KiB (in-use %lu)\n",
          (unsigned long)(want >> 10),
          (unsigned long)((g_heap_end - MM_KERNEL_HEAP_BASE) >> 10),
          (unsigned long)g_bytes_in_use);

    /* If the current tail is free, just extend it; otherwise append a node. */
    if (g_tail && g_tail->free) {
        g_tail->size += want;
        return TRUE;
    }

    POOL_NODE *node = (POOL_NODE *)start;
    node->size = want - POOL_HDR;
    node->requested = 0;
    node->free = TRUE;
    node->tag = 0;
    node->magic = POOL_MAGIC;
    node->reserved = 0;
    node->prev = g_tail;
    node->next = NULL;
    if (g_tail)
        g_tail->next = node;
    else
        g_head = node;
    g_tail = node;
    return TRUE;
}

void ExInitializePool(void)
{
    g_head = g_tail = NULL;
    g_heap_end = MM_KERNEL_HEAP_BASE;
    g_bytes_in_use = 0;

    if (!pool_grow(POOL_GROW_CHUNK - POOL_HDR))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot commit kernel pool");

    KeLog("[ex]   pool: kernel heap at 0x%lx, initial %lu KiB committed\n",
          (unsigned long)MM_KERNEL_HEAP_BASE,
          (unsigned long)(POOL_GROW_CHUNK >> 10));
}

/* Split `node` so it holds exactly `need` payload bytes, if the remainder is
 * big enough to be its own block. */
static void pool_split(POOL_NODE *node, SIZE_T need)
{
    if (node->size < need + POOL_HDR + POOL_MIN_PAYLOAD)
        return;

    POOL_NODE *rest = (POOL_NODE *)((UINT8 *)node + POOL_HDR + need);
    rest->size = node->size - need - POOL_HDR;
    rest->requested = 0;
    rest->free = TRUE;
    rest->tag = 0;
    rest->magic = POOL_MAGIC;
    rest->reserved = 0;
    rest->prev = node;
    rest->next = node->next;
    if (rest->next)
        rest->next->prev = rest;
    else
        g_tail = rest;
    node->next = rest;
    node->size = need;
}

PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{
    (void)type; /* everything is non-paged for now */

    if (size == 0)
        return NULL;
    static UINT32 calls;
    if (++calls % POOL_VALIDATE_EVERY == 0)
        pool_validate("allocate");
    /* Round up for alignment and add the guard bytes an overrun lands in. */
    SIZE_T need = ALIGN_UP_BY(size, POOL_ALIGN) + POOL_GUARD;

    for (int attempt = 0; attempt < 2; attempt++) {
        for (POOL_NODE *n = g_head; n; n = n->next) {
            if (n->free && n->size >= need) {
                pool_split(n, need);
                n->free = FALSE;
                n->tag = tag;
                n->requested = size;
                pool_arm_guard(n);
                g_bytes_in_use += n->size;
                return (UINT8 *)n + POOL_HDR;
            }
        }
        /* No fit: grow the arena and try once more. */
        if (!pool_grow(need)) {
            static UINT8 logged;
            if (!logged) {
                logged = 1;
                KeLog("[ex]   pool exhausted: in-use %lu KiB, arena %lu KiB, "
                      "want %lu bytes (tag '%c%c%c%c')\n",
                      (unsigned long)(g_bytes_in_use >> 10),
                      (unsigned long)((g_heap_end - MM_KERNEL_HEAP_BASE) >> 10),
                      (unsigned long)size,
                      (char)(tag & 0xff), (char)((tag >> 8) & 0xff),
                      (char)((tag >> 16) & 0xff), (char)((tag >> 24) & 0xff));
            }
            break;
        }
    }
    return NULL;
}

PVOID ExAllocatePool(POOL_TYPE type, SIZE_T size)
{
    return ExAllocatePoolWithTag(type, size, 0);
}

void ExFreePool(PVOID p)
{
    if (!p)
        return;

    POOL_NODE *node = payload_to_node(p);
    if (node->magic != POOL_MAGIC && !g_pool_broken) {
        g_pool_broken = 1;
        KeLog("[ex]   POOL CORRUPTION at free: header magic destroyed\n");
        pool_describe("victim   ", node);
    }
    if (node->free) {
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "double free in kernel pool");
        return;
    }
    if (!g_pool_broken && !pool_guard_intact(node)) {
        g_pool_broken = 1;
        KeLog("[ex]   POOL CORRUPTION at free: payload overrun past its end\n");
        pool_describe("victim   ", node);
    }

    node->free = TRUE;
    node->tag = 0;
    node->requested = 0;
    g_bytes_in_use -= node->size;

    /* Coalesce with the higher neighbour. */
    POOL_NODE *next = node->next;
    if (next && next->free) {
        node->size += POOL_HDR + next->size;
        node->next = next->next;
        if (next->next)
            next->next->prev = node;
        else
            g_tail = node;
    }

    /* Coalesce with the lower neighbour. */
    POOL_NODE *prev = node->prev;
    if (prev && prev->free) {
        prev->size += POOL_HDR + node->size;
        prev->next = node->next;
        if (node->next)
            node->next->prev = prev;
        else
            g_tail = prev;
    }
}

SIZE_T ExPoolBytesInUse(void)
{
    return g_bytes_in_use;
}
