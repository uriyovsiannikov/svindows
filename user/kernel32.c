/*
 * user/kernel32.c - a minimal Win32 subsystem library (kernel32.dll).
 *
 * Implements a handful of the classic Win32 API on top of the native NTOS
 * system calls exported by ntdll. Built for the x86-64 Windows target so a
 * normal Win32 program can link against it, exactly as it would on Windows.
 * The chain at runtime is: app.exe -> kernel32.dll -> ntdll.dll -> syscall.
 */

typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef unsigned long long SIZE_T;
typedef int                BOOL;
typedef void             (*FARPROC)(void);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0     0
#define HEAP_ZERO_MEMORY  0x00000008

/* Native services imported from ntdll. */
extern HANDLE    NtCreateFile(const char *name);
extern ULONGLONG NtWriteFile(HANDLE h, const void *buf, ULONGLONG len);
extern ULONGLONG NtReadFile(HANDLE h, void *buf, ULONGLONG len);
extern long      NtClose(HANDLE h);
extern HANDLE    NtCreateThread(LPVOID entry, LPVOID arg);
extern long      NtWaitForSingleObject(HANDLE h);
extern void      NtTerminateThread(void);
extern ULONGLONG NtAllocateVirtualMemory(ULONGLONG size);

/* ------------------------------------------------------------------ */
/* Freestanding helpers (no CRT)                                      */
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

static int ascii_ieq(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

static int str_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (*a != *b)
            return 0;
    return *a == *b;
}

/* ------------------------------------------------------------------ */
/* PEB / loader module list (walked to find loaded modules)           */
/* ------------------------------------------------------------------ */

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct _UNICODE_STRING { WORD Length, MaximumLength; WORD *Buffer; }
    UNICODE_STRING;

/* Field offsets match the Windows x64 layout the kernel loader writes. */
typedef struct _LDR_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;           /* 0x00 */
    LIST_ENTRY     InMemoryOrderLinks;         /* 0x10 */
    LIST_ENTRY     InInitializationOrderLinks; /* 0x20 */
    void          *DllBase;                    /* 0x30 */
    void          *EntryPoint;                 /* 0x38 */
    DWORD          SizeOfImage;                /* 0x40 */
    UNICODE_STRING FullDllName;                /* 0x48 */
    UNICODE_STRING BaseDllName;                /* 0x58 */
} LDR_ENTRY;

typedef struct _PEB_LDR_DATA {
    DWORD      Length;                        /* 0x00 */
    BYTE       Initialized;                   /* 0x04 */
    void      *SsHandle;                      /* 0x08 */
    LIST_ENTRY InLoadOrderModuleList;         /* 0x10 */
    LIST_ENTRY InMemoryOrderModuleList;       /* 0x20 */
    LIST_ENTRY InInitializationOrderModuleList;/* 0x30 */
} PEB_LDR_DATA;

typedef struct _PEB {
    BYTE          Reserved[0x10];  /* 0x00 */
    void         *ImageBaseAddress;/* 0x10 */
    PEB_LDR_DATA *Ldr;             /* 0x18 */
    void         *ProcessParameters;/* 0x20 */
    void         *SubSystemData;   /* 0x28 */
    void         *ProcessHeap;     /* 0x30 */
} PEB;

static PEB *NtCurrentPeb(void)
{
    PEB *peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
}

/* Case-insensitively compare a wide UNICODE_STRING to an ASCII C string. */
static int ustr_eq_ascii(const UNICODE_STRING *u, const char *ascii)
{
    unsigned n = u->Length / 2;
    unsigned i = 0;
    for (; i < n && ascii[i]; i++)
        if (!ascii_ieq((char)u->Buffer[i], ascii[i]))
            return 0;
    return i == n && ascii[i] == 0;
}

__declspec(dllexport) HANDLE GetModuleHandleA(const char *name)
{
    PEB *peb = NtCurrentPeb();
    if (!name)
        return (HANDLE)peb->ImageBaseAddress;
    PEB_LDR_DATA *ldr = peb->Ldr;
    if (!ldr)
        return 0;

    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *p = head->Flink; p != head; p = p->Flink) {
        LDR_ENTRY *e = (LDR_ENTRY *)p; /* InLoadOrderLinks is at offset 0 */
        if (ustr_eq_ascii(&e->BaseDllName, name))
            return (HANDLE)e->DllBase;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* GetProcAddress: parse a module's PE export directory               */
/* ------------------------------------------------------------------ */

typedef struct _IMAGE_EXPORT_DIRECTORY {
    DWORD Characteristics, TimeDateStamp;
    WORD  MajorVersion, MinorVersion;
    DWORD Name, Base, NumberOfFunctions, NumberOfNames;
    DWORD AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY;

static IMAGE_EXPORT_DIRECTORY *pe_export_dir(const char *base)
{
    DWORD lfanew = *(const DWORD *)(base + 0x3C);
    const char *nt = base + lfanew;
    if (*(const DWORD *)nt != 0x00004550) /* "PE\0\0" */
        return 0;
    const char *opt = nt + 0x18; /* signature(4) + file header(20) */
    DWORD rva = *(const DWORD *)(opt + 0x70); /* DataDirectory[0] (export) */
    return rva ? (IMAGE_EXPORT_DIRECTORY *)(base + rva) : 0;
}

__declspec(dllexport) FARPROC GetProcAddress(HANDLE module, const char *name)
{
    const char *base = (const char *)module;
    if (!base)
        return 0;
    IMAGE_EXPORT_DIRECTORY *ed = pe_export_dir(base);
    if (!ed)
        return 0;

    DWORD *funcs = (DWORD *)(base + ed->AddressOfFunctions);

    /* An "ordinal" argument has a zero high half (MAKEINTRESOURCE). */
    if (((ULONGLONG)(void *)name >> 16) == 0) {
        DWORD idx = (DWORD)(ULONGLONG)(void *)name - ed->Base;
        if (idx >= ed->NumberOfFunctions)
            return 0;
        return (FARPROC)(void *)(base + funcs[idx]);
    }

    DWORD *names = (DWORD *)(base + ed->AddressOfNames);
    WORD  *ords  = (WORD  *)(base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        if (str_eq(base + names[i], name))
            return (FARPROC)(void *)(base + funcs[ords[i]]);
    }
    return 0;
}

__declspec(dllexport) HANDLE GetStdHandle(DWORD which)
{
    (void)which; /* every standard handle maps to the console for now */
    return NtCreateFile("\\Device\\Console");
}

__declspec(dllexport) BOOL WriteFile(HANDLE h, const void *buffer, DWORD len,
                                     DWORD *written, LPVOID overlapped)
{
    (void)overlapped;
    ULONGLONG n = NtWriteFile(h, buffer, len);
    if (written)
        *written = (DWORD)n;
    return 1;
}

__declspec(dllexport) BOOL ReadFile(HANDLE h, void *buffer, DWORD len,
                                    DWORD *read, LPVOID overlapped)
{
    (void)overlapped;
    ULONGLONG n = NtReadFile(h, buffer, len);
    if (read)
        *read = (DWORD)n;
    return 1;
}

__declspec(dllexport) HANDLE CreateFileA(const char *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)templ;
    return NtCreateFile(name);
}

__declspec(dllexport) BOOL CloseHandle(HANDLE h)
{
    NtClose(h);
    return 1;
}

__declspec(dllexport) void ExitThread(DWORD code)
{
    (void)code;
    NtTerminateThread();
}

__declspec(dllexport) void ExitProcess(DWORD code)
{
    (void)code;
    NtTerminateThread(); /* simplified: ends the calling (main) thread */
}

/* Per-thread hand-off block, so the thread entry can run the user routine and
 * then terminate when it returns (like BaseThreadInitThunk on Windows). */
typedef struct {
    LPTHREAD_START_ROUTINE Start;
    LPVOID                 Param;
} THREAD_INFO;

static DWORD BaseThreadStart(THREAD_INFO *info)
{
    DWORD code = info->Start(info->Param);
    ExitThread(code); /* does not return */
    return code;
}

__declspec(dllexport) HANDLE CreateThread(LPVOID sa, ULONGLONG stack_size,
                                          LPTHREAD_START_ROUTINE start,
                                          LPVOID param, DWORD flags, DWORD *tid)
{
    (void)sa; (void)stack_size; (void)flags; (void)tid;
    THREAD_INFO *info = (THREAD_INFO *)(LPVOID)(ULONGLONG)
        NtAllocateVirtualMemory(sizeof(THREAD_INFO));
    if (!info)
        return 0;
    info->Start = start;
    info->Param = param;
    return NtCreateThread((LPVOID)BaseThreadStart, info);
}

__declspec(dllexport) DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms; /* timeouts not implemented; treat every wait as INFINITE */
    NtWaitForSingleObject(h);
    return WAIT_OBJECT_0;
}

/* ------------------------------------------------------------------ */
/* Process heap: a first-fit free list over NtAllocateVirtualMemory   */
/* ------------------------------------------------------------------ */

/* Every allocation is preceded by one of these headers; blocks form a single
 * address-ordered list spanning the heap region, which makes coalescing easy. */
typedef struct _BLOCK {
    ULONGLONG      size; /* payload bytes */
    struct _BLOCK *next; /* next block by address (NULL at the end)  */
    int            free;
    int            pad;
} BLOCK;

typedef struct _HEAP {
    BLOCK *first;
} HEAP;

static HEAP *g_process_heap;

static ULONGLONG align_up8(ULONGLONG n) { return (n + 7) & ~7ULL; }

__declspec(dllexport) HANDLE HeapCreate(DWORD flags, SIZE_T initial, SIZE_T max)
{
    (void)flags; (void)max;
    ULONGLONG region = initial ? align_up8(initial) : 0x10000; /* >=64 KiB */
    region += sizeof(HEAP) + sizeof(BLOCK);

    void *mem = (void *)(ULONGLONG)NtAllocateVirtualMemory(region);
    if (!mem)
        return 0;

    HEAP *h = (HEAP *)mem;
    BLOCK *b = (BLOCK *)(h + 1);
    b->size = region - sizeof(HEAP) - sizeof(BLOCK);
    b->next = 0;
    b->free = 1;
    h->first = b;
    return (HANDLE)h;
}

__declspec(dllexport) HANDLE GetProcessHeap(void)
{
    if (!g_process_heap) {
        g_process_heap = (HEAP *)HeapCreate(0, 0x10000, 0);
        NtCurrentPeb()->ProcessHeap = g_process_heap;
    }
    return (HANDLE)g_process_heap;
}

__declspec(dllexport) LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    HEAP *h = (HEAP *)heap;
    if (!h)
        return 0;
    ULONGLONG size = align_up8(bytes ? bytes : 1);

    for (BLOCK *b = h->first; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* Split off the remainder if there's room for another block. */
        if (b->size >= size + sizeof(BLOCK) + 16) {
            BLOCK *nb = (BLOCK *)((char *)(b + 1) + size);
            nb->size = b->size - size - sizeof(BLOCK);
            nb->next = b->next;
            nb->free = 1;
            b->next = nb;
            b->size = size;
        }
        b->free = 0;

        void *p = (void *)(b + 1);
        if (flags & HEAP_ZERO_MEMORY)
            memset(p, 0, size);
        return p;
    }
    return 0; /* heap exhausted; no growth yet */
}

__declspec(dllexport) BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID ptr)
{
    (void)flags;
    HEAP *h = (HEAP *)heap;
    if (!h || !ptr)
        return 1;

    BLOCK *b = (BLOCK *)ptr - 1;
    b->free = 1;

    /* Coalesce adjacent free blocks in one forward pass. */
    for (BLOCK *c = h->first; c && c->next; ) {
        if (c->free && c->next->free) {
            c->size += sizeof(BLOCK) + c->next->size;
            c->next = c->next->next;
        } else {
            c = c->next;
        }
    }
    return 1;
}
