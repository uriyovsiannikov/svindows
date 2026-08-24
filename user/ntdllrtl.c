/*
 * user/ntdllrtl.c - the C side of NTOS ntdll.dll.
 *
 * Real Windows binaries import substantial runtime support directly from
 * ntdll: the Rtl heap, critical sections, UNICODE_STRING helpers and a small
 * CRT surface. Until now those imports resolved to return-0 stubs, which
 * silently broke user32/gdi32 internals (every RtlAllocateHeap returned NULL,
 * critical sections did nothing). This module provides honest minimal
 * implementations over the native Nt* services.
 */

typedef void              *HANDLE;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef long long          LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long      SIZE_T;
typedef unsigned short     WCHAR;
typedef unsigned short     USHORT;
typedef unsigned int       UINT32;
typedef unsigned short      UINT16;
typedef long               NTSTATUS;
typedef const WCHAR       *PCWSTR;
typedef WCHAR             *PWSTR;

#define NT_SUCCESS(s)   ((NTSTATUS)(s) >= 0)
#define TRUE  1
#define FALSE 0
typedef int BOOLEAN;
#define STATUS_SUCCESS           ((NTSTATUS)0)
#define STATUS_NO_MEMORY         ((NTSTATUS)0xC0000017)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NOT_FOUND         ((NTSTATUS)0xC0000225)
#define STATUS_NOT_IMPLEMENTED   ((NTSTATUS)0xC0000001L)

/* Native services exported by the asm half of this DLL. */
extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG ZeroBits, ULONGLONG *RegionSize,
                                        DWORD AllocationType, DWORD Protect);
extern void     NtDisplayString(const char *text);

static DWORD read_teb_dword(ULONGLONG off)
{
    DWORD v;
    __asm__ volatile("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static ULONGLONG read_teb_qword(ULONGLONG off)
{
    ULONGLONG v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static void write_teb_dword(ULONGLONG off, DWORD val)
{
    __asm__ volatile("movl %0, %%gs:(%1)" : : "r"(val), "r"(off));
}

void *memset(void *dst, int v, SIZE_T n);
void *memcpy(void *dst, const void *src, SIZE_T n);

/* ------------------------------------------------------------------ */
/* Memory functions (real binaries import these from ntdll)            */
/* ------------------------------------------------------------------ */

void *memset(void *dst, int v, SIZE_T n)
{
    BYTE *p = (BYTE *)dst;
    while (n--)
        *p++ = (BYTE)v;
    return dst;
}

void *memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return (int)*x - (int)*y;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The Rtl heap: a coalescing free-list allocator served from          */
/* NtAllocateVirtualMemory blocks. Every heap handle maps to the same  */
/* process-wide heap, matching the single-process model.               */
/* ------------------------------------------------------------------ */

typedef struct _RTL_BLOCK {
    SIZE_T Size;                 /* payload bytes */
    struct _RTL_BLOCK *Next;     /* free-list link when not in use */
} RTL_BLOCK;

#define RTL_HEAP_MAGIC 0xFFEEFFEEULL

typedef struct _RTL_HEAP {
    ULONGLONG Magic;
    RTL_BLOCK *FreeHead;
    BYTE *Cursor;                /* bump cursor inside the current segment */
    BYTE *SegmentEnd;
} RTL_HEAP;

static RTL_HEAP g_process_heap;
static LONG g_heap_lock;

static void heap_lock(void)
{
    while (__sync_lock_test_and_set(&g_heap_lock, 1))
        ; /* brief busy wait: allocations never hold the lock long */
}

static void heap_unlock(void)
{
    __sync_lock_release(&g_heap_lock);
}

static BOOLEAN heap_grow(RTL_HEAP *heap, SIZE_T bytes)
{
    void *base = 0;
    ULONGLONG size = (bytes + 0xFFFFULL) & ~0xFFFFULL;
    if (size < 0x10000)
        size = 0x10000;
    if (!NT_SUCCESS(NtAllocateVirtualMemory((HANDLE)-1LL, &base, 0, &size,
                                            0x3000 /* MEM_COMMIT|RESERVE */,
                                            0x04 /* PAGE_READWRITE */)))
        return FALSE;
    heap->Cursor = (BYTE *)base;
    heap->SegmentEnd = (BYTE *)base + size;
    return TRUE;
}

HANDLE RtlCreateHeap(ULONG flags, void *base, SIZE_T reserve, SIZE_T commit,
                     void *lock, void *params)
{
    (void)flags; (void)base; (void)reserve; (void)commit; (void)lock;
    (void)params;
    if (!g_process_heap.Magic) {
        g_process_heap.Magic = RTL_HEAP_MAGIC;
        g_process_heap.FreeHead = 0;
        g_process_heap.Cursor = 0;
        g_process_heap.SegmentEnd = 0;
    }
    return (HANDLE)&g_process_heap;
}

HANDLE RtlDestroyHeap(HANDLE heap)
{
    (void)heap;
    return 0;
}

void *RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size)
{
    (void)heap;
    if (!size)
        size = 1;
    size = (size + 15UL) & ~15UL;

    heap_lock();
    RTL_HEAP *h = &g_process_heap;
    if (!h->Magic) {
        h->Magic = RTL_HEAP_MAGIC;
        h->FreeHead = 0;
        h->Cursor = 0;
        h->SegmentEnd = 0;
    }

    /* Best fit from the free list (first fit keeps it simple). */
    RTL_BLOCK **link = &h->FreeHead;
    for (RTL_BLOCK *b = h->FreeHead; b; b = b->Next) {
        if (b->Size >= size) {
            *link = b->Next;
            heap_unlock();
            if (flags & 0x8 /* HEAP_ZERO_MEMORY */)
                memset(b + 1, 0, size);
            return b + 1;
        }
        link = &b->Next;
    }

    if ((SIZE_T)(h->SegmentEnd - h->Cursor) < size + sizeof(RTL_BLOCK)) {
        if (!heap_grow(h, size + sizeof(RTL_BLOCK))) {
            heap_unlock();
            return 0;
        }
    }
    RTL_BLOCK *block = (RTL_BLOCK *)h->Cursor;
    h->Cursor += sizeof(RTL_BLOCK) + size;
    block->Size = size;
    heap_unlock();
    if (flags & 0x8)
        memset(block + 1, 0, size);
    return block + 1;
}

BOOL RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    if (!ptr)
        return 1;
    RTL_BLOCK *block = (RTL_BLOCK *)ptr - 1;
    heap_lock();
    RTL_HEAP *h = &g_process_heap;
    block->Next = h->FreeHead;
    h->FreeHead = block;
    heap_unlock();
    return 1;
}

void *RtlReAllocateHeap(HANDLE heap, ULONG flags, void *ptr, SIZE_T size)
{
    if (!ptr)
        return 0;
    RTL_BLOCK *old = (RTL_BLOCK *)ptr - 1;
    void *fresh = RtlAllocateHeap(heap, flags, size);
    if (!fresh)
        return 0;
    memcpy(fresh, ptr, old->Size < size ? old->Size : size);
    RtlFreeHeap(heap, flags, ptr);
    return fresh;
}

SIZE_T RtlSizeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    return ptr ? ((RTL_BLOCK *)ptr - 1)->Size : 0;
}

void RtlFlushHeaps(void)
{
}

BOOLEAN RtlValidateHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags; (void)ptr;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Critical sections: recursive spin locks keyed by the thread id.     */
/* ------------------------------------------------------------------ */

typedef struct _RTL_CRITICAL_SECTION_NT {
    void *DebugInfo;          /* 0x00 */
    LONG LockCount;           /* 0x08 */
    LONG RecursionCount;      /* 0x0C */
    HANDLE OwningThread;      /* 0x10 */
    HANDLE LockSemaphore;     /* 0x18 */
    ULONGLONG SpinCount;      /* 0x20 */
} RTL_CRITICAL_SECTION_NT;

NTSTATUS RtlInitializeCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    c->DebugInfo = 0;
    c->LockCount = -1;
    c->RecursionCount = 0;
    c->OwningThread = 0;
    c->LockSemaphore = 0;
    c->SpinCount = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlInitializeCriticalSectionAndSpinCount(void *cs, ULONG count)
{
    NTSTATUS st = RtlInitializeCriticalSection(cs);
    ((RTL_CRITICAL_SECTION_NT *)cs)->SpinCount = count;
    return st;
}

NTSTATUS RtlEnterCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    ULONGLONG tid = read_teb_qword(0x48); /* ClientId.UniqueThread */
    if (c->OwningThread == (HANDLE)tid) {
        c->RecursionCount++;
        return STATUS_SUCCESS;
    }
    for (;;) {
        if (__sync_bool_compare_and_swap(&c->LockCount, -1, 0)) {
            c->OwningThread = (HANDLE)tid;
            c->RecursionCount = 1;
            return STATUS_SUCCESS;
        }
        __asm__ volatile("pause");
    }
}

NTSTATUS RtlLeaveCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    if (--c->RecursionCount == 0) {
        c->OwningThread = 0;
        __sync_lock_release(&c->LockCount);
        c->LockCount = -1;
    }
    return STATUS_SUCCESS;
}

BOOL RtlTryEnterCriticalSection(void *cs)
{
    RTL_CRITICAL_SECTION_NT *c = (RTL_CRITICAL_SECTION_NT *)cs;
    ULONGLONG tid = read_teb_qword(0x48);
    if (c->OwningThread == (HANDLE)tid) {
        c->RecursionCount++;
        return TRUE;
    }
    if (!__sync_bool_compare_and_swap(&c->LockCount, -1, 0))
        return FALSE;
    c->OwningThread = (HANDLE)tid;
    c->RecursionCount = 1;
    return TRUE;
}

NTSTATUS RtlDeleteCriticalSection(void *cs)
{
    memset(cs, 0, sizeof(RTL_CRITICAL_SECTION_NT));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SRW locks: exclusive bit + shared counter in one pointer.           */
/* ------------------------------------------------------------------ */

void RtlInitializeSRWLock(void *srwlock)
{
    *(ULONGLONG *)srwlock = 0;
}

void RtlAcquireSRWLockExclusive(void *srwlock)
{
    while (__sync_lock_test_and_set((unsigned int *)srwlock, 1))
        __asm__ volatile("pause");
}

void RtlReleaseSRWLockExclusive(void *srwlock)
{
    __sync_lock_release((unsigned int *)srwlock);
}

void RtlAcquireSRWLockShared(void *srwlock)
{
    unsigned int v;
    for (;;) {
        v = *(volatile unsigned int *)srwlock;
        if (!(v & 1u) &&
            __sync_bool_compare_and_swap((unsigned int *)srwlock,
                                         v, v + 2))
            return;
        __asm__ volatile("pause");
    }
}

void RtlReleaseSRWLockShared(void *srwlock)
{
    __sync_fetch_and_sub((unsigned int *)srwlock, 2);
}

/* Slim resources map onto the same primitives. */
void RtlInitializeResource(void *r)      { memset(r, 0, 24); }
void RtlDeleteResource(void *r)          { (void)r; }
void RtlAcquireResourceExclusive(void *r, BOOLEAN wait)
{ (void)wait; RtlAcquireSRWLockExclusive(r); }
void RtlReleaseResource(void *r)         { RtlReleaseSRWLockExclusive(r); }
ULONG RtlQueryResourcePolicy(void *r)    { (void)r; return 0; }

/* ------------------------------------------------------------------ */
/* UNICODE_STRING helpers                                              */
/* ------------------------------------------------------------------ */

typedef struct _UNICODE_STRING_NT {
    WORD Length;             /* bytes */
    WORD MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING_NT;

typedef struct _STRING_NT {
    WORD Length;
    WORD MaximumLength;
    char *Buffer;
} STRING_NT;

void RtlInitUnicodeString(UNICODE_STRING_NT *ustr, PCWSTR text)
{
    UINT n = 0;
    while (text && text[n])
        n++;
    ustr->Length = (WORD)(n * sizeof(WCHAR));
    ustr->MaximumLength = (WORD)(n ? n * sizeof(WCHAR) + 2 : 0);
    ustr->Buffer = (WCHAR *)text;
}

void RtlInitAnsiString(STRING_NT *astr, const char *text)
{
    UINT n = 0;
    while (text && text[n])
        n++;
    astr->Length = (WORD)n;
    astr->MaximumLength = (WORD)(n + 1);
    astr->Buffer = (char *)text;
}

NTSTATUS RtlFreeUnicodeString(UNICODE_STRING_NT *ustr)
{
    if (ustr && ustr->Buffer)
        RtlFreeHeap(0, 0, ustr->Buffer);
    if (ustr)
        memset(ustr, 0, sizeof(*ustr));
    return STATUS_SUCCESS;
}

LONG RtlCompareUnicodeString(const UNICODE_STRING_NT *a,
                             const UNICODE_STRING_NT *b, ULONG case_sensitive)
{
    UINT na = a->Length / 2, nb = b->Length / 2;
    UINT i;
    for (i = 0; i < na && i < nb; i++) {
        WCHAR ca = a->Buffer[i], cb = b->Buffer[i];
        if (!case_sensitive) {
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
        }
        if (ca != cb)
            return (LONG)ca - (LONG)cb;
    }
    return (LONG)(na - nb);
}

BOOLEAN RtlEqualUnicodeString(const UNICODE_STRING_NT *a,
                              const UNICODE_STRING_NT *b,
                              ULONG case_sensitive)
{
    return RtlCompareUnicodeString(a, b, case_sensitive) == 0;
}

NTSTATUS RtlAnsiStringToUnicodeString(UNICODE_STRING_NT *dst,
                                      const STRING_NT *src, BOOLEAN allocate)
{
    UINT chars = src->Length;
    if (!allocate) {
        if (dst->MaximumLength < (WORD)((chars + 1) * sizeof(WCHAR)))
            return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    } else {
        dst->Buffer =
            RtlAllocateHeap(0, 0, (chars + 1) * sizeof(WCHAR));
        if (!dst->Buffer)
            return STATUS_NO_MEMORY;
        dst->MaximumLength = (WORD)((chars + 1) * sizeof(WCHAR));
    }
    for (UINT i = 0; i < chars; i++)
        dst->Buffer[i] = (WCHAR)(BYTE)src->Buffer[i];
    dst->Buffer[chars] = 0;
    dst->Length = (WORD)(chars * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS RtlCreateUnicodeStringFromAsciiz(UNICODE_STRING_NT *out,
                                          const char *text)
{
    UINT n = 0;
    while (text[n])
        n++;
    out->Buffer = RtlAllocateHeap(0, 8 /* zero */, (n + 1) * sizeof(WCHAR));
    if (!out->Buffer)
        return STATUS_NO_MEMORY;
    for (UINT i = 0; i < n; i++)
        out->Buffer[i] = (WCHAR)(BYTE)text[i];
    out->Buffer[n] = 0;
    out->Length = (WORD)(n * sizeof(WCHAR));
    out->MaximumLength = (WORD)((n + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS RtlMultiByteToUnicodeN(WCHAR *dst, ULONG dst_bytes, ULONG *written,
                                const char *src, ULONG src_len)
{
    ULONG n = src_len;
    if (n > dst_bytes / 2)
        n = dst_bytes / 2;
    for (ULONG i = 0; i < n; i++)
        dst[i] = (WCHAR)(BYTE)src[i];
    if (written)
        *written = n * 2;
    return STATUS_SUCCESS;
}

NTSTATUS RtlMultiByteToUnicodeSize(ULONG *size, const char *src, ULONG len)
{
    (void)src;
    *size = len * 2;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeToMultiByteN(char *dst, ULONG dst_len, ULONG *written,
                                const WCHAR *src, ULONG src_bytes)
{
    ULONG n = src_bytes / 2;
    if (n > dst_len)
        n = dst_len;
    for (ULONG i = 0; i < n; i++)
        dst[i] = (char)(src[i] < 0x100 ? src[i] : '?');
    if (written)
        *written = n;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeToMultiByteSize(ULONG *size, const WCHAR *src,
                                   ULONG src_bytes)
{
    (void)src;
    *size = src_bytes / 2;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnicodeStringToInteger(const UNICODE_STRING_NT *ustr,
                                   ULONG base, ULONG *value)
{
    if (!ustr || !value)
        return STATUS_INVALID_PARAMETER;
    UINT n = ustr->Length / 2;
    UINT i = 0;
    while (i < n &&
           (ustr->Buffer[i] == ' ' ||
            (ustr->Buffer[i] >= 9 && ustr->Buffer[i] <= 13)))
        i++;
    ULONG negative = 0;
    if (i < n && (ustr->Buffer[i] == '-' || ustr->Buffer[i] == '+')) {
        negative = ustr->Buffer[i] == '-';
        i++;
    }
    if (!base || base == 2 || base == 8 || base == 10 || base == 16) {
        /* default detection when base==0 */
    } else
        return STATUS_INVALID_PARAMETER;
    if (!base) {
        base = 10;
        if (i + 1 < n && ustr->Buffer[i] == '0' &&
            (ustr->Buffer[i + 1] | 32) == 'x') {
            base = 16;
            i += 2;
        } else if (i < n && ustr->Buffer[i] == '0')
            base = 8;
    }
    ULONGLONG acc = 0;
    BOOLEAN any = FALSE;
    for (; i < n; i++) {
        WCHAR c = ustr->Buffer[i];
        ULONG d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        acc = acc * base + d;
        any = TRUE;
    }
    if (!any)
        return STATUS_INVALID_PARAMETER;
    *value = negative ? (ULONG)-(LONG)acc : (ULONG)acc;
    return STATUS_SUCCESS;
}

BOOLEAN RtlGetIntegerAtom(PCWSTR text, WORD *atom)
{
    if (!text)
        return FALSE;
    UINT i = 0;
    if (text[i] == '#')
        i++;
    ULONG value = 0;
    BOOLEAN any = FALSE;
    for (; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9')
            return FALSE;
        value = value * 10 + (ULONG)(text[i] - '0');
        if (value > 65535)
            return FALSE;
        any = TRUE;
    }
    if (!any || !value)
        return FALSE;
    if (atom)
        *atom = (WORD)value;
    return TRUE;
}

NTSTATUS RtlPrefixString(const STRING_NT *prefix, const STRING_NT *str,
                         BOOLEAN case_sensitive)
{
    if (prefix->Length > str->Length)
        return STATUS_INVALID_PARAMETER; /* not a prefix */
    for (WORD i = 0; i < prefix->Length; i++) {
        char a = str->Buffer[i], b = prefix->Buffer[i];
        if (!case_sensitive) {
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
        }
        if (a != b)
            return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

ULONG RtlLengthRequiredSid(BYTE count)
{
    return 8ul + 4ul * count;
}

/* ------------------------------------------------------------------ */
/* Wide/ANSI string and conversion surface                             */
/* ------------------------------------------------------------------ */

int wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

int _wcsicmp(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (LONG)ca - (LONG)cb;
    }
}

int _wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T count)
{
    for (SIZE_T i = 0; i < count; i++) {
        WCHAR ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (LONG)ca - (LONG)cb;
    }
    return 0;
}

SIZE_T wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    while (s[n])
        n++;
    return n;
}

WCHAR *wcsncpy_s(WCHAR *dst, SIZE_T cap, const WCHAR *src, SIZE_T count)
{
    if (!dst || !cap)
        return dst;
    SIZE_T n = 0;
    while (src && src[n] && n < count - 1)
        n++;
    memcpy(dst, src, n * sizeof(WCHAR));
    dst[n] = 0;
    return dst;
}

NTSTATUS wcscpy_s(WCHAR *dst, SIZE_T cap, const WCHAR *src)
{
    if (!dst || !cap)
        return STATUS_INVALID_PARAMETER;
    SIZE_T n = wcslen(src ? src : L"");
    if (n >= cap) {
        dst[0] = 0;
        return 0x8007007AL; /* E_INVALIDARG-ish buffer error */
    }
    memcpy(dst, src, (n + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS wcscat_s(WCHAR *dst, SIZE_T cap, const WCHAR *src)
{
    SIZE_T used = wcslen(dst);
    return wcscpy_s(dst + used, cap - used, src);
}

NTSTATUS wcsncat_s(WCHAR *dst, SIZE_T cap, const WCHAR *src, SIZE_T count)
{
    SIZE_T used = wcslen(dst);
    if (used >= cap)
        return STATUS_INVALID_PARAMETER;
    SIZE_T i = 0;
    while (i < count && src[i]) {
        if (used + i >= cap - 1)
            return STATUS_INVALID_PARAMETER;
        dst[used + i] = src[i];
        i++;
    }
    dst[used + i] = 0;
    return STATUS_SUCCESS;
}

const WCHAR *wcsrchr(const WCHAR *text, WCHAR c)
{
    const WCHAR *found = 0;
    for (; *text; text++)
        if (*text == c)
            found = text;
    return found;
}

const char *strrchr(const char *text, int c)
{
    const char *found = 0;
    for (; *text; text++)
        if (*text == (char)c)
            found = text;
    return found;
}

int _stricmp(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (unsigned char)ca - (unsigned char)cb;
    }
}

ULONG __cdecl _wtoi(const WCHAR *text)
{
    ULONG v = 0;
    BOOLEAN negative = FALSE;
    if (*text == '-') {
        negative = TRUE;
        text++;
    }
    while (*text >= '0' && *text <= '9')
        v = v * 10 + (ULONG)(*text++ - '0');
    return negative ? (ULONG)-(LONG)v : v;
}

NTSTATUS wcstol(const WCHAR *text, WCHAR **end, LONG base, LONG *out)
{
    ULONG v = 0;
    BOOLEAN any = FALSE, neg = FALSE;
    if (*text == '-') {
        neg = TRUE;
        text++;
    }
    for (;;) {
        ULONG d;
        if (*text >= '0' && *text <= '9') d = (ULONG)(*text - '0');
        else if (base == 16 && (*text | 32) >= 'a' && (*text | 32) <= 'f')
            d = (ULONG)((*text | 32) - 'a' + 10);
        else break;
        if (d >= (ULONG)base) break;
        v = v * (ULONG)base + d;
        any = TRUE;
        text++;
    }
    if (end)
        *end = (WCHAR *)(any ? text : (const WCHAR *)text);
    *out = neg ? -(LONG)v : (LONG)v;
    return any ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS wcstoul(const WCHAR *text, WCHAR **end, LONG base, ULONG *out)
{
    LONG signed_out = 0;
    NTSTATUS st = wcstol(text, end, base, &signed_out);
    *out = (ULONG)signed_out;
    return st;
}

WCHAR *_ultow_s(ULONG value, WCHAR *dst, SIZE_T cap, LONG base)
{
    WCHAR tmp[24];
    UINT n = 0;
    if (!base || base > 36)
        base = 10;
    do {
        ULONG d = value % (ULONG)base;
        tmp[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= (ULONG)base;
    } while (value);
    if (!cap || n + 1 > cap)
        return 0;
    for (UINT i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
    return dst;
}

int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int iswspace(WCHAR c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}

void qsort(void *base, SIZE_T count, SIZE_T width,
           int (*compare)(const void *, const void *))
{
    BYTE *b = (BYTE *)base;
    for (SIZE_T i = 1; i < count; i++) {
        BYTE key[64];
        if (width > sizeof(key))
            return; /* refuse oversized elements */
        memcpy(key, b + i * width, width);
        SIZE_T j = i;
        while (j && compare(b + (j - 1) * width, key) > 0) {
            memcpy(b + j * width, b + (j - 1) * width, width);
            j--;
        }
        memcpy(b + j * width, key, width);
    }
}

/* ------------------------------------------------------------------ */
/* Error state, misc queries                                           */
/* ------------------------------------------------------------------ */

void RtlSetLastWin32Error(DWORD error)
{
    write_teb_dword(0x68, error);
}

DWORD RtlGetLastWin32Error(void)
{
    return read_teb_dword(0x68);
}

DWORD RtlNtStatusToDosError(NTSTATUS status)
{
    switch ((ULONG)status) {
    case 0xC0000034: return 2;   /* OBJECT_NAME_NOT_FOUND */
    case 0xC0000008: return 6;   /* INVALID_HANDLE */
    case 0xC0000022: return 5;   /* ACCESS_DENIED */
    case 0xC0000017: return 8;   /* NO_MEMORY */
    case 0xC000000D: return 87;  /* INVALID_PARAMETER */
    case 0xC0000225: return 1168; /* NOT_FOUND */
    case 0:          return 0;
    default:         return 1;   /* INVALID_FUNCTION */
    }
}

DWORD RtlNtStatusToDosErrorNoTeb(NTSTATUS status)
{
    return RtlNtStatusToDosError(status);
}

BOOLEAN RtlIsThreadWithinLoaderCallout(void)
{
    return FALSE;
}

void *RtlPcToFileHeader(void *pc, void **base)
{
    if (base)
        *base = 0;
    return 0;
}

ULONGLONG RtlGetActiveConsoleId(void)
{
    return 0;
}

WORD RtlGetThreadLangIdByIndex(DWORD index)
{
    (void)index;
    return 0x0409;
}

/* Exported DATA symbol: real binaries dereference it as WORD *. */
WORD NlsAnsiCodePage = 1252;

NTSTATUS RtlOpenCurrentUser(DWORD access, HANDLE *key)
{
    (void)access;
    if (!key)
        return STATUS_INVALID_PARAMETER;
    *key = (HANDLE)0x80000001ULL; /* HKEY_CURRENT_USER */
    return STATUS_SUCCESS;
}

BOOLEAN RtlGetNtProductType(ULONG *product)
{
    if (product)
        *product = 1; /* WINNT */
    return TRUE;
}

NTSTATUS RtlQueryElevationFlags(ULONG *flags)
{
    if (flags)
        *flags = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlCheckRegistryKey(ULONG relative, const WCHAR *path)
{
    (void)relative; (void)path;
    return STATUS_NOT_FOUND;
}

ULONG RtlMapGenericMask(ULONG access, const void *mapping)
{
    /* The GENERIC mapping structure is {GenericRead, GenericWrite,
     * GenericExecute, GenericAll}; apply it literally. */
    const ULONG *m = (const ULONG *)mapping;
    ULONG result = 0;
    if (access & 0x80000000UL) result |= m[0];
    if (access & 0x40000000UL) result |= m[1];
    if (access & 0x20000000UL) result |= m[2];
    if (access & 0x10000000UL) result |= m[3];
    return result;
}

void RtlRunEncodeUnicodeString(void *seed, UNICODE_STRING_NT *ustr)
{
    (void)seed;
    (void)ustr;
}

void RtlRunDecodeUnicodeString(void *seed, UNICODE_STRING_NT *ustr)
{
    (void)seed;
    (void)ustr;
}

ULONG RtlRandomEx(ULONG *seed)
{
    *seed = *seed * 1103515245 + 12345;
    return *seed;
}

BOOLEAN RtlDllShutdownInProgress(void)
{
    return FALSE;
}

NTSTATUS RtlGetLastNtStatus(void)
{
    return (NTSTATUS)read_teb_dword(0x50); /* TEB LastStatusValue */
}

/* ------------------------------------------------------------------ */
/* Activation contexts: report "not found" so callers take their       */
/* fallback paths instead of treating garbage as a valid context.      */
/* ------------------------------------------------------------------ */

/*
 * There is no side-by-side store in this system, so no name ever resolves to
 * a manifest section. The canonical answer real ntdll gives for that case is
 * STATUS_SXS_SECTION_NOT_FOUND: user32's class-name capture accepts exactly
 * this status (or a fully successful lookup) -- any other negative status is
 * treated as a broken activation context and fails RegisterClass silently.
 */
#define STATUS_SXS_SECTION_NOT_FOUND 0xC0150008

NTSTATUS RtlFindActivationContextSectionString(ULONG flags, const void *guid,
                                               ULONG section, const void *name,
                                               void *data)
{
    (void)flags; (void)guid; (void)section; (void)name; (void)data;
    return STATUS_SXS_SECTION_NOT_FOUND;
}

NTSTATUS RtlActivateActivationContextUnsafeFast(void *cookie, void *ctx)
{
    (void)cookie; (void)ctx;
    return STATUS_SUCCESS;
}

NTSTATUS RtlDeactivateActivationContextUnsafeFast(void *cookie, void *frame)
{
    (void)cookie; (void)frame;
    return STATUS_SUCCESS;
}

NTSTATUS RtlReleaseActivationContext(void *ctx)
{
    (void)ctx;
    return STATUS_SUCCESS;
}

NTSTATUS RtlQueryInformationActiveActivationContext(ULONG cls, void *buf,
                                                    ULONG size, ULONG *ret)
{
    (void)cls; (void)buf; (void)size;
    if (ret)
        *ret = 0;
    return STATUS_NOT_FOUND;
}

NTSTATUS LdrQueryImageFileExecutionOptions(const UNICODE_STRING_NT *image,
                                           PCWSTR name, ULONG type,
                                           void *data, ULONG size,
                                           ULONG *ret_size)
{
    (void)image; (void)name; (void)type; (void)data; (void)size;
    (void)ret_size;
    return STATUS_NOT_FOUND;
}

void LdrFlushAlternateResourceModules(void)
{
}

/* ------------------------------------------------------------------ */
/* CSR / ETW / WNF / telemetry no-ops                                  */
/* ------------------------------------------------------------------ */

void *CsrAllocateCaptureBuffer(ULONG count, ULONG size)
{
    return RtlAllocateHeap(0, 0, size ? size : 16);
}

void CsrFreeCaptureBuffer(void *buffer)
{
    RtlFreeHeap(0, 0, buffer);
}

NTSTATUS CsrAllocateMessagePointer(void *buffer, ULONG size, void **message)
{
    (void)buffer;
    *message = 0;
    return STATUS_SUCCESS;
}

NTSTATUS CsrClientCallServer(void *message, void *capture, ULONG code,
                             ULONG size)
{
    (void)message; (void)capture; (void)code; (void)size;
    return STATUS_NOT_FOUND;
}

ULONG EtwRegisterTraceGuidsW(void *a, void *b, void *c, ULONG d, void *e,
                             void *f, void *g, void *h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h;
    return 1;
}

ULONG EtwUnregisterTraceGuids(ULONG handle, void *ctx)
{
    (void)handle; (void)ctx;
    return 1;
}

ULONGLONG EtwEventRegister(const void *id, void *cb, void *ctx, void *reg)
{
    (void)id; (void)cb; (void)ctx;
    if (reg)
        *(ULONG *)reg = 1;
    return 1;
}

ULONG EtwEventUnregister(ULONGLONG handle)
{
    (void)handle;
    return 0;
}

ULONG EtwEventWrite(ULONGLONG handle, const void *desc, ULONG count,
                    void *data)
{
    (void)handle; (void)desc; (void)count; (void)data;
    return 0;
}

ULONG EtwEventWriteTransfer(ULONGLONG handle, const void *desc,
                            const void *activity, const void *related,
                            ULONG count, void *data)
{
    (void)handle; (void)desc; (void)activity; (void)related; (void)count;
    (void)data;
    return 0;
}

BOOLEAN EtwEventEnabled(ULONGLONG handle, const void *desc)
{
    (void)handle; (void)desc;
    return FALSE;
}

ULONG EtwEventSetInformation(ULONGLONG handle, ULONG cls, void *info,
                             ULONG size)
{
    (void)handle; (void)cls; (void)info; (void)size;
    return 0;
}

ULONG EtwGetTraceEnableLevel(ULONG handle)
{
    (void)handle;
    return 0;
}

ULONG EtwGetTraceEnableFlags(ULONG handle)
{
    (void)handle;
    return 0;
}

ULONG EtwGetTraceLoggerHandle(ULONG handle)
{
    (void)handle;
    return 1;
}

ULONG EtwLogTraceEvent(void *event)
{
    (void)event;
    return 0;
}

ULONG EtwTraceMessage(void *logger, ULONG flags, void *guid, USHORT number,
                      ...)
{
    (void)logger; (void)flags; (void)guid; (void)number;
    return 0;
}

NTSTATUS NtQueryWnfStateData(void *id, void *scope, void *context,
                             ULONG *stamp, void *buffer, ULONG *size)
{
    (void)id; (void)scope; (void)context; (void)stamp; (void)buffer;
    if (size)
        *size = 0;
    return STATUS_NOT_FOUND;
}

NTSTATUS RtlQueryWnfStateData(void *id, void *scope, void *context,
                              ULONG *stamp, void *buffer, ULONG *size)
{
    return NtQueryWnfStateData(id, scope, context, stamp, buffer, size);
}

NTSTATUS RtlPublishWnfStateData(void *id, void *scope, void *context)
{
    (void)id; (void)scope; (void)context;
    return STATUS_SUCCESS;
}

NTSTATUS RtlSubscribeWnfStateChangeNotification(void **subscription,
                                                void *id, ULONG state,
                                                void *callback, void *ctx,
                                                void *stamp, ULONG flags,
                                                void *blob)
{
    (void)id; (void)state; (void)callback; (void)ctx; (void)stamp;
    (void)flags; (void)blob;
    if (subscription)
        *subscription = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlUnsubscribeWnfNotificationWaitForCompletion(void *subscription)
{
    (void)subscription;
    return STATUS_SUCCESS;
}

ULONG WinSqmAddToStream(const void *event, ULONG size, ULONG flags)
{
    (void)event; (void)size; (void)flags;
    return 0;
}

ULONG WinSqmAddToStreamEx(const void *event, ULONG size, ULONG flags,
                          void *extra)
{
    (void)event; (void)size; (void)flags; (void)extra;
    return 0;
}

ULONG WinSqmIncrementDWORD(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    return 0;
}

ULONG WinSqmSetDWORD(void *a, ULONG b, ULONG c)
{
    (void)a; (void)b; (void)c;
    return 0;
}

void ShipAssert(const char *file, ULONG line, ULONG code)
{
    (void)file; (void)line; (void)code;
}

/* ------------------------------------------------------------------ */
/* A minimal static environment block backing %VAR% expansion.         */
/* ------------------------------------------------------------------ */

static const struct { const WCHAR *name; const WCHAR *value; } g_env_vars[] = {
    { L"SystemDrive",   L"C:" },
    { L"SystemRoot",    L"C:\\Windows" },
    { L"windir",        L"C:\\Windows" },
    { L"TEMP",          L"C:\\Temp" },
    { L"TMP",           L"C:\\Temp" },
    { L"USERPROFILE",   L"C:\\Users\\User" },
    { L"HOMEDRIVE",     L"C:" },
    { L"HOMEPATH",      L"\\Users\\User" },
    { L"APPDATA",       L"C:\\Users\\User\\AppData\\Roaming" },
    { L"LOCALAPPDATA",  L"C:\\Users\\User\\AppData\\Local" },
    { L"ProgramFiles",  L"C:\\Program Files" },
    { L"ProgramData",   L"C:\\ProgramData" },
    { L"COMPUTERNAME",  L"NTOS" },
};

static const WCHAR *env_lookup(const WCHAR *name)
{
    for (UINT i = 0; i < sizeof(g_env_vars) / sizeof(g_env_vars[0]); i++) {
        const WCHAR *a = g_env_vars[i].name, *b = name;
        for (;;) {
            WCHAR ca = *a++, cb = *b++;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (!cb)
                return ca ? 0 : g_env_vars[i].value;
            if (ca != cb)
                break;
        }
    }
    return 0;
}

NTSTATUS RtlCreateEnvironment(ULONG flags, void **environment)
{
    (void)flags;
    if (environment)
        *environment = (void *)1;
    return STATUS_SUCCESS;
}

NTSTATUS RtlDestroyEnvironment(void *environment)
{
    (void)environment;
    return STATUS_SUCCESS;
}

NTSTATUS RtlQueryEnvironmentVariable_U(void *environment,
                                       UNICODE_STRING_NT *name,
                                       UNICODE_STRING_NT *value,
                                       ULONG *value_length)
{
    (void)environment;
    if (!name || !value)
        return STATUS_INVALID_PARAMETER;
    WCHAR stack[32];
    UINT chars = name->Length / 2;
    if (chars > 31)
        return STATUS_NOT_FOUND;
    for (UINT i = 0; i < chars; i++)
        stack[i] = name->Buffer[i];
    stack[chars] = 0;

    const WCHAR *found = env_lookup(stack);
    if (!found)
        return STATUS_NOT_FOUND;

    UINT len = 0;
    while (found[len])
        len++;
    if (value->MaximumLength < (len + 1) * sizeof(WCHAR)) {
        if (value_length)
            *value_length = (len + 1) * sizeof(WCHAR);
        return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    }
    memcpy(value->Buffer, found, (len + 1) * sizeof(WCHAR));
    value->Length = (WORD)(len * sizeof(WCHAR));
    if (value_length)
        *value_length = (len) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS RtlSetEnvironmentVariable(void **environment,
                                   UNICODE_STRING_NT *name,
                                   UNICODE_STRING_NT *value)
{
    (void)environment; (void)name; (void)value;
    return STATUS_SUCCESS;
}

NTSTATUS RtlSetCurrentEnvironment(void *environment, void **previous)
{
    (void)environment;
    if (previous)
        *previous = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RtlExpandEnvironmentStrings_U(void *environment,
                                       const UNICODE_STRING_NT *source,
                                       UNICODE_STRING_NT *destination,
                                       ULONG *destination_length)
{
    (void)environment;
    if (!source || !destination)
        return STATUS_INVALID_PARAMETER;

    UINT n = source->Length / 2;
    UINT out = 0;
    BOOLEAN overflow = FALSE;
    for (UINT i = 0; i < n; i++) {
        if (source->Buffer[i] != '%') {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = source->Buffer[i];
            else
                overflow = TRUE;
            out++;
            continue;
        }
        UINT j = i + 1;
        while (j < n && source->Buffer[j] != '%')
            j++;
        if (j >= n) {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = '%';
            out++;
            continue;
        }
        WCHAR name[32];
        UINT name_len = j - i - 1;
        if (name_len > 31)
            name_len = 31;
        for (UINT k = 0; k < name_len; k++)
            name[k] = source->Buffer[i + 1 + k];
        name[name_len] = 0;
        const WCHAR *value = env_lookup(name);
        if (!value)
            value = L"";
        for (; *value; value++) {
            if (out + 1 < destination->MaximumLength / 2)
                destination->Buffer[out] = *value;
            else
                overflow = TRUE;
            out++;
        }
        i = j;
    }
    if (out + 1 < destination->MaximumLength / 2 && !overflow) {
        destination->Buffer[out] = 0;
        destination->Length = (WORD)(out * sizeof(WCHAR));
        if (destination_length)
            *destination_length = (out + 1) * sizeof(WCHAR);
        return STATUS_SUCCESS;
    }
    if (destination_length)
        *destination_length = (out + 1) * sizeof(WCHAR);
    return 0x80000005L; /* STATUS_BUFFER_OVERFLOW */
}

/* ------------------------------------------------------------------ */
/* VDM backchannel: user32's UserRegisterWowHandlers gates its work on */
/* NtVdmControl(VDM_CHECK = 14) reporting "allowed" through a flag.     */
/* ------------------------------------------------------------------ */

NTSTATUS NtVdmControl(ULONG command, void *output)
{
    if (command == 14 && output) {
        ((BYTE *)output)[0x10] = 1;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* ------------------------------------------------------------------ */
/* Win32k normally invokes user32!UserRegisterWowHandlers during USER  */
/* initialization, handing back the handler table whose sixth member   */
/* must be a user32-internal routine that RegisterClassW validates.    */
/* With no win32k we perform the same registration ourselves right     */
/* after the loader finishes DLL process attach.                       */
/* ------------------------------------------------------------------ */

static void wow_handler_stub(void)
{
}

typedef struct _LDR_PEB_WALK {
    BYTE Reserved[0x10];
    void *ImageBaseAddress;      /* 0x10 */
    void *Ldr;                   /* 0x18 */
} PEB_WALK;

typedef struct _PEB_LDR_WALK {
    BYTE Reserved[0x10];
    struct { void *Flink, *Blink; } InLoadOrderModuleList; /* 0x10 */
} PEB_LDR_WALK;

static int wide_name_is(const WCHAR *name, UINT length_chars,
                        const char *ascii)
{
    UINT i = 0;
    for (; i < length_chars && ascii[i]; i++) {
        WCHAR c = name[i];
        char a = ascii[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (c != (WCHAR)(BYTE)a)
            return FALSE;
    }
    return i == length_chars && ascii[i] == 0;
}

void LdrpRegisterWowHandlers(void);
__attribute__((noinline)) void LdrpRegisterWowHandlers(void)
{
    ULONGLONG peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    if (!peb)
        return;
    PEB_LDR_WALK *ldr = *(PEB_LDR_WALK **)(peb + 0x18);
    if (!ldr)
        return;

    for (void *link = ldr->InLoadOrderModuleList.Flink;
         link && link != &ldr->InLoadOrderModuleList.Flink;
         link = *(void **)link) {
        BYTE *entry = (BYTE *)link;
        BYTE *dll_base = *(BYTE **)(entry + 0x30);
        WCHAR *name = *(WCHAR **)(entry + 0x60);
        WORD name_len = *(WORD *)(entry + 0x58);
        if (!dll_base || !name ||
            !wide_name_is(name, name_len / 2, "user32.dll"))
            continue;

        /* Walk user32's export table for UserRegisterWowHandlers. */
        DWORD pe_offset = *(DWORD *)(dll_base + 0x3C);
        DWORD *dd = (DWORD *)(dll_base + pe_offset + 24 + 112);
        DWORD exp_rva = dd[0], exp_size = dd[1]; /* directory entry 0 */
        if (!exp_rva || !exp_size)
            return;
        BYTE *dir = dll_base + exp_rva;
        UINT32 number_of_names = *(UINT32 *)(dir + 24);
        UINT32 names_rva = *(UINT32 *)(dir + 32);
        UINT32 funcs_rva = *(UINT32 *)(dir + 28);
        UINT16 *ordinals = (UINT16 *)(dll_base + *(UINT32 *)(dir + 36));
        UINT32 *functions = (UINT32 *)(dll_base + funcs_rva);

        for (UINT32 i = 0; i < number_of_names; i++) {
            const char *export_name =
                (const char *)(dll_base + *(UINT32 *)(dll_base +
                                                     names_rva + i * 4));
            static const char want[] = "UserRegisterWowHandlers";
            UINT j;
            for (j = 0; export_name[j] == want[j] && want[j]; j++)
                ;
            if (want[j])
                continue;

            void (*register_handlers)(const void *, BYTE *) =
                (void (*)(const void *, BYTE *))
                    (dll_base + functions[ordinals[i]]);
            void *handlers[8];
            for (UINT k = 0; k < 8; k++)
                handlers[k] = (void *)wow_handler_stub;
            /* RegisterClassW validates that this member points at
             * user32's own fnid-resolution routine (RVA 0xD1A0 in the
             * supplied build). */
            handlers[6] = dll_base + 0xD1A0;
            BYTE allowed = 0;
            register_handlers(handlers, &allowed);
            NtDisplayString("[ntdll] WowHandlers registered\r\n");
            return;
        }
        return;
    }
}

/*
 * ------------------------------------------------------------------------
 * DefWindowProc
 * ------------------------------------------------------------------------
 * USER32 exports DefWindowProcW/A as forwarders to NTDLL.NtdllDefWindowProc_W
 * /_A, so the default window procedure genuinely lives here. Returning zero
 * for everything (the previous stub) is wrong in a way that breaks window
 * creation outright: WM_NCCREATE's documented default is TRUE, and win32k
 * aborts CreateWindowEx when the class procedure -- which forwards the
 * message here -- answers FALSE. The results below are the documented
 * defaults; messages with a zero default fall through the switch.
 */
typedef unsigned long long ULONG_PTR;
typedef long long          LONG_PTR;

#define WM_CREATE                 0x0001
#define WM_DESTROY                0x0002
#define WM_SETREDRAW              0x000B
#define WM_SETTEXT                0x000C
#define WM_GETTEXT                0x000D
#define WM_GETTEXTLENGTH          0x000E
#define WM_ERASEBKGND             0x0014
#define WM_QUERYENDSESSION        0x0011
#define WM_QUERYOPEN              0x0013
#define WM_SHOWWINDOW             0x0018
#define WM_SETCURSOR              0x0020
#define WM_MOUSEACTIVATE          0x0021
#define WM_NCCREATE               0x0081
#define WM_NCDESTROY              0x0082
#define WM_NCCALCSIZE             0x0083
#define WM_NCHITTEST              0x0084
#define WM_NCACTIVATE             0x0086
#define WM_GETDLGCODE             0x0087
#define WM_NOTIFYFORMAT           0x0055
#define WM_DEVICECHANGE           0x0219
#define WM_INPUTLANGCHANGEREQUEST 0x0050
#define WM_QUERYUISTATE           0x0129
#define WM_CTLCOLORMSGBOX         0x0132
#define WM_CTLCOLORSTATIC         0x0138

#define HTCLIENT      1
#define MA_ACTIVATE   1
#define NFR_UNICODE   2

LONG_PTR NtdllDefWindowProc_W(HANDLE hwnd, UINT msg,
                                                    ULONG_PTR wparam,
                                                    LONG_PTR lparam)
{
    (void)hwnd;
    (void)wparam;
    (void)lparam;

    switch (msg) {
    /* Creation: a FALSE here cancels the window. */
    case WM_NCCREATE:
        return TRUE;

    /* Queries whose "nobody handled it" answer is affirmative. */
    case WM_NCACTIVATE:
    case WM_QUERYENDSESSION:
    case WM_QUERYOPEN:
    case WM_SETCURSOR:
    case WM_SETTEXT:
    case WM_ERASEBKGND:
    case WM_DEVICECHANGE:
    case WM_INPUTLANGCHANGEREQUEST:
        return TRUE;

    case WM_NCHITTEST:
        /* No non-client frame is tracked yet: every hit is in the client
         * area, which is the answer for a borderless window anyway. */
        return HTCLIENT;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_NOTIFYFORMAT:
        /* Every window in this system is a Unicode window. */
        return NFR_UNICODE;

    /* The CTLCOLOR family answers with a brush; no GDI stock brushes exist
     * yet, so the honest answer stays NULL rather than a fake handle. */
    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLORSTATIC:
        return 0;

    /* Everything else -- WM_CREATE, WM_DESTROY, WM_NCDESTROY, WM_NCCALCSIZE,
     * WM_PAINT, WM_SHOWWINDOW, the WINDOWPOS pair, ... -- defaults to 0. */
    default:
        return 0;
    }
}

LONG_PTR NtdllDefWindowProc_A(HANDLE hwnd, UINT msg,
                                                    ULONG_PTR wparam,
                                                    LONG_PTR lparam)
{
    return NtdllDefWindowProc_W(hwnd, msg, wparam, lparam);
}
