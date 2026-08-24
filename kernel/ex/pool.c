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

/* Per-block header. Kept at 32 bytes so the payload that follows is 16-aligned. */
typedef struct _POOL_NODE {
    struct _POOL_NODE *prev; /* address-order neighbour (lower) */
    struct _POOL_NODE *next; /* address-order neighbour (higher) */
    SIZE_T             size; /* payload bytes, excluding this header */
    UINT32             free;
    UINT32             tag;
} POOL_NODE;

#define POOL_HDR   sizeof(POOL_NODE)
#define POOL_ALIGN 16
#define POOL_MIN_PAYLOAD 16
#define POOL_GROW_CHUNK  0x10000 /* commit 64 KiB at a time */

static POOL_NODE *g_head;
static POOL_NODE *g_tail;
static UINT64     g_heap_end;   /* first uncommitted virtual address */
static SIZE_T     g_bytes_in_use;

static ALWAYS_INLINE POOL_NODE *payload_to_node(void *p)
{
    return (POOL_NODE *)((UINT8 *)p - POOL_HDR);
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
    node->free = TRUE;
    node->tag = 0;
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
    rest->free = TRUE;
    rest->tag = 0;
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
    SIZE_T need = ALIGN_UP_BY(size, POOL_ALIGN);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (POOL_NODE *n = g_head; n; n = n->next) {
            if (n->free && n->size >= need) {
                pool_split(n, need);
                n->free = FALSE;
                n->tag = tag;
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
    if (node->free) {
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "double free in kernel pool");
        return;
    }

    node->free = TRUE;
    node->tag = 0;
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
