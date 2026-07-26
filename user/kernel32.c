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

typedef unsigned short WCHAR;
typedef long           NTSTATUS;

/* NT parameter structures (Windows x64 layout). */
typedef struct _UNICODE_STRING_K { WCHAR Length, MaximumLength; WCHAR *Buffer; }
    UNICODE_STRING_K; /* note: Buffer sits at +8 by alignment (as on Windows) */

typedef struct _OBJECT_ATTRIBUTES {
    DWORD             Length;
    HANDLE            RootDirectory;
    UNICODE_STRING_K *ObjectName;
    DWORD             Attributes;
    void             *SecurityDescriptor;
    void             *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; void *Pointer; };
    ULONGLONG Information;
} IO_STATUS_BLOCK;

#define FILE_OPEN            1
#define MEM_COMMIT           0x1000
#define MEM_RESERVE          0x2000
#define PAGE_READWRITE       0x04
#define NT_INVALID_HANDLE    ((HANDLE)(ULONGLONG)-1)
#define NT_SUCCESS(s)        ((NTSTATUS)(s) >= 0)

/* Native services imported from ntdll, at their real Windows signatures. */
extern NTSTATUS NtCreateFile(HANDLE *FileHandle, DWORD DesiredAccess,
                             OBJECT_ATTRIBUTES *ObjectAttributes,
                             IO_STATUS_BLOCK *IoStatusBlock, void *AllocationSize,
                             DWORD FileAttributes, DWORD ShareAccess,
                             DWORD CreateDisposition, DWORD CreateOptions,
                             void *EaBuffer, DWORD EaLength);
extern NTSTATUS NtReadFile(HANDLE File, HANDLE Event, void *ApcRoutine,
                           void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                           void *Buffer, DWORD Length, void *ByteOffset,
                           void *Key);
extern NTSTATUS NtWriteFile(HANDLE File, HANDLE Event, void *ApcRoutine,
                            void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                            const void *Buffer, DWORD Length, void *ByteOffset,
                            void *Key);
extern NTSTATUS NtAllocateVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG ZeroBits, ULONGLONG *RegionSize,
                                        DWORD AllocationType, DWORD Protect);
extern long      NtClose(HANDLE h);
extern NTSTATUS  NtCreateThreadEx(HANDLE *ThreadHandle, DWORD DesiredAccess,
                                  void *ObjectAttributes, HANDLE ProcessHandle,
                                  void *StartRoutine, void *Argument,
                                  DWORD CreateFlags, ULONGLONG ZeroBits,
                                  ULONGLONG StackSize, ULONGLONG MaxStackSize,
                                  void *AttributeList);
extern NTSTATUS  NtWaitForSingleObject(HANDLE Handle, int Alertable,
                                       long long *Timeout);
extern void      NtTerminateThread(void);
extern HANDLE    NtLoadLibrary(const char *name);
extern NTSTATUS  NtDelayExecution(int Alertable, long long *Interval);

void *memset(void *dst, int v, SIZE_T n); /* defined below */

/* Allocate `bytes` of committed user memory (returns 0 on failure). */
static void *nt_alloc(ULONGLONG bytes)
{
    void *base = 0;
    ULONGLONG size = bytes;
    NTSTATUS st = NtAllocateVirtualMemory(NT_INVALID_HANDLE, &base, 0, &size,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return NT_SUCCESS(st) ? base : 0;
}

/* Open a file or namespace object by ASCII name via the real NtCreateFile. */
static HANDLE nt_open(const char *name)
{
    WCHAR wname[260];
    int n = 0;
    for (; name[n] && n < 259; n++)
        wname[n] = (WCHAR)(unsigned char)name[n];
    wname[n] = 0;

    UNICODE_STRING_K us = { (WCHAR)(n * 2), (WCHAR)(n * 2 + 2), wname };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;

    HANDLE h = 0;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, 0, &oa, &iosb, 0, 0, 0, FILE_OPEN, 0, 0, 0);
    return NT_SUCCESS(st) ? h : NT_INVALID_HANDLE;
}

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

__declspec(dllexport) HANDLE LoadLibraryA(const char *name)
{
    /* Already loaded? Return the existing module (Windows semantics). */
    HANDLE existing = GetModuleHandleA(name);
    if (existing)
        return existing;
    /* Otherwise ask the kernel loader to map it and link it into PEB->Ldr. */
    return NtLoadLibrary(name);
}

__declspec(dllexport) BOOL FreeLibrary(HANDLE module)
{
    (void)module; /* unloading is a no-op for now (no per-module refcounts) */
    return 1;
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
    /* Cached, like the real GetStdHandle: repeated calls (e.g. from every
     * printf) return the same handle instead of reopening the console. */
    static HANDLE g_std_console;
    if (!g_std_console)
        g_std_console = nt_open("\\Device\\Console");
    return g_std_console;
}

__declspec(dllexport) BOOL WriteFile(HANDLE h, const void *buffer, DWORD len,
                                     DWORD *written, LPVOID overlapped)
{
    (void)overlapped;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtWriteFile(h, 0, 0, 0, &iosb, buffer, len, 0, 0);
    if (written)
        *written = (DWORD)iosb.Information;
    return NT_SUCCESS(st);
}

__declspec(dllexport) BOOL ReadFile(HANDLE h, void *buffer, DWORD len,
                                    DWORD *read, LPVOID overlapped)
{
    (void)overlapped;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtReadFile(h, 0, 0, 0, &iosb, buffer, len, 0, 0);
    if (read)
        *read = (DWORD)iosb.Information;
    return NT_SUCCESS(st);
}

__declspec(dllexport) HANDLE CreateFileA(const char *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)templ;
    return nt_open(name);
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
    THREAD_INFO *info = (THREAD_INFO *)nt_alloc(sizeof(THREAD_INFO));
    if (!info)
        return 0;
    info->Start = start;
    info->Param = param;

    HANDLE h = 0;
    NtCreateThreadEx(&h, 0, 0, NT_INVALID_HANDLE, (void *)BaseThreadStart, info,
                     0, 0, 0, 0, 0);
    return h;
}

__declspec(dllexport) DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms; /* timeouts not implemented; treat every wait as INFINITE */
    NtWaitForSingleObject(h, 0, 0);
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

    void *mem = nt_alloc(region);
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

/* ------------------------------------------------------------------ */
/* Time: KUSER_SHARED_DATA (the read-only page at 0x7FFE0000)          */
/* ------------------------------------------------------------------ */

#define KUSD ((volatile unsigned char *)(ULONGLONG)0x7FFE0000ULL)

/* Read a KSYSTEM_TIME (Low, High1, High2) lock-free from the shared page. */
static ULONGLONG read_ksystem_time(unsigned off)
{
    for (;;) {
        int high1 = *(volatile int *)(KUSD + off + 0x4);
        unsigned low = *(volatile unsigned *)(KUSD + off + 0x0);
        int high2 = *(volatile int *)(KUSD + off + 0x8);
        if (high1 == high2)
            return ((ULONGLONG)(unsigned)high1 << 32) | low;
    }
}

__declspec(dllexport) ULONGLONG GetTickCount64(void)
{
    ULONGLONG ticks = read_ksystem_time(0x320);          /* TickCount     */
    unsigned mult = *(volatile unsigned *)(KUSD + 0x004); /* Multiplier    */
    return (ticks * mult) >> 24;                          /* -> milliseconds */
}

__declspec(dllexport) DWORD GetTickCount(void)
{
    return (DWORD)GetTickCount64();
}

__declspec(dllexport) void GetSystemTimeAsFileTime(void *lpFileTime)
{
    ULONGLONG t = read_ksystem_time(0x014); /* SystemTime, 100 ns units */
    if (lpFileTime) {
        ((DWORD *)lpFileTime)[0] = (DWORD)t;
        ((DWORD *)lpFileTime)[1] = (DWORD)(t >> 32);
    }
}

__declspec(dllexport) void Sleep(DWORD ms)
{
    /* Relative delay: negative 100 ns units. */
    long long interval = -(long long)ms * 10000;
    NtDelayExecution(0, &interval);
}

/* ------------------------------------------------------------------ */
/* Command line (from PEB->ProcessParameters)                          */
/* ------------------------------------------------------------------ */

static char g_cmdline[260];

__declspec(dllexport) char *GetCommandLineA(void)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (pp) {
        /* CommandLine UNICODE_STRING at offset 0x70; Buffer at +8. */
        WORD len = *(WORD *)(pp + 0x70) / 2;
        WCHAR *buf = *(WCHAR **)(pp + 0x78);
        unsigned i = 0;
        for (; i < len && i < sizeof(g_cmdline) - 1 && buf[i]; i++)
            g_cmdline[i] = (char)buf[i];
        g_cmdline[i] = 0;
    }
    return g_cmdline;
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

__declspec(dllexport) int lstrlenA(const char *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

__declspec(dllexport) char *lstrcpyA(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

__declspec(dllexport) char *lstrcatA(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

/* Format an unsigned value into buf (base 10 or 16); returns the length. */
static int fmt_uint(char *buf, ULONGLONG v, int base, int is_signed,
                    long long sv)
{
    char tmp[24];
    int n = 0, len = 0;
    int neg = 0;
    if (is_signed) {
        if (sv < 0) {
            neg = 1;
            v = (ULONGLONG)(-sv);
        } else {
            v = (ULONGLONG)sv;
        }
    }
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        int d = (int)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    if (neg)
        buf[len++] = '-';
    while (n)
        buf[len++] = tmp[--n];
    return len;
}

/* A small wsprintfA: supports %d %u %x %s %c %% (no width/precision). */
__declspec(dllexport) int wsprintfA(char *out, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out[o++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'd': o += fmt_uint(out + o, 0, 10, 1, __builtin_va_arg(ap, int)); break;
        case 'u': o += fmt_uint(out + o, __builtin_va_arg(ap, unsigned), 10, 0, 0); break;
        case 'x': o += fmt_uint(out + o, __builtin_va_arg(ap, unsigned), 16, 0, 0); break;
        case 'c': out[o++] = (char)__builtin_va_arg(ap, int); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s)
                out[o++] = *s++;
            break;
        }
        case '%': out[o++] = '%'; break;
        default:  out[o++] = '%'; out[o++] = *p; break;
        }
    }
    out[o] = 0;
    __builtin_va_end(ap);
    return o;
}
