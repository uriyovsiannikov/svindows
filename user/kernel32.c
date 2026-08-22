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

__declspec(dllexport) void SetLastError(DWORD error);

/* Pointer encoding is a process-local hardening primitive on Windows. Until a
 * per-process secret is exposed by the kernel, identity encoding preserves the
 * required Encode/Decode round trip and, critically, keeps UCRT callback
 * pointers callable instead of turning missing-import zeroes into garbage. */
__declspec(dllexport) void *EncodePointer(void *pointer)
{
    return pointer;
}

__declspec(dllexport) void *DecodePointer(void *pointer)
{
    return pointer;
}

__declspec(dllexport) BOOL Beep(DWORD frequency, DWORD duration)
{
    (void)frequency;
    (void)duration;
    return 1;
}

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0     0
#define HEAP_ZERO_MEMORY  0x00000008
#define LANG_ENGLISH_US   0x0409

typedef unsigned short WCHAR;
typedef long           NTSTATUS;
typedef struct _FILETIME { DWORD Low, High; } FILETIME;
typedef struct _SYSTEMTIME {
    WORD Year, Month, DayOfWeek, Day;
    WORD Hour, Minute, Second, Milliseconds;
} SYSTEMTIME;

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
extern NTSTATUS  NtWaitForMultipleObjects(DWORD Count, const HANDLE *Handles,
                                          DWORD WaitType, int Alertable,
                                          long long *Timeout);
extern NTSTATUS  NtCreateEvent(HANDLE *EventHandle, DWORD DesiredAccess,
                               void *ObjectAttributes, int EventType,
                               BOOL InitialState);
extern NTSTATUS  NtSetEvent(HANDLE EventHandle, long *PreviousState);
extern NTSTATUS  NtResetEvent(HANDLE EventHandle, long *PreviousState);
extern NTSTATUS  NtCreateSemaphore(HANDLE *SemaphoreHandle, DWORD DesiredAccess,
                                   void *ObjectAttributes, long InitialCount,
                                   long MaximumCount);
extern NTSTATUS  NtReleaseSemaphore(HANDLE SemaphoreHandle, long ReleaseCount,
                                    long *PreviousCount);
extern NTSTATUS  NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus);
extern HANDLE    NtLoadLibrary(const char *name);
extern NTSTATUS  NtDelayExecution(int Alertable, long long *Interval);
extern NTSTATUS  NtProtectVirtualMemory(HANDLE Process, void **BaseAddress,
                                        ULONGLONG *RegionSize, DWORD NewProtect,
                                        DWORD *OldProtect);
extern NTSTATUS  NtEnumerateRootFiles(DWORD Index, void *FindData);
extern NTSTATUS  NtQueryFileInfo(HANDLE File, void *Info);
extern NTSTATUS  NtSetFilePosition(HANDLE File, ULONGLONG Position,
                                    ULONGLONG *NewPosition);

/* GS-relative TEB access (the TEB is at GS in ring 3). */
static DWORD read_gs_dword(ULONGLONG off)
{
    DWORD v;
    __asm__ volatile("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static void write_gs_dword(ULONGLONG off, DWORD val)
{
    __asm__ volatile("movl %0, %%gs:(%1)" : : "r"(val), "r"(off));
}
static ULONGLONG read_gs_qword(ULONGLONG off)
{
    ULONGLONG v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}
static void write_gs_qword(ULONGLONG off, ULONGLONG val)
{
    __asm__ volatile("movq %0, %%gs:(%1)" : : "r"(val), "r"(off) : "memory");
}

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

static DWORD ntstatus_to_win32(NTSTATUS status)
{
    switch ((DWORD)status) {
    case 0xC0000034: return 2;  /* STATUS_OBJECT_NAME_NOT_FOUND */
    case 0xC0000008: return 6;  /* STATUS_INVALID_HANDLE */
    case 0xC0000022: return 5;  /* STATUS_ACCESS_DENIED */
    case 0xC0000017: return 8;  /* STATUS_NO_MEMORY */
    default:         return 1;  /* ERROR_INVALID_FUNCTION */
    }
}

static HANDLE nt_open_w(const WCHAR *name, DWORD access, DWORD share,
                        DWORD disposition, DWORD attributes)
{
    DWORD nt_disposition;
    switch (disposition) {
    case 1: nt_disposition = 2; break; /* CREATE_NEW -> FILE_CREATE */
    case 2: nt_disposition = 5; break; /* CREATE_ALWAYS -> FILE_OVERWRITE_IF */
    case 3: nt_disposition = 1; break; /* OPEN_EXISTING -> FILE_OPEN */
    case 4: nt_disposition = 3; break; /* OPEN_ALWAYS -> FILE_OPEN_IF */
    case 5: nt_disposition = 4; break; /* TRUNCATE_EXISTING -> FILE_OVERWRITE */
    default:
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NT_INVALID_HANDLE;
    }
    if (!name) {
        SetLastError(87);
        return NT_INVALID_HANDLE;
    }

    DWORD chars = 0;
    while (name[chars] && chars < 32767) chars++;
    if (name[chars]) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NT_INVALID_HANDLE;
    }

    UNICODE_STRING_K us = {
        (WCHAR)(chars * sizeof(WCHAR)),
        (WCHAR)((chars + 1) * sizeof(WCHAR)),
        (WCHAR *)name
    };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;

    HANDLE handle = NT_INVALID_HANDLE;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtCreateFile(&handle, access, &oa, &iosb, 0,
                                   attributes & 0xFFFF, share,
                                   nt_disposition, 0, 0, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return NT_INVALID_HANDLE;
    }
    return handle;
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

__declspec(dllexport) HANDLE GetModuleHandleW(const WCHAR *name)
{
    if (!name)
        return GetModuleHandleA(0);
    char ascii[64];
    unsigned i = 0;
    for (; name[i] && i < sizeof(ascii) - 1; i++)
        ascii[i] = (char)name[i];
    ascii[i] = 0;
    return GetModuleHandleA(ascii);
}

__declspec(dllexport) BOOL GetModuleHandleExW(DWORD flags,
                                              const WCHAR *name_or_address,
                                              HANDLE *module)
{
    const DWORD FROM_ADDRESS = 0x00000004u;
    if (!module || (flags & ~0x00000007u) || !name_or_address) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    *module = 0;

    if (!(flags & FROM_ADDRESS)) {
        *module = GetModuleHandleW(name_or_address);
    } else {
        ULONGLONG address = (ULONGLONG)name_or_address;
        PEB *peb = NtCurrentPeb();
        PEB_LDR_DATA *ldr = peb ? peb->Ldr : 0;
        if (ldr) {
            LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
            for (LIST_ENTRY *p = head->Flink; p != head; p = p->Flink) {
                LDR_ENTRY *entry = (LDR_ENTRY *)p;
                ULONGLONG base = (ULONGLONG)entry->DllBase;
                if (address >= base && address < base + entry->SizeOfImage) {
                    *module = entry->DllBase;
                    break;
                }
            }
        }
    }

    if (!*module) {
        SetLastError(126); /* ERROR_MOD_NOT_FOUND */
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) HANDLE LoadLibraryA(const char *name)
{
    /* Already loaded? Return the existing module (Windows semantics). */
    HANDLE existing = GetModuleHandleA(name);
    if (existing)
        return existing;
    /* Otherwise ask the kernel loader to map it and link it into PEB->Ldr. */
    HANDLE module = NtLoadLibrary(name);
    if (!module)
        return 0;

    /* Runtime loads do not pass through the initial ntdll bootstrap block, so
     * perform DLL_PROCESS_ATTACH here after relocation/import binding. This is
     * required by control-panel and shell extension DLLs before any exported
     * entry point is called. */
    BYTE *image = (BYTE *)module;
    DWORD pe_offset = *(DWORD *)(image + 0x3c);
    if (*(DWORD *)(image + pe_offset) == 0x00004550) {
        BYTE *optional = image + pe_offset + 24;
        DWORD entry_rva = *(DWORD *)(optional + 16);
        if (entry_rva) {
            BOOL (*entry)(HANDLE, DWORD, void *) =
                (BOOL (*)(HANDLE, DWORD, void *))(image + entry_rva);
            if (!entry(module, 1 /* DLL_PROCESS_ATTACH */, 0)) {
                SetLastError(1114); /* ERROR_DLL_INIT_FAILED */
                return 0;
            }
        }
    }
    return module;
}

__declspec(dllexport) HANDLE LoadLibraryW(const WCHAR *name)
{
    if (!name)
        return 0;
    char ascii[96];
    unsigned i = 0;
    for (; name[i] && i < sizeof(ascii) - 1; i++)
        ascii[i] = (char)name[i];
    ascii[i] = 0;
    return LoadLibraryA(ascii);
}

__declspec(dllexport) HANDLE LoadLibraryExW(const WCHAR *name, HANDLE file,
                                             DWORD flags)
{
    (void)file;
    (void)flags;
    return LoadLibraryW(name);
}

__declspec(dllexport) HANDLE LoadLibraryExA(const char *name, HANDLE file,
                                             DWORD flags)
{
    (void)file;
    (void)flags;
    return LoadLibraryA(name);
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

__declspec(dllexport) BOOL GetConsoleMode(HANDLE console, DWORD *mode)
{
    if (!console || console == NT_INVALID_HANDLE || !mode) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    *mode = 0x0001 | 0x0002; /* ENABLE_PROCESSED_OUTPUT | WRAP_AT_EOL */
    return 1;
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

__declspec(dllexport) BOOL WriteConsoleW(HANDLE console, const WCHAR *text,
                                         DWORD char_count, DWORD *written,
                                         LPVOID reserved)
{
    char bytes[256];
    DWORD done = 0;
    (void)reserved;

    if (!text) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    while (done < char_count) {
        DWORD count = char_count - done;
        DWORD byte_count;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        for (byte_count = 0; byte_count < count; byte_count++) {
            WCHAR c = text[done + byte_count];
            bytes[byte_count] = c <= 0x7f ? (char)c : '?';
        }
        if (!WriteFile(console, bytes, byte_count, 0, 0)) {
            if (written) *written = done;
            return 0;
        }
        done += count;
    }
    if (written) *written = done;
    return 1;
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

typedef struct _NTOS_FILE_INFO {
    ULONGLONG Size;
    ULONGLONG Position;
    DWORD Attributes;
    DWORD IsConsole;
    WORD FatWriteDate;
    WORD FatWriteTime;
    DWORD Reserved;
} NTOS_FILE_INFO;

static BOOL leap_year(DWORD year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static ULONGLONG fat_datetime_ticks(WORD date, WORD time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    DWORD year = 1980 + (date >> 9);
    DWORD month = (date >> 5) & 15;
    DWORD day = date & 31;
    if (!date || month < 1 || month > 12 || day < 1)
        return 0;
    ULONGLONG days = 0;
    for (DWORD y = 1601; y < year; y++)
        days += leap_year(y) ? 366 : 365;
    for (DWORD m = 1; m < month; m++) {
        days += month_days[m - 1];
        if (m == 2 && leap_year(year)) days++;
    }
    days += day - 1;
    ULONGLONG seconds = days * 86400ULL;
    seconds += ((time >> 11) & 31) * 3600ULL;
    seconds += ((time >> 5) & 63) * 60ULL;
    seconds += (time & 31) * 2ULL;
    return seconds * 10000000ULL;
}

static FILETIME ticks_to_filetime(ULONGLONG ticks)
{
    FILETIME ft;
    ft.Low = (DWORD)ticks;
    ft.High = (DWORD)(ticks >> 32);
    return ft;
}

__declspec(dllexport) DWORD GetFileSize(HANDLE file, DWORD *high)
{
    NTOS_FILE_INFO info;
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    if (high) *high = (DWORD)(info.Size >> 32);
    return (DWORD)info.Size;
}

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    dwVolumeSerialNumber;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    nNumberOfLinks;
    DWORD    nFileIndexHigh;
    DWORD    nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;

__declspec(dllexport) BOOL GetFileInformationByHandle(
    HANDLE file, BY_HANDLE_FILE_INFORMATION *out)
{
    NTOS_FILE_INFO info;
    if (!out) {
        SetLastError(87);
        return 0;
    }
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = info.Attributes;
    out->nFileSizeHigh = (DWORD)(info.Size >> 32);
    out->nFileSizeLow = (DWORD)info.Size;
    out->nNumberOfLinks = 1;
    out->ftLastWriteTime = ticks_to_filetime(
        fat_datetime_ticks(info.FatWriteDate, info.FatWriteTime));
    return 1;
}

__declspec(dllexport) BOOL SetFilePointerEx(HANDLE file, long long distance,
                                             long long *new_position,
                                             DWORD move_method)
{
    NTOS_FILE_INFO info;
    NTSTATUS status = NtQueryFileInfo(file, &info);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }

    ULONGLONG base;
    switch (move_method) {
    case 0: base = 0; break;             /* FILE_BEGIN */
    case 1: base = info.Position; break; /* FILE_CURRENT */
    case 2: base = info.Size; break;     /* FILE_END */
    default:
        SetLastError(87);
        return 0;
    }
    if (distance < 0 && (ULONGLONG)(-(distance + 1)) + 1 > base) {
        SetLastError(131); /* ERROR_NEGATIVE_SEEK */
        return 0;
    }
    ULONGLONG target = distance < 0
        ? base - ((ULONGLONG)(-(distance + 1)) + 1)
        : base + (ULONGLONG)distance;
    ULONGLONG actual;
    status = NtSetFilePosition(file, target, &actual);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    if (new_position) *new_position = (long long)actual;
    return 1;
}

__declspec(dllexport) HANDLE CreateFileA(const char *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)templ;
    return nt_open(name);
}

__declspec(dllexport) HANDLE CreateFileW(const WCHAR *name, DWORD access,
                                         DWORD share, LPVOID sa, DWORD disp,
                                         DWORD flags, HANDLE templ)
{
    (void)sa; (void)templ;
    return nt_open_w(name, access, share, disp, flags);
}

__declspec(dllexport) BOOL CloseHandle(HANDLE h)
{
    NtClose(h);
    return 1;
}

__declspec(dllexport) void ExitThread(DWORD code)
{
    NtTerminateThread(0, (NTSTATUS)code);
}

__declspec(dllexport) void FreeLibraryAndExitThread(HANDLE module, DWORD code)
{
    FreeLibrary(module);
    ExitThread(code);
}

__declspec(dllexport) void ExitProcess(DWORD code)
{
    void *caller = __builtin_return_address(0);
    extern long NtDisplayString(const char *text);
    static const char hex[] = "0123456789abcdef";
    char message[64];
    int n = 0;
    const char *p = "[k32] ExitProcess(code=0x";
    while (*p)
        message[n++] = *p++;
    for (int i = 0; i < 8; i++)
        message[n++] = hex[(code >> (28 - 4 * i)) & 0xF];
    p = ") caller=0x";
    while (*p)
        message[n++] = *p++;
    for (int i = 0; i < 16; i++)
        message[n++] = hex[((ULONGLONG)caller >> (60 - 4 * i)) & 0xF];
    message[n++] = '\n';
    message[n] = 0;
    NtDisplayString(message);
    NtTerminateThread(0, (NTSTATUS)code); /* simplified: ends the main thread */
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
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForSingleObject(h, 0, timeout);
    if (status == 0x102)
        return 0x102; /* WAIT_TIMEOUT */
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1; /* WAIT_FAILED */
    }
    SetLastError(0);
    return WAIT_OBJECT_0;
}

__declspec(dllexport) DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms,
                                                   BOOL alertable)
{
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForSingleObject(h, alertable, timeout);
    if (status == 0x102)
        return 0x102;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    SetLastError(0);
    return WAIT_OBJECT_0;
}

__declspec(dllexport) DWORD WaitForMultipleObjectsEx(DWORD count,
                                                      const HANDLE *handles,
                                                      BOOL wait_all, DWORD ms,
                                                      BOOL alertable)
{
    long long interval;
    long long *timeout = 0;
    if (ms != (DWORD)-1) {
        interval = ms ? -(long long)ms * 10000 : 0;
        timeout = &interval;
    }
    NTSTATUS status = NtWaitForMultipleObjects(count, handles,
                                                wait_all ? 0 : 1,
                                                alertable, timeout);
    if (status == 0x102)
        return 0x102;
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return (DWORD)-1;
    }
    SetLastError(0);
    return (DWORD)status; /* WAIT_OBJECT_0 + signaled index */
}

__declspec(dllexport) DWORD WaitForMultipleObjects(DWORD count,
                                                    const HANDLE *handles,
                                                    BOOL wait_all, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, wait_all, ms, 0);
}

__declspec(dllexport) HANDLE CreateEventW(LPVOID security_attributes,
                                          BOOL manual_reset,
                                          BOOL initial_state,
                                          const WCHAR *name)
{
    (void)security_attributes;
    (void)name; /* named kernel objects are not exposed to Win32 yet */
    HANDLE event = 0;
    NTSTATUS status = NtCreateEvent(&event, 0x001F0003u, 0,
                                    manual_reset ? 0 : 1, initial_state);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return event;
}

__declspec(dllexport) HANDLE CreateEventA(LPVOID security_attributes,
                                          BOOL manual_reset,
                                          BOOL initial_state,
                                          const char *name)
{
    (void)name;
    return CreateEventW(security_attributes, manual_reset, initial_state, 0);
}

__declspec(dllexport) HANDLE CreateEventExW(LPVOID security_attributes,
                                             const WCHAR *name, DWORD flags,
                                             DWORD desired_access)
{
    const DWORD CREATE_EVENT_MANUAL_RESET = 0x1;
    const DWORD CREATE_EVENT_INITIAL_SET = 0x2;
    (void)desired_access;
    if (flags & ~(CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET)) {
        SetLastError(87);
        return 0;
    }
    return CreateEventW(security_attributes,
                        (flags & CREATE_EVENT_MANUAL_RESET) != 0,
                        (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}

__declspec(dllexport) HANDLE CreateMutexExW(LPVOID security_attributes,
                                             const WCHAR *name, DWORD flags,
                                             DWORD desired_access)
{
    (void)desired_access;
    /* Single-process bootstrap: use a manual-reset dispatcher object as the
     * waitable mutex backing. Named-object sharing and ownership/abandonment
     * will move to a native Mutant object later. */
    (void)flags;
    return CreateEventW(security_attributes, 1, 1, name);
}

__declspec(dllexport) HANDLE CreateMutexW(LPVOID security_attributes,
                                           BOOL initial_owner,
                                           const WCHAR *name)
{
    return CreateMutexExW(security_attributes, name,
                          initial_owner ? 1 : 0, 0x001f0001u);
}

__declspec(dllexport) BOOL ReleaseMutex(HANDLE mutex)
{
    /* Mutexes are backed by manual-reset dispatcher events in the current
     * single-process object model. Releasing makes the object available. */
    NTSTATUS status = NtSetEvent(mutex, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    return 1;
}

__declspec(dllexport) BOOL SetEvent(HANDLE event)
{
    NTSTATUS status = NtSetEvent(event, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL ResetEvent(HANDLE event)
{
    NTSTATUS status = NtResetEvent(event, 0);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
}

__declspec(dllexport) HANDLE CreateSemaphoreExW(
    LPVOID security_attributes, long initial_count, long maximum_count,
    const WCHAR *name, DWORD flags, DWORD desired_access)
{
    (void)security_attributes;
    (void)name;
    (void)desired_access;
    if (flags) {
        SetLastError(87);
        return 0;
    }
    HANDLE semaphore = 0;
    NTSTATUS status = NtCreateSemaphore(&semaphore, 0x001F0003u, 0,
                                        initial_count, maximum_count);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return semaphore;
}

__declspec(dllexport) HANDLE CreateSemaphoreW(LPVOID security_attributes,
                                               long initial_count,
                                               long maximum_count,
                                               const WCHAR *name)
{
    return CreateSemaphoreExW(security_attributes, initial_count,
                              maximum_count, name, 0, 0x001F0003u);
}

__declspec(dllexport) BOOL ReleaseSemaphore(HANDLE semaphore,
                                             long release_count,
                                             long *previous_count)
{
    NTSTATUS status = NtReleaseSemaphore(semaphore, release_count,
                                         previous_count);
    if (!NT_SUCCESS(status)) {
        SetLastError(ntstatus_to_win32(status));
        return 0;
    }
    SetLastError(0);
    return 1;
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

static BLOCK *heap_grow(HEAP *heap, ULONGLONG wanted)
{
    ULONGLONG payload = wanted < 0x10000 ? 0x10000 : align_up8(wanted);
    BLOCK *block = (BLOCK *)nt_alloc(sizeof(BLOCK) + payload);
    if (!block)
        return 0;
    block->size = payload;
    block->next = 0;
    block->free = 1;
    block->pad = 0;

    BLOCK *last = heap->first;
    while (last->next)
        last = last->next;
    last->next = block;
    return block;
}

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
    /* Windows process heaps grow on demand.  Keep segments in the same block
     * list; they need not be virtually adjacent. */
    BLOCK *b = heap_grow(h, size);
    if (!b)
        return 0;
    if (b->size >= size + sizeof(BLOCK) + 16) {
        BLOCK *tail = (BLOCK *)((char *)(b + 1) + size);
        tail->size = b->size - size - sizeof(BLOCK);
        tail->next = b->next;
        tail->free = 1;
        tail->pad = 0;
        b->next = tail;
        b->size = size;
    }
    b->free = 0;
    if (flags & HEAP_ZERO_MEMORY)
        memset(b + 1, 0, size);
    return b + 1;
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
        BOOL adjacent = (char *)(c + 1) + c->size == (char *)c->next;
        if (c->free && c->next->free && adjacent) {
            c->size += sizeof(BLOCK) + c->next->size;
            c->next = c->next->next;
        } else {
            c = c->next;
        }
    }
    return 1;
}

static BLOCK *heap_find_block(HEAP *heap, const void *ptr)
{
    if (!heap || !ptr)
        return 0;
    for (BLOCK *b = heap->first; b; b = b->next)
        if ((const void *)(b + 1) == ptr)
            return b;
    return 0;
}

__declspec(dllexport) SIZE_T HeapSize(HANDLE heap, DWORD flags,
                                      const void *ptr)
{
    (void)flags;
    BLOCK *b = heap_find_block((HEAP *)heap, ptr);
    if (!b || b->free) {
        SetLastError(6); /* ERROR_INVALID_HANDLE / invalid heap block */
        return (SIZE_T)-1;
    }
    return (SIZE_T)b->size;
}

__declspec(dllexport) BOOL HeapValidate(HANDLE heap, DWORD flags,
                                        const void *ptr)
{
    (void)flags;
    if (!heap)
        return 0;
    if (!ptr)
        return 1;
    BLOCK *b = heap_find_block((HEAP *)heap, ptr);
    return b && !b->free;
}

__declspec(dllexport) LPVOID HeapReAlloc(HANDLE heap, DWORD flags,
                                         LPVOID ptr, SIZE_T bytes)
{
    if (!ptr)
        return HeapAlloc(heap, flags, bytes);
    if (!bytes) {
        HeapFree(heap, 0, ptr);
        return 0;
    }

    HEAP *h = (HEAP *)heap;
    BLOCK *b = heap_find_block(h, ptr);
    if (!b || b->free) {
        SetLastError(6);
        return 0;
    }
    ULONGLONG wanted = align_up8(bytes);
    ULONGLONG old_size = b->size;
    if (wanted <= old_size) {
        if (old_size >= wanted + sizeof(BLOCK) + 16) {
            BLOCK *tail = (BLOCK *)((char *)(b + 1) + wanted);
            tail->size = old_size - wanted - sizeof(BLOCK);
            tail->next = b->next;
            tail->free = 1;
            b->next = tail;
            b->size = wanted;
        }
        return ptr;
    }

    if (b->next && b->next->free &&
        old_size + sizeof(BLOCK) + b->next->size >= wanted) {
        b->size += sizeof(BLOCK) + b->next->size;
        b->next = b->next->next;
        if (flags & HEAP_ZERO_MEMORY)
            memset((char *)ptr + old_size, 0, wanted - old_size);
        return ptr;
    }

    LPVOID replacement = HeapAlloc(heap, flags, bytes);
    if (!replacement)
        return 0;
    memcpy(replacement, ptr, old_size < bytes ? old_size : bytes);
    HeapFree(heap, 0, ptr);
    return replacement;
}

__declspec(dllexport) BOOL HeapSetInformation(HANDLE heap, int info_class,
                                              LPVOID info, SIZE_T info_size)
{
    (void)heap; (void)info_class; (void)info; (void)info_size;
    return 1;
}

__declspec(dllexport) LPVOID LocalAlloc(DWORD flags, SIZE_T bytes)
{
    return HeapAlloc(GetProcessHeap(),
                     (flags & 0x0040) ? HEAP_ZERO_MEMORY : 0, bytes);
}

__declspec(dllexport) LPVOID LocalFree(LPVOID ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
    return 0;
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

__declspec(dllexport) BOOL FileTimeToSystemTime(const FILETIME *file_time,
                                                SYSTEMTIME *system_time)
{
    static const BYTE month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!file_time || !system_time) {
        SetLastError(87);
        return 0;
    }
    ULONGLONG ticks = ((ULONGLONG)file_time->High << 32) | file_time->Low;
    ULONGLONG total_seconds = ticks / 10000000ULL;
    ULONGLONG days = total_seconds / 86400ULL;
    DWORD seconds = (DWORD)(total_seconds % 86400ULL);
    DWORD day_of_week = (DWORD)((days + 1) % 7); /* 1601-01-01 was Monday */

    DWORD year = 1601;
    while (year < 30828) {
        DWORD count = leap_year(year) ? 366 : 365;
        if (days < count) break;
        days -= count;
        year++;
    }
    if (year == 30828) {
        SetLastError(87);
        return 0;
    }
    DWORD month = 1;
    while (month <= 12) {
        DWORD count = month_days[month - 1];
        if (month == 2 && leap_year(year)) count++;
        if (days < count) break;
        days -= count;
        month++;
    }
    memset(system_time, 0, sizeof(*system_time));
    system_time->Year = (WORD)year;
    system_time->Month = (WORD)month;
    system_time->Day = (WORD)(days + 1);
    system_time->DayOfWeek = (WORD)day_of_week;
    system_time->Hour = (WORD)(seconds / 3600);
    system_time->Minute = (WORD)((seconds / 60) % 60);
    system_time->Second = (WORD)(seconds % 60);
    system_time->Milliseconds = (WORD)((ticks % 10000000ULL) / 10000ULL);
    return 1;
}

__declspec(dllexport) BOOL FileTimeToLocalFileTime(const FILETIME *utc,
                                                    FILETIME *local)
{
    if (!utc || !local) {
        SetLastError(87);
        return 0;
    }
    /* NTOS has no configured time-zone database yet, so local time is UTC. */
    *local = *utc;
    return 1;
}

static void put_dec_w(WCHAR *out, DWORD value, DWORD digits)
{
    while (digits) {
        out[--digits] = (WCHAR)('0' + value % 10);
        value /= 10;
    }
}

static int copy_formatted_time(const WCHAR *value, int length,
                               WCHAR *out, int capacity)
{
    int required = length + 1;
    if (!out || capacity == 0)
        return required;
    if (capacity < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    for (int i = 0; i <= length; i++) out[i] = value[i];
    return required;
}

__declspec(dllexport) int GetDateFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[11];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Year, 4);
    value[4] = '-';
    put_dec_w(value + 5, time->Month, 2);
    value[7] = '-';
    put_dec_w(value + 8, time->Day, 2);
    value[10] = 0;
    return copy_formatted_time(value, 10, out, capacity);
}

__declspec(dllexport) int GetTimeFormatW(DWORD locale, DWORD flags,
                                         const SYSTEMTIME *time,
                                         const WCHAR *format, WCHAR *out,
                                         int capacity)
{
    WCHAR value[9];
    (void)locale; (void)flags; (void)format;
    if (!time) {
        SetLastError(87);
        return 0;
    }
    put_dec_w(value, time->Hour, 2);
    value[2] = ':';
    put_dec_w(value + 3, time->Minute, 2);
    value[5] = ':';
    put_dec_w(value + 6, time->Second, 2);
    value[8] = 0;
    return copy_formatted_time(value, 8, out, capacity);
}

__declspec(dllexport) void Sleep(DWORD ms)
{
    /* Relative delay: negative 100 ns units. */
    long long interval = -(long long)ms * 10000;
    NtDelayExecution(0, &interval);
}

__declspec(dllexport) DWORD SleepEx(DWORD ms, BOOL alertable)
{
    (void)alertable;
    Sleep(ms);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command line (from PEB->ProcessParameters)                          */
/* ------------------------------------------------------------------ */

static char g_cmdline[260];
static WCHAR g_empty_wcmdline[1];

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

__declspec(dllexport) WCHAR *GetCommandLineW(void)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (pp) {
        WCHAR *buffer = *(WCHAR **)(pp + 0x78);
        if (buffer)
            return buffer;
    }
    return g_empty_wcmdline;
}

__declspec(dllexport) WORD GetUserDefaultUILanguage(void)
{
    return 0x0409; /* en-US, matching the supplied inbox binaries */
}

__declspec(dllexport) BOOL GetVersionExW(void *version_info)
{
    if (!version_info)
        return 0;
    DWORD size = *(DWORD *)version_info;
    if (size < 20)
        return 0;
    DWORD *v = (DWORD *)version_info;
    v[1] = 10;       /* major */
    v[2] = 0;        /* minor */
    v[3] = 14393;    /* RS1 build used by Explorer */
    v[4] = 2;        /* VER_PLATFORM_WIN32_NT */
    return 1;
}

__declspec(dllexport) BOOL GetProductInfo(DWORD major, DWORD minor,
                                           DWORD sp_major, DWORD sp_minor,
                                           DWORD *product)
{
    (void)major; (void)minor; (void)sp_major; (void)sp_minor;
    if (!product)
        return 0;
    *product = 0x30; /* PRODUCT_PROFESSIONAL */
    return 1;
}

__declspec(dllexport) void OutputDebugStringW(const WCHAR *text)
{
    (void)text;
}

#define LOCAL_ATOM_CAPACITY 128
static WCHAR g_atom_names[LOCAL_ATOM_CAPACITY][64];
static WORD g_atom_count;

__declspec(dllexport) WORD GlobalAddAtomW(const WCHAR *name)
{
    if (!name)
        return 0;
    for (WORD i = 0; i < g_atom_count; i++) {
        unsigned j = 0;
        while (g_atom_names[i][j] && name[j] &&
               g_atom_names[i][j] == name[j])
            j++;
        if (!g_atom_names[i][j] && !name[j])
            return (WORD)(0xc000 + i);
    }
    if (g_atom_count >= LOCAL_ATOM_CAPACITY)
        return 0;
    WORD slot = g_atom_count++;
    unsigned j = 0;
    for (; name[j] && j < 63; j++)
        g_atom_names[slot][j] = name[j];
    g_atom_names[slot][j] = 0;
    return (WORD)(0xc000 + slot);
}

__declspec(dllexport) WORD AddAtomW(const WCHAR *name)
{
    return GlobalAddAtomW(name);
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

__declspec(dllexport) int lstrlenW(const WCHAR *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static WCHAR nls_fold_w(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - ('a' - 'A')) : c;
}

static BYTE nls_fold_a(BYTE c)
{
    return (c >= 'a' && c <= 'z') ? (BYTE)(c - ('a' - 'A')) : c;
}

__declspec(dllexport) DWORD GetThreadLocale(void)
{
    return LANG_ENGLISH_US;
}

__declspec(dllexport) DWORD GetUserDefaultLCID(void)
{
    return LANG_ENGLISH_US;
}

/* CompareString returns CSTR_LESS_THAN/EQUAL/GREATER_THAN (1/2/3), never
 * strcmp-style zero on success.  Supporting NORM_IGNORECASE is sufficient for
 * command-line parsers and path matching while NTOS remains ASCII-only. */
__declspec(dllexport) int CompareStringW(DWORD locale, DWORD flags,
                                         const WCHAR *a, int a_len,
                                         const WCHAR *b, int b_len)
{
    (void)locale;
    if (!a || !b) return 0;
    if (a_len < 0) a_len = lstrlenW(a);
    if (b_len < 0) b_len = lstrlenW(b);
    int common = a_len < b_len ? a_len : b_len;
    for (int i = 0; i < common; i++) {
        WCHAR ca = (flags & 1) ? nls_fold_w(a[i]) : a[i];
        WCHAR cb = (flags & 1) ? nls_fold_w(b[i]) : b[i];
        if (ca != cb) return ca < cb ? 1 : 3;
    }
    return a_len == b_len ? 2 : (a_len < b_len ? 1 : 3);
}

__declspec(dllexport) int CompareStringA(DWORD locale, DWORD flags,
                                         const char *a, int a_len,
                                         const char *b, int b_len)
{
    (void)locale;
    if (!a || !b) return 0;
    if (a_len < 0) { a_len = 0; while (a[a_len]) a_len++; }
    if (b_len < 0) { b_len = 0; while (b[b_len]) b_len++; }
    int common = a_len < b_len ? a_len : b_len;
    for (int i = 0; i < common; i++) {
        BYTE ca = (BYTE)a[i], cb = (BYTE)b[i];
        if (flags & 1) { ca = nls_fold_a(ca); cb = nls_fold_a(cb); }
        if (ca != cb) return ca < cb ? 1 : 3;
    }
    return a_len == b_len ? 2 : (a_len < b_len ? 1 : 3);
}

__declspec(dllexport) int FindStringOrdinal(DWORD find_flags,
                                            const WCHAR *source,
                                            int source_len,
                                            const WCHAR *value,
                                            int value_len,
                                            BOOL ignore_case)
{
    if (!source || !value) return -1;
    if (source_len < 0) source_len = lstrlenW(source);
    if (value_len < 0) value_len = lstrlenW(value);
    if (value_len > source_len) return -1;
    int first = 0, last = source_len - value_len, step = 1;
    if (find_flags & 1) { first = last; last = 0; step = -1; }
    for (int pos = first;; pos += step) {
        int i = 0;
        for (; i < value_len; i++) {
            WCHAR a = source[pos + i], b = value[i];
            if (ignore_case) { a = nls_fold_w(a); b = nls_fold_w(b); }
            if (a != b) break;
        }
        if (i == value_len) return pos;
        if (pos == last) break;
    }
    return -1;
}

static int wide_ieq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR ca = (*a >= 'a' && *a <= 'z') ? (WCHAR)(*a - 32) : *a;
        WCHAR cb = (*b >= 'a' && *b <= 'z') ? (WCHAR)(*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static DWORD copy_wide_value(const WCHAR *value, WCHAR *out, DWORD size)
{
    DWORD n = (DWORD)lstrlenW(value);
    if (!out || size == 0)
        return n + 1;
    if (size <= n) {
        for (DWORD i = 0; i + 1 < size; i++) out[i] = value[i];
        if (size) out[size - 1] = 0;
        return n + 1;
    }
    for (DWORD i = 0; i <= n; i++) out[i] = value[i];
    return n;
}

__declspec(dllexport) DWORD GetEnvironmentVariableW(const WCHAR *name,
                                                     WCHAR *out, DWORD size)
{
    static const WCHAR path_name[] = {'P','A','T','H',0};
    static const WCHAR path_value[] = {'C',':','\\',0};
    static const WCHAR ext_name[] = {'P','A','T','H','E','X','T',0};
    static const WCHAR ext_value[] =
        {'.','C','O','M',';','.','E','X','E',';','.','B','A','T',';',
         '.','C','M','D',0};
    static const WCHAR root_name[] =
        {'S','Y','S','T','E','M','R','O','O','T',0};
    static const WCHAR root_value[] = {'C',':','\\','W','I','N','D','O','W','S',0};
    const WCHAR *value = 0;
    if (name && wide_ieq(name, path_name)) value = path_value;
    else if (name && wide_ieq(name, ext_name)) value = ext_value;
    else if (name && wide_ieq(name, root_name)) value = root_value;
    if (!value) {
        SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }
    return copy_wide_value(value, out, size);
}

__declspec(dllexport) DWORD GetCurrentDirectoryW(DWORD size, WCHAR *out)
{
    static const WCHAR root[] = {'C',':','\\',0};
    return copy_wide_value(root, out, size);
}

__declspec(dllexport) DWORD GetFullPathNameW(const WCHAR *path, DWORD size,
                                             WCHAR *out, WCHAR **file_part)
{
    WCHAR full[520];
    int n = 0;
    BOOL absolute = path && path[0] && path[1] == ':';
    if (!absolute) {
        full[n++] = 'C'; full[n++] = ':'; full[n++] = '\\';
    }
    for (int i = 0; path && path[i] && n < 519; i++)
        full[n++] = path[i];
    full[n] = 0;
    DWORD result = copy_wide_value(full, out, size);
    if (file_part && out && size > (DWORD)n) {
        *file_part = out;
        for (int i = 0; i < n; i++)
            if (out[i] == '\\' || out[i] == '/') *file_part = &out[i + 1];
    }
    return result;
}

__declspec(dllexport) DWORD GetLongPathNameW(const WCHAR *path, WCHAR *out,
                                             DWORD size)
{
    return copy_wide_value(path, out, size);
}

__declspec(dllexport) WCHAR *CharUpperW(WCHAR *text)
{
    ULONGLONG value = (ULONGLONG)text;
    if ((value >> 16) == 0) {
        WCHAR c = (WCHAR)value;
        if (c >= 'a' && c <= 'z') c -= 32;
        return (WCHAR *)(ULONGLONG)c;
    }
    for (WCHAR *p = text; p && *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return text;
}

static BOOL wide_in_set(WCHAR c, const WCHAR *set)
{
    while (set && *set)
        if (c == *set++) return 1;
    return 0;
}

__declspec(dllexport) BOOL StrTrimW(WCHAR *text, const WCHAR *trim)
{
    if (!text || !trim) return 0;
    int start = 0, len = lstrlenW(text);
    while (start < len && wide_in_set(text[start], trim)) start++;
    while (len > start && wide_in_set(text[len - 1], trim)) len--;
    BOOL changed = start != 0 || text[len] != 0;
    int out = 0;
    while (start < len) text[out++] = text[start++];
    text[out] = 0;
    return changed;
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

/* A small wsprintfA: supports %d %u %x %p %s %c %% (no width/precision). */
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
        case 'p':
            out[o++] = '0'; out[o++] = 'x';
            o += fmt_uint(out + o, (ULONGLONG)__builtin_va_arg(ap, void *), 16, 0, 0);
            break;
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

/* ------------------------------------------------------------------ */
/* Thread / process information (from the TEB)                         */
/* ------------------------------------------------------------------ */

/* TEB offsets: ClientId.UniqueProcess 0x40, UniqueThread 0x48, LastError 0x68. */
__declspec(dllexport) DWORD  GetLastError(void)        { return read_gs_dword(0x68); }
__declspec(dllexport) void   SetLastError(DWORD e)     { write_gs_dword(0x68, e); }
__declspec(dllexport) DWORD  GetCurrentProcessId(void) { return (DWORD)read_gs_qword(0x40); }
__declspec(dllexport) DWORD  GetCurrentThreadId(void)  { return (DWORD)read_gs_qword(0x48); }
__declspec(dllexport) HANDLE GetCurrentProcess(void)   { return (HANDLE)(ULONGLONG)-1; }
__declspec(dllexport) HANDLE GetCurrentThread(void)    { return (HANDLE)(ULONGLONG)-2; }

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */

/* Win64 has 64 inline TLS slots.  Keep the process allocation bitmap in
 * kernel32 and hang each thread's slot vector from TEB+0x58
 * (ThreadLocalStoragePointer).  The real TEB also mirrors these values at
 * TlsSlots, but using the canonical pointer is sufficient for the public
 * Tls* API and avoids requiring an oversized TEB while that layout grows. */
#define TLS_MINIMUM_AVAILABLE 64
#define TLS_OUT_OF_INDEXES    ((DWORD)-1)

static volatile ULONGLONG g_tls_bitmap;

static void **tls_slots(BOOL create)
{
    void **slots = (void **)(ULONGLONG)read_gs_qword(0x58);
    if (!slots && create) {
        void **fresh = (void **)nt_alloc(TLS_MINIMUM_AVAILABLE * sizeof(void *));
        if (!fresh)
            return 0;
        memset(fresh, 0, TLS_MINIMUM_AVAILABLE * sizeof(void *));

        /* Only the current thread writes its own TEB, so no cross-thread CAS
         * is needed here. */
        __asm__ volatile("movq %0, %%gs:0x58" : : "r"(fresh) : "memory");
        slots = fresh;
    }
    return slots;
}

__declspec(dllexport) DWORD TlsAlloc(void)
{
    for (;;) {
        ULONGLONG old = g_tls_bitmap;
        if (old == ~(ULONGLONG)0) {
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return TLS_OUT_OF_INDEXES;
        }
        DWORD index = 0;
        while (old & ((ULONGLONG)1 << index))
            index++;
        ULONGLONG updated = old | ((ULONGLONG)1 << index);
        if (__sync_bool_compare_and_swap(&g_tls_bitmap, old, updated)) {
            SetLastError(0);
            return index;
        }
    }
}

__declspec(dllexport) BOOL TlsFree(DWORD index)
{
    if (index >= TLS_MINIMUM_AVAILABLE) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    ULONGLONG mask = (ULONGLONG)1 << index;
    ULONGLONG old;
    do {
        old = g_tls_bitmap;
        if (!(old & mask)) {
            SetLastError(87);
            return 0;
        }
    } while (!__sync_bool_compare_and_swap(&g_tls_bitmap, old, old & ~mask));

    void **slots = tls_slots(0);
    if (slots)
        slots[index] = 0;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) void *TlsGetValue(DWORD index)
{
    if (index >= TLS_MINIMUM_AVAILABLE ||
        !(g_tls_bitmap & ((ULONGLONG)1 << index))) {
        SetLastError(87);
        return 0;
    }
    void **slots = tls_slots(0);
    SetLastError(0); /* NULL is a valid stored TLS value. */
    return slots ? slots[index] : 0;
}

__declspec(dllexport) BOOL TlsSetValue(DWORD index, void *value)
{
    if (index >= TLS_MINIMUM_AVAILABLE ||
        !(g_tls_bitmap & ((ULONGLONG)1 << index))) {
        SetLastError(87);
        return 0;
    }
    void **slots = tls_slots(1);
    if (!slots) {
        SetLastError(8);
        return 0;
    }
    slots[index] = value;
    SetLastError(0);
    return 1;
}

__declspec(dllexport) BOOL TerminateProcess(HANDLE process, DWORD code)
{
    (void)process;
    ExitProcess(code);
    return 1;
}

__declspec(dllexport) DWORD SetThreadUILanguage(DWORD language)
{
    /* Zero selects the process/user default and must still return a valid
     * LANGID.  Several inbox command-line utilities treat zero as fatal. */
    return language ? language : LANG_ENGLISH_US;
}

__declspec(dllexport) DWORD GetFileType(HANDLE file)
{
    (void)file;
    return 2; /* FILE_TYPE_CHAR: our standard handles are the console */
}

/* A deliberately small UTF-16 -> narrow converter.  ASCII is enough for the
 * hostname and current console; non-ASCII input is replaced with '?'. */
__declspec(dllexport) int WideCharToMultiByte(unsigned code_page, DWORD flags,
                                              const WCHAR *wide, int wide_len,
                                              char *out, int out_len,
                                              const char *default_char,
                                              BOOL *used_default)
{
    (void)code_page; (void)flags; (void)default_char;
    int n = 0;
    BOOL replaced = 0;
    if (!wide)
        return 0;
    if (wide_len < 0) {
        while (wide[n]) n++;
        wide_len = n + 1;
    }
    if (!out || out_len == 0)
        return wide_len;
    for (n = 0; n < wide_len && n < out_len; n++) {
        WCHAR c = wide[n];
        if (c <= 0x7f)
            out[n] = (char)c;
        else {
            out[n] = '?';
            replaced = 1;
        }
    }
    if (used_default)
        *used_default = replaced;
    return n;
}

__declspec(dllexport) int MultiByteToWideChar(unsigned code_page, DWORD flags,
                                              const char *text, int text_len,
                                              WCHAR *out, int out_len)
{
    (void)code_page;
    (void)flags;
    if (!text)
        return 0;
    if (text_len < 0) {
        text_len = 0;
        while (text[text_len++])
            ;
    }
    if (!out || out_len == 0)
        return text_len;
    int n = 0;
    for (; n < text_len && n < out_len; n++)
        out[n] = (WCHAR)(BYTE)text[n];
    return n;
}

/* ------------------------------------------------------------------ */
/* Root directory enumeration (first Win32 filesystem-search layer)    */
/* ------------------------------------------------------------------ */

typedef struct _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    WCHAR    cFileName[260];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct _NTOS_FAT_FIND_DATA {
    char Name[260];
    DWORD Size;
    DWORD Attributes;
    WORD WriteDate;
    WORD WriteTime;
} NTOS_FAT_FIND_DATA;

typedef struct _FIND_HANDLE {
    DWORD NextIndex;
    WCHAR Pattern[260];
} FIND_HANDLE;

static WCHAR fold_char(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - ('a' - 'A')) : c;
}

static BOOL wildcard_match(const WCHAR *pattern, const char *name)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (wildcard_match(pattern, name)) return 1;
                name++;
            }
            return wildcard_match(pattern, name);
        }
        if (!*name || (*pattern != '?' &&
            fold_char(*pattern) != fold_char((WCHAR)(BYTE)*name)))
            return 0;
        pattern++;
        name++;
    }
    return *name == 0;
}

static void fill_find_data(WIN32_FIND_DATAW *out,
                           const NTOS_FAT_FIND_DATA *in)
{
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = in->Attributes;
    out->nFileSizeLow = in->Size;
    out->ftLastWriteTime = ticks_to_filetime(
        fat_datetime_ticks(in->WriteDate, in->WriteTime));
    int i = 0;
    for (; in->Name[i] && i < 259; i++)
        out->cFileName[i] = (WCHAR)(BYTE)in->Name[i];
    out->cFileName[i] = 0;
}

static BOOL find_next_match(FIND_HANDLE *find, WIN32_FIND_DATAW *out)
{
    NTOS_FAT_FIND_DATA entry;
    while (NtEnumerateRootFiles(find->NextIndex++, &entry) >= 0) {
        if (wildcard_match(find->Pattern, entry.Name)) {
            fill_find_data(out, &entry);
            return 1;
        }
    }
    SetLastError(18); /* ERROR_NO_MORE_FILES */
    return 0;
}

__declspec(dllexport) DWORD GetFileAttributesW(const WCHAR *path)
{
    NTOS_FAT_FIND_DATA entry;
    const WCHAR *leaf;

    if (!path || !*path) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return (DWORD)-1;
    }

    /* The filesystem currently exposes one FAT volume as C:\. */
    leaf = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    if (!*leaf || (path[0] && path[1] == ':' && !path[2]))
        return 0x10; /* FILE_ATTRIBUTE_DIRECTORY */

    for (DWORD index = 0; NtEnumerateRootFiles(index, &entry) >= 0; index++) {
        if (wildcard_match(leaf, entry.Name))
            return entry.Attributes ? entry.Attributes : 0x20;
    }

    SetLastError(2); /* ERROR_FILE_NOT_FOUND */
    return (DWORD)-1; /* INVALID_FILE_ATTRIBUTES */
}

__declspec(dllexport) HANDLE FindFirstFileExW(const WCHAR *path, int info_level,
                                              WIN32_FIND_DATAW *out,
                                              int search_op, LPVOID filter,
                                              DWORD flags)
{
    (void)info_level; (void)search_op; (void)filter; (void)flags;
    if (!path || !out) {
        SetLastError(87);
        return (HANDLE)(ULONGLONG)-1;
    }
    FIND_HANDLE *find = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(*find));
    if (!find) {
        SetLastError(8);
        return (HANDLE)(ULONGLONG)-1;
    }
    const WCHAR *leaf = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    int i = 0;
    for (; leaf[i] && i < 259; i++) find->Pattern[i] = leaf[i];
    find->Pattern[i] = 0;
    if (!find_next_match(find, out)) {
        HeapFree(GetProcessHeap(), 0, find);
        SetLastError(2);
        return (HANDLE)(ULONGLONG)-1;
    }
    return find;
}

__declspec(dllexport) BOOL FindNextFileW(HANDLE handle, WIN32_FIND_DATAW *out)
{
    return handle && handle != (HANDLE)(ULONGLONG)-1 && out
               ? find_next_match((FIND_HANDLE *)handle, out) : 0;
}

__declspec(dllexport) BOOL FindClose(HANDLE handle)
{
    if (!handle || handle == (HANDLE)(ULONGLONG)-1) return 0;
    return HeapFree(GetProcessHeap(), 0, handle);
}

/* ------------------------------------------------------------------ */
/* Interlocked operations                                              */
/* ------------------------------------------------------------------ */

typedef long LONG;
#define WINAPI
typedef unsigned int UINT;
typedef long long LONG_PTR;
typedef HANDLE *PHANDLE;

__declspec(dllexport) LONG InterlockedIncrement(LONG volatile *p)
{
    return __sync_add_and_fetch(p, 1);
}
__declspec(dllexport) LONG InterlockedDecrement(LONG volatile *p)
{
    return __sync_sub_and_fetch(p, 1);
}
__declspec(dllexport) LONG InterlockedExchange(LONG volatile *p, LONG value)
{
    return __sync_lock_test_and_set(p, value);
}
__declspec(dllexport) LONG InterlockedCompareExchange(LONG volatile *dst,
                                                      LONG exchange, LONG compare)
{
    return __sync_val_compare_and_swap(dst, compare, exchange);
}

/* ------------------------------------------------------------------ */
/* Critical sections (recursive lock)                                  */
/* ------------------------------------------------------------------ */

typedef struct _CRIT { LONG lock; DWORD owner; LONG recursion; } CRIT;

__declspec(dllexport) void InitializeCriticalSection(void *cs)
{
    CRIT *c = (CRIT *)cs;
    c->lock = 0;
    c->owner = 0;
    c->recursion = 0;
}

__declspec(dllexport) void EnterCriticalSection(void *cs)
{
    CRIT *c = (CRIT *)cs;
    DWORD me = GetCurrentThreadId();
    if (c->owner == me) {
        c->recursion++;
        return;
    }
    while (!__sync_bool_compare_and_swap(&c->lock, 0, 1))
        __asm__ volatile("pause"); /* spin; the timer preempts to run the owner */
    c->owner = me;
    c->recursion = 1;
}

__declspec(dllexport) BOOL TryEnterCriticalSection(void *cs)
{
    CRIT *c = (CRIT *)cs;
    DWORD me = GetCurrentThreadId();
    if (c->owner == me) {
        c->recursion++;
        return 1;
    }
    if (!__sync_bool_compare_and_swap(&c->lock, 0, 1))
        return 0;
    c->owner = me;
    c->recursion = 1;
    return 1;
}

__declspec(dllexport) void LeaveCriticalSection(void *cs)
{
    CRIT *c = (CRIT *)cs;
    if (--c->recursion == 0) {
        c->owner = 0;
        __sync_lock_release(&c->lock);
    }
}

__declspec(dllexport) void DeleteCriticalSection(void *cs) { (void)cs; }

__declspec(dllexport) BOOL InitializeCriticalSectionAndSpinCount(void *cs,
                                                                 DWORD count)
{
    (void)count;
    InitializeCriticalSection(cs);
    return 1;
}

__declspec(dllexport) BOOL InitializeCriticalSectionEx(void *cs, DWORD count,
                                                        DWORD flags)
{
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, count);
}

/* ------------------------------------------------------------------ */
/* Slim reader/writer locks                                            */
/* ------------------------------------------------------------------ */

/* SRWLOCK is one pointer-sized word in the public ABI. The first correct
 * implementation serializes shared and exclusive acquisitions alike. This is
 * intentionally conservative but provides real cross-thread exclusion and
 * preserves the layout expected by unmodified Windows binaries. */
static void srw_acquire(void *lock)
{
    ULONGLONG volatile *word = (ULONGLONG volatile *)lock;
    while (!__sync_bool_compare_and_swap(word, 0, 1))
        __asm__ volatile("pause");
}

__declspec(dllexport) void AcquireSRWLockExclusive(void *lock)
{
    srw_acquire(lock);
}

__declspec(dllexport) void InitializeSRWLock(void *lock)
{
    *(ULONGLONG volatile *)lock = 0;
}

__declspec(dllexport) void AcquireSRWLockShared(void *lock)
{
    srw_acquire(lock);
}

__declspec(dllexport) BOOL TryAcquireSRWLockExclusive(void *lock)
{
    return __sync_bool_compare_and_swap((ULONGLONG volatile *)lock, 0, 1);
}

__declspec(dllexport) BOOL TryAcquireSRWLockShared(void *lock)
{
    return TryAcquireSRWLockExclusive(lock);
}

static void srw_release(void *lock)
{
    __sync_lock_release((ULONGLONG volatile *)lock);
}

__declspec(dllexport) void ReleaseSRWLockExclusive(void *lock)
{
    srw_release(lock);
}

__declspec(dllexport) void ReleaseSRWLockShared(void *lock)
{
    srw_release(lock);
}

/* ------------------------------------------------------------------ */
/* One-time initialization                                             */
/* ------------------------------------------------------------------ */

#define INIT_ONCE_CHECK_ONLY  0x00000001
#define INIT_ONCE_INIT_FAILED 0x00000004

typedef BOOL (*INIT_ONCE_FN)(void *once, void *parameter, void **context);

/* State values 0/1/2 mean uninitialized/in progress/completed with a NULL
 * context.  A completed non-NULL context is stored directly; Win32 requires
 * such context pointers to have their low two bits clear. */
__declspec(dllexport) BOOL InitOnceBeginInitialize(void *init_once, DWORD flags,
                                                   BOOL *pending, void **context)
{
    void *volatile *state = (void *volatile *)init_once;
    if (!state || !pending)
        return 0;

    for (;;) {
        void *value = *state;
        if (!value) {
            if (flags & INIT_ONCE_CHECK_ONLY) {
                *pending = 1;
                if (context) *context = 0;
                return 1;
            }
            if (__sync_bool_compare_and_swap(state, 0, (void *)1)) {
                *pending = 1;
                if (context) *context = 0;
                return 1;
            }
            continue;
        }
        if (value == (void *)1) {
            Sleep(0);
            continue;
        }
        *pending = 0;
        if (context)
            *context = value == (void *)2 ? 0 : value;
        return 1;
    }
}

__declspec(dllexport) BOOL InitOnceComplete(void *init_once, DWORD flags,
                                            void *context)
{
    void *volatile *state = (void *volatile *)init_once;
    if (!state)
        return 0;
    if (flags & INIT_ONCE_INIT_FAILED) {
        __sync_lock_test_and_set(state, 0);
        return 1;
    }
    if ((ULONGLONG)context & 3)
        return 0;
    __sync_lock_test_and_set(state, context ? context : (void *)2);
    return 1;
}

__declspec(dllexport) BOOL InitOnceExecuteOnce(void *init_once,
                                               INIT_ONCE_FN callback,
                                               void *parameter, void **context)
{
    BOOL pending;
    void *result = 0;
    if (!callback || !InitOnceBeginInitialize(init_once, 0, &pending, &result))
        return 0;
    if (pending) {
        if (!callback(init_once, parameter, &result)) {
            InitOnceComplete(init_once, INIT_ONCE_INIT_FAILED, 0);
            return 0;
        }
        if (!InitOnceComplete(init_once, 0, result))
            return 0;
    }
    if (context)
        *context = result;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Minimal thread-pool work objects                                    */
/* ------------------------------------------------------------------ */

typedef void (*THREADPOOL_WORK_CALLBACK)(void *callback_instance,
                                         void *context, void *work);

#define MAX_THREADPOOL_WORK_ITEMS 32
typedef struct _THREADPOOL_WORK_ITEM {
    BOOL Used;
    BOOL Submitted;
    THREADPOOL_WORK_CALLBACK Callback;
    void *Context;
    void *Environment;
    struct _THREADPOOL_WORK_ITEM *Next;
} THREADPOOL_WORK_ITEM;

static THREADPOOL_WORK_ITEM g_threadpool_work_items[MAX_THREADPOOL_WORK_ITEMS];
static THREADPOOL_WORK_ITEM *g_threadpool_work_head;
static THREADPOOL_WORK_ITEM *g_threadpool_work_tail;
static volatile LONG g_threadpool_work_lock;
static volatile LONG g_threadpool_worker_state; /* 0=none, 1=starting, 2=ready */
static HANDLE g_threadpool_work_semaphore;
static HANDLE g_threadpool_workers[4];

static void threadpool_lock(void)
{
    while (!__sync_bool_compare_and_swap(&g_threadpool_work_lock, 0, 1))
        __asm__ volatile("pause");
}

static void threadpool_unlock(void)
{
    __sync_lock_release(&g_threadpool_work_lock);
}

__declspec(dllexport) void *CreateThreadpoolWork(
    THREADPOOL_WORK_CALLBACK callback, void *context, void *environment)
{
    if (!callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    for (int i = 0; i < MAX_THREADPOOL_WORK_ITEMS; i++) {
        THREADPOOL_WORK_ITEM *work = &g_threadpool_work_items[i];
        if (!work->Used) {
            work->Callback = callback;
            work->Context = context;
            work->Environment = environment;
            work->Used = 1;
            SetLastError(0);
            return work;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

static DWORD threadpool_worker_start(void *parameter)
{
    (void)parameter;
    for (;;) {
        if (WaitForSingleObject(g_threadpool_work_semaphore, (DWORD)-1) !=
            WAIT_OBJECT_0)
            continue;

        threadpool_lock();
        THREADPOOL_WORK_ITEM *work = g_threadpool_work_head;
        if (work) {
            g_threadpool_work_head = work->Next;
            if (!g_threadpool_work_head)
                g_threadpool_work_tail = 0;
            work->Next = 0;
        }
        threadpool_unlock();

        if (!work)
            continue;
        work->Callback(0, work->Context, work);
        __sync_lock_test_and_set(&work->Submitted, 0);
    }
}

static BOOL threadpool_ensure_workers(void)
{
    if (g_threadpool_worker_state == 2)
        return 1;

    if (__sync_bool_compare_and_swap(&g_threadpool_worker_state, 0, 1)) {
        g_threadpool_work_semaphore = CreateSemaphoreW(0, 0, 0x7fffffff, 0);
        if (!g_threadpool_work_semaphore) {
            __sync_lock_test_and_set(&g_threadpool_worker_state, 0);
            return 0;
        }
        for (DWORD i = 0; i < 4; i++) {
            g_threadpool_workers[i] =
                CreateThread(0, 0, threadpool_worker_start, 0, 0, 0);
            if (!g_threadpool_workers[i]) {
                __sync_lock_test_and_set(&g_threadpool_worker_state, 0);
                return 0;
            }
        }
        __sync_lock_test_and_set(&g_threadpool_worker_state, 2);
        return 1;
    }

    while (g_threadpool_worker_state == 1)
        Sleep(0);
    return g_threadpool_worker_state == 2;
}

__declspec(dllexport) void SubmitThreadpoolWork(void *work_handle)
{
    THREADPOOL_WORK_ITEM *work = (THREADPOOL_WORK_ITEM *)work_handle;
    if (!work || !work->Used || !work->Callback || work->Submitted) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return;
    }

    if (!threadpool_ensure_workers()) {
        SetLastError(8);
        return;
    }
    if (!__sync_bool_compare_and_swap(&work->Submitted, 0, 1)) {
        SetLastError(87);
        return;
    }

    threadpool_lock();
    work->Next = 0;
    if (g_threadpool_work_tail)
        g_threadpool_work_tail->Next = work;
    else
        g_threadpool_work_head = work;
    g_threadpool_work_tail = work;
    threadpool_unlock();

    if (!ReleaseSemaphore(g_threadpool_work_semaphore, 1, 0)) {
        /* Leave the item queued: another release/submission can still let a
         * worker drain it, but report the synchronization failure. */
        work->Submitted = 0;
        SetLastError(8);
        return;
    }
    SetLastError(0);
}

typedef void (*THREADPOOL_WAIT_CALLBACK)(void *callback_instance,
                                         void *context, void *wait,
                                         DWORD wait_result);

#define MAX_THREADPOOL_WAIT_ITEMS 32
typedef struct _THREADPOOL_WAIT_ITEM {
    BOOL Used;
    BOOL Waiting;
    THREADPOOL_WAIT_CALLBACK Callback;
    void *Context;
    void *Environment;
    HANDLE Object;
} THREADPOOL_WAIT_ITEM;

static THREADPOOL_WAIT_ITEM g_threadpool_wait_items[MAX_THREADPOOL_WAIT_ITEMS];
static volatile LONG g_threadpool_wait_lock;
static volatile LONG g_threadpool_wait_state;
static HANDLE g_threadpool_wait_control;
static HANDLE g_threadpool_wait_monitor;

static void threadpool_wait_lock(void)
{
    while (!__sync_bool_compare_and_swap(&g_threadpool_wait_lock, 0, 1))
        __asm__ volatile("pause");
}

static void threadpool_wait_unlock(void)
{
    __sync_lock_release(&g_threadpool_wait_lock);
}

__declspec(dllexport) void *CreateThreadpoolWait(
    THREADPOOL_WAIT_CALLBACK callback, void *context, void *environment)
{
    if (!callback) {
        SetLastError(87);
        return 0;
    }
    for (int i = 0; i < MAX_THREADPOOL_WAIT_ITEMS; i++) {
        THREADPOOL_WAIT_ITEM *wait = &g_threadpool_wait_items[i];
        if (!wait->Used) {
            wait->Callback = callback;
            wait->Context = context;
            wait->Environment = environment;
            wait->Used = 1;
            SetLastError(0);
            return wait;
        }
    }
    SetLastError(8);
    return 0;
}

static DWORD threadpool_wait_monitor_start(void *parameter)
{
    (void)parameter;
    for (;;) {
        HANDLE handles[MAX_THREADPOOL_WAIT_ITEMS + 1];
        THREADPOOL_WAIT_ITEM *items[MAX_THREADPOOL_WAIT_ITEMS + 1];
        DWORD count = 1;
        handles[0] = g_threadpool_wait_control;
        items[0] = 0;

        threadpool_wait_lock();
        for (int i = 0; i < MAX_THREADPOOL_WAIT_ITEMS; i++) {
            THREADPOOL_WAIT_ITEM *wait = &g_threadpool_wait_items[i];
            if (wait->Used && wait->Waiting && wait->Object) {
                handles[count] = wait->Object;
                items[count] = wait;
                count++;
            }
        }
        threadpool_wait_unlock();

        DWORD result = WaitForMultipleObjects(count, handles, 0, (DWORD)-1);
        if (result == WAIT_OBJECT_0)
            continue; /* control event: rebuild the snapshot */
        if (result == (DWORD)-1 || result < WAIT_OBJECT_0 ||
            result >= WAIT_OBJECT_0 + count) {
            Sleep(10);
            continue;
        }

        DWORD index = result - WAIT_OBJECT_0;
        THREADPOOL_WAIT_ITEM *wait = items[index];
        BOOL invoke = 0;
        threadpool_wait_lock();
        if (wait && wait->Used && wait->Waiting &&
            wait->Object == handles[index]) {
            wait->Waiting = 0; /* one-shot until SetThreadpoolWait rearms it */
            invoke = 1;
        }
        threadpool_wait_unlock();
        if (invoke)
            wait->Callback(0, wait->Context, wait, WAIT_OBJECT_0);
    }
}

static BOOL threadpool_ensure_wait_monitor(void)
{
    if (g_threadpool_wait_state == 2)
        return 1;
    if (__sync_bool_compare_and_swap(&g_threadpool_wait_state, 0, 1)) {
        /* Auto-reset: one reconfiguration wake is enough to rebuild the full
         * object snapshot. */
        g_threadpool_wait_control = CreateEventW(0, 0, 0, 0);
        if (g_threadpool_wait_control)
            g_threadpool_wait_monitor = CreateThread(
                0, 0, threadpool_wait_monitor_start, 0, 0, 0);
        if (!g_threadpool_wait_control || !g_threadpool_wait_monitor) {
            __sync_lock_test_and_set(&g_threadpool_wait_state, 0);
            return 0;
        }
        __sync_lock_test_and_set(&g_threadpool_wait_state, 2);
        return 1;
    }
    while (g_threadpool_wait_state == 1)
        Sleep(0);
    return g_threadpool_wait_state == 2;
}

__declspec(dllexport) void SetThreadpoolWait(void *wait_handle,
                                             HANDLE object,
                                             const void *timeout)
{
    (void)timeout;
    THREADPOOL_WAIT_ITEM *wait = (THREADPOOL_WAIT_ITEM *)wait_handle;
    if (!wait || !wait->Used) {
        SetLastError(87);
        return;
    }
    if (!threadpool_ensure_wait_monitor()) {
        SetLastError(8);
        return;
    }

    threadpool_wait_lock();
    wait->Object = object;
    wait->Waiting = object != 0;
    threadpool_wait_unlock();
    SetEvent(g_threadpool_wait_control);
    SetLastError(0);
}

/* ------------------------------------------------------------------ */
/* Virtual memory                                                      */
/* ------------------------------------------------------------------ */

__declspec(dllexport) LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type,
                                          DWORD protect)
{
    (void)addr; (void)type; (void)protect; /* our allocator picks the address */
    return nt_alloc(size);
}

__declspec(dllexport) BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    (void)addr; (void)size; (void)type; /* no unmap yet */
    return 1;
}

__declspec(dllexport) BOOL VirtualProtect(LPVOID addr, SIZE_T size,
                                          DWORD newProtect, DWORD *oldProtect)
{
    void *base = addr;
    ULONGLONG region = size;
    NTSTATUS st = NtProtectVirtualMemory(NT_INVALID_HANDLE, &base, &region,
                                         newProtect, oldProtect);
    return NT_SUCCESS(st);
}

/* ------------------------------------------------------------------ */
/* Performance counter (backed by KUSER_SHARED_DATA system time)       */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL QueryPerformanceCounter(long long *count)
{
    if (count)
        *count = (long long)read_ksystem_time(0x014); /* 100 ns units */
    return 1;
}

__declspec(dllexport) BOOL QueryPerformanceFrequency(long long *freq)
{
    if (freq)
        *freq = 10000000; /* 100 ns tick -> 10 MHz */
    return 1;
}

/* ------------------------------------------------------------------ */
/* COM bootstrap (COM API-set contracts are currently aliased here)    */
/* ------------------------------------------------------------------ */

/* TEB.ReservedForOle on x64. Genuine OLE32 expects CoInitializeEx to
 * establish a per-apartment block here before OleInitialize increments its
 * apartment/OLE counters. The full combase apartment object will replace this
 * bootstrap allocation as more COM services move out of the kernel32 host. */
#define TEB_RESERVED_FOR_OLE_OFFSET 0x1758

__declspec(dllexport) LONG CoInitializeEx(void *reserved, DWORD flags)
{
    (void)reserved;
    (void)flags;
    void *ole = (void *)(ULONGLONG)
        read_gs_qword(TEB_RESERVED_FOR_OLE_OFFSET);
    if (ole)
        return 1; /* S_FALSE: apartment was already initialized */

    ole = nt_alloc(0x1000);
    if (!ole)
        return (LONG)0x8007000e; /* E_OUTOFMEMORY */
    memset(ole, 0, 0x1000);
    write_gs_qword(TEB_RESERVED_FOR_OLE_OFFSET, (ULONGLONG)ole);
    return 0; /* S_OK */
}

__declspec(dllexport) void CoUninitialize(void)
{
    /* Keep the apartment block alive for the lifetime of this TEB. Native COM
     * also retains substantial per-thread state until thread teardown. */
}

__declspec(dllexport) void *CoTaskMemAlloc(SIZE_T bytes)
{
    return HeapAlloc(GetProcessHeap(), 0, bytes);
}

__declspec(dllexport) void *CoTaskMemRealloc(void *memory, SIZE_T bytes)
{
    if (!memory)
        return CoTaskMemAlloc(bytes);
    return HeapReAlloc(GetProcessHeap(), 0, memory, bytes);
}

__declspec(dllexport) void CoTaskMemFree(void *memory)
{
    if (memory)
        HeapFree(GetProcessHeap(), 0, memory);
}

__declspec(dllexport) LONG CreateBindCtx(DWORD reserved, void **bind_context);

typedef struct _GUID_K {
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[8];
} GUID_K;

static WCHAR guid_hex(unsigned int value)
{
    return (WCHAR)(value < 10 ? '0' + value : 'A' + value - 10);
}

__declspec(dllexport) int StringFromGUID2(const GUID_K *guid, WCHAR *out,
                                          int capacity)
{
    if (!guid || !out || capacity < 39)
        return 0;
    int p = 0;
    out[p++] = '{';
#define PUT_HEX(value, digits) do { \
        unsigned long long _v = (unsigned long long)(value); \
        for (int _i = (digits) - 1; _i >= 0; _i--) \
            out[p++] = guid_hex((unsigned int)((_v >> (_i * 4)) & 0xf)); \
    } while (0)
    PUT_HEX(guid->Data1, 8); out[p++] = '-';
    PUT_HEX(guid->Data2, 4); out[p++] = '-';
    PUT_HEX(guid->Data3, 4); out[p++] = '-';
    PUT_HEX(guid->Data4[0], 2); PUT_HEX(guid->Data4[1], 2); out[p++] = '-';
    for (int i = 2; i < 8; i++) PUT_HEX(guid->Data4[i], 2);
    out[p++] = '}';
    out[p] = 0;
#undef PUT_HEX
    return p + 1;
}

typedef struct _SHELL_ITEM SHELL_ITEM;
typedef struct _SHELL_ITEM_VTBL {
    LONG  (*QueryInterface)(SHELL_ITEM *, const GUID_K *, void **);
    DWORD (*AddRef)(SHELL_ITEM *);
    DWORD (*Release)(SHELL_ITEM *);
    LONG  (*BindToHandler)(SHELL_ITEM *, void *, const GUID_K *,
                           const GUID_K *, void **);
    LONG  (*GetParent)(SHELL_ITEM *, void **);
    LONG  (*GetDisplayName)(SHELL_ITEM *, DWORD, WCHAR **);
    LONG  (*GetAttributes)(SHELL_ITEM *, DWORD, DWORD *);
    LONG  (*Compare)(SHELL_ITEM *, SHELL_ITEM *, DWORD, int *);
} SHELL_ITEM_VTBL;

struct _SHELL_ITEM {
    const SHELL_ITEM_VTBL *Vtbl;
    volatile LONG References;
};

static const GUID_K g_iid_iunknown = {
    0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x46 }
};
static const GUID_K g_iid_ishellitem = {
    0x43826D1E, 0xE718, 0x42EE, { 0xBC, 0x55, 0xA1, 0xE2,
                                 0x61, 0xC3, 0x7B, 0xFE }
};
static const GUID_K g_folderid_programs = {
    0xA77F5D77, 0x2E2B, 0x44C3, { 0xA6, 0xA2, 0xAB, 0xA6,
                                  0x01, 0x05, 0x4A, 0x51 }
};
static const GUID_K g_iid_ibindctx = {
    0x0000000E, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x46 }
};
static const GUID_K g_bhid_enum_items = {
    0x94F60519, 0x2850, 0x4924, { 0xAA, 0x5A, 0xD1, 0x5E,
                                  0x84, 0x86, 0x80, 0x39 }
};
static const GUID_K g_iid_ienum_shell_items = {
    0x70629033, 0xE363, 0x4A28, { 0xA5, 0x67, 0x0D, 0xB7,
                                  0x80, 0x06, 0xE6, 0xD7 }
};

static BOOL guid_equal(const GUID_K *a, const GUID_K *b)
{
    if (!a || !b || a->Data1 != b->Data1 || a->Data2 != b->Data2 ||
        a->Data3 != b->Data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->Data4[i] != b->Data4[i])
            return 0;
    return 1;
}

typedef struct _BIND_CONTEXT BIND_CONTEXT;
typedef struct _BIND_CONTEXT_VTBL {
    LONG  (*QueryInterface)(BIND_CONTEXT *, const GUID_K *, void **);
    DWORD (*AddRef)(BIND_CONTEXT *);
    DWORD (*Release)(BIND_CONTEXT *);
    LONG  (*RegisterObjectBound)(BIND_CONTEXT *, void *);
    LONG  (*RevokeObjectBound)(BIND_CONTEXT *, void *);
    LONG  (*ReleaseBoundObjects)(BIND_CONTEXT *);
    LONG  (*SetBindOptions)(BIND_CONTEXT *, void *);
    LONG  (*GetBindOptions)(BIND_CONTEXT *, void *);
    LONG  (*GetRunningObjectTable)(BIND_CONTEXT *, void **);
    LONG  (*RegisterObjectParam)(BIND_CONTEXT *, WCHAR *, void *);
    LONG  (*GetObjectParam)(BIND_CONTEXT *, WCHAR *, void **);
    LONG  (*EnumObjectParam)(BIND_CONTEXT *, void **);
    LONG  (*RevokeObjectParam)(BIND_CONTEXT *, WCHAR *);
} BIND_CONTEXT_VTBL;

struct _BIND_CONTEXT {
    const BIND_CONTEXT_VTBL *Vtbl;
    volatile LONG References;
};

static LONG bind_context_query_interface(BIND_CONTEXT *context,
                                          const GUID_K *iid, void **object)
{
    if (!object)
        return (LONG)0x80070057L;
    *object = 0;
    if (!guid_equal(iid, &g_iid_iunknown) &&
        !guid_equal(iid, &g_iid_ibindctx))
        return (LONG)0x80004002L;
    __sync_add_and_fetch(&context->References, 1);
    *object = context;
    return 0;
}

static DWORD bind_context_add_ref(BIND_CONTEXT *context)
{
    return (DWORD)__sync_add_and_fetch(&context->References, 1);
}

static DWORD bind_context_release(BIND_CONTEXT *context)
{
    LONG references = __sync_sub_and_fetch(&context->References, 1);
    if (references < 1) {
        context->References = 1;
        references = 1;
    }
    return (DWORD)references;
}

static LONG bind_context_not_implemented(BIND_CONTEXT *context)
{
    (void)context;
    return (LONG)0x80004001L;
}

static const BIND_CONTEXT_VTBL g_bind_context_vtbl = {
    bind_context_query_interface,
    bind_context_add_ref,
    bind_context_release,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented,
    (void *)bind_context_not_implemented
};
static BIND_CONTEXT g_bind_context = { &g_bind_context_vtbl, 1 };

__declspec(dllexport) LONG CreateBindCtx(DWORD reserved, void **bind_context)
{
    if (reserved || !bind_context)
        return (LONG)0x80070057L;
    *bind_context = 0;
    bind_context_add_ref(&g_bind_context);
    *bind_context = &g_bind_context;
    return 0;
}

typedef struct _ENUM_SHELL_ITEMS ENUM_SHELL_ITEMS;
typedef struct _ENUM_SHELL_ITEMS_VTBL {
    LONG  (*QueryInterface)(ENUM_SHELL_ITEMS *, const GUID_K *, void **);
    DWORD (*AddRef)(ENUM_SHELL_ITEMS *);
    DWORD (*Release)(ENUM_SHELL_ITEMS *);
    LONG  (*Next)(ENUM_SHELL_ITEMS *, DWORD, void **, DWORD *);
    LONG  (*Skip)(ENUM_SHELL_ITEMS *, DWORD);
    LONG  (*Reset)(ENUM_SHELL_ITEMS *);
    LONG  (*Clone)(ENUM_SHELL_ITEMS *, void **);
} ENUM_SHELL_ITEMS_VTBL;

struct _ENUM_SHELL_ITEMS {
    const ENUM_SHELL_ITEMS_VTBL *Vtbl;
    volatile LONG References;
};

static LONG enum_shell_items_query_interface(ENUM_SHELL_ITEMS *enumerator,
                                              const GUID_K *iid, void **object)
{
    if (!object)
        return (LONG)0x80070057L;
    *object = 0;
    if (!guid_equal(iid, &g_iid_iunknown) &&
        !guid_equal(iid, &g_iid_ienum_shell_items))
        return (LONG)0x80004002L;
    __sync_add_and_fetch(&enumerator->References, 1);
    *object = enumerator;
    return 0;
}

static DWORD enum_shell_items_add_ref(ENUM_SHELL_ITEMS *enumerator)
{
    return (DWORD)__sync_add_and_fetch(&enumerator->References, 1);
}

static DWORD enum_shell_items_release(ENUM_SHELL_ITEMS *enumerator)
{
    LONG references = __sync_sub_and_fetch(&enumerator->References, 1);
    if (references < 1) {
        enumerator->References = 1;
        references = 1;
    }
    return (DWORD)references;
}

static LONG enum_shell_items_next(ENUM_SHELL_ITEMS *enumerator, DWORD count,
                                   void **items, DWORD *fetched)
{
    (void)enumerator;
    if (items && count)
        items[0] = 0;
    if (fetched)
        *fetched = 0;
    return 1; /* S_FALSE: the Programs folder is currently empty */
}

static LONG enum_shell_items_skip(ENUM_SHELL_ITEMS *enumerator, DWORD count)
{
    (void)enumerator; (void)count;
    return 1;
}

static LONG enum_shell_items_reset(ENUM_SHELL_ITEMS *enumerator)
{
    (void)enumerator;
    return 0;
}

static LONG enum_shell_items_clone(ENUM_SHELL_ITEMS *enumerator, void **clone)
{
    return enum_shell_items_query_interface(enumerator,
                                             &g_iid_ienum_shell_items, clone);
}

static const ENUM_SHELL_ITEMS_VTBL g_enum_shell_items_vtbl = {
    enum_shell_items_query_interface,
    enum_shell_items_add_ref,
    enum_shell_items_release,
    enum_shell_items_next,
    enum_shell_items_skip,
    enum_shell_items_reset,
    enum_shell_items_clone
};
static ENUM_SHELL_ITEMS g_programs_enumerator = {
    &g_enum_shell_items_vtbl, 1
};

static LONG shell_item_query_interface(SHELL_ITEM *item, const GUID_K *iid,
                                        void **object)
{
    if (!object)
        return (LONG)0x80070057L; /* E_INVALIDARG */
    *object = 0;
    if (!guid_equal(iid, &g_iid_iunknown) &&
        !guid_equal(iid, &g_iid_ishellitem))
        return (LONG)0x80004002L; /* E_NOINTERFACE */
    __sync_add_and_fetch(&item->References, 1);
    *object = item;
    return 0;
}

static DWORD shell_item_add_ref(SHELL_ITEM *item)
{
    return (DWORD)__sync_add_and_fetch(&item->References, 1);
}

static DWORD shell_item_release(SHELL_ITEM *item)
{
    LONG references = __sync_sub_and_fetch(&item->References, 1);
    /* This bootstrap object is static and can be handed out again. */
    if (references < 1) {
        item->References = 1;
        references = 1;
    }
    return (DWORD)references;
}

static LONG shell_item_bind_to_handler(SHELL_ITEM *item, void *bind_context,
                                        const GUID_K *handler,
                                        const GUID_K *iid, void **object)
{
    (void)item; (void)bind_context;
    if (!object)
        return (LONG)0x80070057L;
    *object = 0;
    if (!guid_equal(handler, &g_bhid_enum_items) ||
        !guid_equal(iid, &g_iid_ienum_shell_items))
        return (LONG)0x80004002L;
    return enum_shell_items_query_interface(&g_programs_enumerator, iid,
                                             object);
}

static LONG shell_item_get_parent(SHELL_ITEM *item, void **parent)
{
    (void)item;
    if (parent)
        *parent = 0;
    return (LONG)0x80004001L; /* E_NOTIMPL */
}

static LONG shell_item_get_display_name(SHELL_ITEM *item, DWORD form,
                                         WCHAR **name)
{
    static const WCHAR programs[] = {
        'C',':','\\','P','r','o','g','r','a','m','s',0
    };
    (void)item; (void)form;
    if (!name)
        return (LONG)0x80070057L;
    *name = CoTaskMemAlloc(sizeof(programs));
    if (!*name)
        return (LONG)0x8007000EL; /* E_OUTOFMEMORY */
    memcpy(*name, programs, sizeof(programs));
    return 0;
}

static LONG shell_item_get_attributes(SHELL_ITEM *item, DWORD mask,
                                       DWORD *attributes)
{
    const DWORD supported = 0xF0000000u; /* folder/filesystem capability bits */
    (void)item;
    if (!attributes)
        return (LONG)0x80070057L;
    *attributes = mask & supported;
    return 0;
}

static LONG shell_item_compare(SHELL_ITEM *item, SHELL_ITEM *other,
                                DWORD hint, int *order)
{
    (void)hint;
    if (!order)
        return (LONG)0x80070057L;
    *order = item == other ? 0 : 1;
    return 0;
}

static const SHELL_ITEM_VTBL g_shell_item_vtbl = {
    shell_item_query_interface,
    shell_item_add_ref,
    shell_item_release,
    shell_item_bind_to_handler,
    shell_item_get_parent,
    shell_item_get_display_name,
    shell_item_get_attributes,
    shell_item_compare
};
static SHELL_ITEM g_programs_shell_item = { &g_shell_item_vtbl, 1 };

__declspec(dllexport) LONG SHCreateItemInKnownFolder(
    const void *known_folder_id, DWORD flags, const WCHAR *item_name,
    const void *interface_id, void **object)
{
    (void)flags;
    if (!object)
        return (LONG)0x80070057L;
    *object = 0;
    if (item_name ||
        !guid_equal((const GUID_K *)known_folder_id, &g_folderid_programs))
        return (LONG)0x80070002L; /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
    return shell_item_query_interface(&g_programs_shell_item,
                                      (const GUID_K *)interface_id, object);
}

__declspec(dllexport) LONG PSCreateMemoryPropertyStore(
    const void *interface_id, void **property_store)
{
    (void)interface_id;
    if (property_store)
        *property_store = 0;
    return (LONG)0x80004001L; /* E_NOTIMPL */
}

__declspec(dllexport) LONG CoCreateInstance(const void *class_id,
                                             void *outer, DWORD context,
                                             const void *interface_id,
                                             void **object)
{
    (void)class_id; (void)outer; (void)context; (void)interface_id;
    if (object)
        *object = 0;
    return (LONG)0x80040154L; /* REGDB_E_CLASSNOTREG */
}

/* ------------------------------------------------------------------ */
/* Minimal SHLWAPI registry helper (SHLWAPI is currently API-aliased)  */
/* ------------------------------------------------------------------ */

__declspec(dllexport) LONG SHRegGetValueW(void *key, const WCHAR *subkey,
                                          const WCHAR *value, DWORD flags,
                                          DWORD *type, void *data, DWORD *size)
{
    (void)key; (void)subkey; (void)value; (void)flags; (void)data; (void)size;
    if (type)
        *type = 0;

    /* NTOS starts with no Winlogon/Shell policy values.  Reporting the real
     * Win32 failure lets callers use their built-in defaults; a generic
     * return-zero import stub incorrectly means ERROR_SUCCESS. */
    return 2; /* ERROR_FILE_NOT_FOUND */
}

/* ------------------------------------------------------------------ */
/* Minimal VERSION.dll surface (the loader aliases VERSION to us)      */
/* ------------------------------------------------------------------ */

static BOOL wide_contains_ascii(const WCHAR *wide, const char *ascii)
{
    if (!wide || !ascii)
        return 0;
    for (; *wide; wide++) {
        const WCHAR *w = wide;
        const char *a = ascii;
        while (*a && *w == (WCHAR)(BYTE)*a) {
            w++;
            a++;
        }
        if (!*a)
            return 1;
    }
    return 0;
}

__declspec(dllexport) DWORD GetFileVersionInfoSizeExW(DWORD flags,
                                                       const WCHAR *filename,
                                                       DWORD *handle)
{
    (void)flags; (void)filename;
    if (handle)
        *handle = 0;
    return 64;
}

__declspec(dllexport) BOOL GetFileVersionInfoExW(DWORD flags,
                                                 const WCHAR *filename,
                                                 DWORD handle, DWORD length,
                                                 void *data)
{
    (void)flags; (void)filename; (void)handle;
    if (!data || length < 4) {
        SetLastError(122);
        return 0;
    }
    memset(data, 0, length);
    return 1;
}

__declspec(dllexport) BOOL VerQueryValueW(const void *block,
                                          const WCHAR *subblock,
                                          void **value, unsigned *length)
{
    static DWORD translation = 0x04B00409; /* en-US, Unicode code page */
    static WCHAR internal_name[] = {
        'w','h','e','r','e','.','e','x','e',0
    };
    (void)block;
    if (!subblock || !value || !length)
        return 0;
    if (wide_contains_ascii(subblock, "Translation")) {
        *value = &translation;
        *length = sizeof(translation);
        return 1;
    }
    if (wide_contains_ascii(subblock, "InternalName")) {
        *value = internal_name;
        *length = sizeof(internal_name) / sizeof(internal_name[0]);
        return 1;
    }
    *value = 0;
    *length = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module file name (from PEB->ProcessParameters ImagePathName)        */
/* ------------------------------------------------------------------ */

__declspec(dllexport) DWORD GetModuleFileNameA(HANDLE module, char *buf,
                                               DWORD size)
{
    (void)module;
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    DWORD n = 0;
    if (pp && buf && size) {
        WORD   len = *(WORD *)(pp + 0x60) / 2;  /* ImagePathName.Length (chars) */
        WCHAR *w   = *(WCHAR **)(pp + 0x68);     /* ImagePathName.Buffer          */
        for (; n < len && n < size - 1 && w[n]; n++)
            buf[n] = (char)w[n];
    }
    if (buf && size)
        buf[n] = 0;
    return n;
}

__declspec(dllexport) DWORD GetModuleFileNameW(HANDLE module, WCHAR *buf,
                                               DWORD size)
{
    (void)module;
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (!pp || !buf || !size)
        return 0;

    DWORD len = *(WORD *)(pp + 0x60) / 2;
    WCHAR *src = *(WCHAR **)(pp + 0x68);
    DWORD n = 0;
    while (src && n < len && src[n] && n + 1 < size) {
        buf[n] = src[n];
        n++;
    }
    buf[n] = 0;
    if (n < len) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return size;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Minimal USER32 window-class registry                                */
/* ------------------------------------------------------------------ */

typedef WORD ATOM;
typedef long long LRESULT;
typedef LRESULT (*WNDPROC)(HANDLE window, DWORD message,
                          ULONGLONG wparam, long long lparam);

typedef struct _WNDCLASSEXW {
    DWORD   cbSize;
    DWORD   style;
    WNDPROC lpfnWndProc;
    int     cbClsExtra;
    int     cbWndExtra;
    HANDLE  hInstance;
    HANDLE  hIcon;
    HANDLE  hCursor;
    HANDLE  hbrBackground;
    const WCHAR *lpszMenuName;
    const WCHAR *lpszClassName;
    HANDLE  hIconSm;
} WNDCLASSEXW;

#define USER_MAX_CLASSES 32
#define USER_CLASS_NAME_MAX 128

typedef struct _USER_CLASS {
    BOOL    Used;
    ATOM    Atom;
    DWORD   Style;
    WNDPROC WindowProc;
    int     WindowExtra;
    HANDLE  Instance;
    WCHAR   Name[USER_CLASS_NAME_MAX];
} USER_CLASS;

static USER_CLASS g_user_classes[USER_MAX_CLASSES];

#define USER_MAX_WINDOWS 64
typedef struct _USER_WINDOW {
    BOOL       Used;
    USER_CLASS *Class;
    DWORD      ExStyle;
    DWORD      Style;
    int        X;
    int        Y;
    int        Width;
    int        Height;
    HANDLE     Parent;
    HANDLE     Menu;
    HANDLE     Instance;
    void       *CreateParameter;
    DWORD      Band;
} USER_WINDOW;

static USER_WINDOW g_user_windows[USER_MAX_WINDOWS];
static HANDLE g_user_message_event;

#define USER_MAX_REGISTERED_MESSAGES 64
typedef struct _USER_REGISTERED_MESSAGE {
    BOOL  Used;
    DWORD Id;
    WCHAR Name[USER_CLASS_NAME_MAX];
} USER_REGISTERED_MESSAGE;

static USER_REGISTERED_MESSAGE
    g_user_registered_messages[USER_MAX_REGISTERED_MESSAGES];

typedef struct _USER_CURSOR {
    DWORD ResourceId;
} USER_CURSOR;

static USER_CURSOR g_user_system_cursors[] = {
    { 32512 }, /* IDC_ARROW */
    { 32513 }, /* IDC_IBEAM */
    { 32514 }, /* IDC_WAIT */
    { 32515 }, /* IDC_CROSS */
    { 32516 }, /* IDC_UPARROW */
    { 32642 }, /* IDC_SIZENWSE */
    { 32643 }, /* IDC_SIZENESW */
    { 32644 }, /* IDC_SIZEWE */
    { 32645 }, /* IDC_SIZENS */
    { 32646 }, /* IDC_SIZEALL */
    { 32648 }, /* IDC_NO */
    { 32649 }, /* IDC_HAND */
    { 32650 }, /* IDC_APPSTARTING */
    { 32651 }  /* IDC_HELP */
};

static BOOL user_class_name_equal(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

__declspec(dllexport) ATOM RegisterClassExW(const WNDCLASSEXW *window_class)
{
    if (!window_class || window_class->cbSize < sizeof(WNDCLASSEXW) ||
        !window_class->lpszClassName || !window_class->lpfnWndProc) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (entry->Used && entry->Instance == window_class->hInstance &&
            user_class_name_equal(entry->Name, window_class->lpszClassName)) {
            SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
            return 0;
        }
    }

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (!entry->Used) {
            int n = 0;
            while (window_class->lpszClassName[n] &&
                   n + 1 < USER_CLASS_NAME_MAX) {
                entry->Name[n] = window_class->lpszClassName[n];
                n++;
            }
            entry->Name[n] = 0;
            entry->Atom = (ATOM)(i + 1);
            entry->Style = window_class->style;
            entry->WindowProc = window_class->lpfnWndProc;
            entry->WindowExtra = window_class->cbWndExtra;
            entry->Instance = window_class->hInstance;
            entry->Used = 1;
            SetLastError(0);
            return entry->Atom;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

__declspec(dllexport) DWORD RegisterWindowMessageW(const WCHAR *message_name)
{
    if (!message_name || !*message_name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    for (int i = 0; i < USER_MAX_REGISTERED_MESSAGES; i++) {
        USER_REGISTERED_MESSAGE *entry = &g_user_registered_messages[i];
        if (entry->Used && user_class_name_equal(entry->Name, message_name))
            return entry->Id;
    }

    for (int i = 0; i < USER_MAX_REGISTERED_MESSAGES; i++) {
        USER_REGISTERED_MESSAGE *entry = &g_user_registered_messages[i];
        if (!entry->Used) {
            int n = 0;
            while (message_name[n] && n + 1 < USER_CLASS_NAME_MAX) {
                entry->Name[n] = message_name[n];
                n++;
            }
            entry->Name[n] = 0;
            entry->Id = 0xC000u + (DWORD)i;
            entry->Used = 1;
            SetLastError(0);
            return entry->Id;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

__declspec(dllexport) HANDLE LoadCursorW(HANDLE instance,
                                         const WCHAR *cursor_name)
{
    /* The standard cursor identifiers are MAKEINTRESOURCE values.  NTOS does
     * not draw cursor bitmaps in USER32 yet, but callers still need stable,
     * typed handles while registering their window classes. */
    ULONGLONG raw_name = (ULONGLONG)cursor_name;
    if (!instance && raw_name <= 0xFFFFu) {
        DWORD id = (DWORD)raw_name;
        for (DWORD i = 0;
             i < sizeof(g_user_system_cursors) / sizeof(g_user_system_cursors[0]);
             i++) {
            if (g_user_system_cursors[i].ResourceId == id) {
                SetLastError(0);
                return &g_user_system_cursors[i];
            }
        }
    }

    SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    return 0;
}

static USER_CLASS *user_find_class(const WCHAR *class_name, HANDLE instance)
{
    ULONGLONG raw_name = (ULONGLONG)class_name;
    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        USER_CLASS *entry = &g_user_classes[i];
        if (!entry->Used)
            continue;
        if (raw_name <= 0xFFFFu) {
            if (entry->Atom == (ATOM)raw_name)
                return entry;
        } else if (user_class_name_equal(entry->Name, class_name) &&
                   (!instance || !entry->Instance || entry->Instance == instance)) {
            return entry;
        }
    }
    return 0;
}

__declspec(dllexport) HANDLE CreateWindowInBand(
    DWORD ex_style, const WCHAR *class_name, const WCHAR *window_name,
    DWORD style, int x, int y, int width, int height, HANDLE parent,
    HANDLE menu, HANDLE instance, void *parameter, DWORD band)
{
    (void)window_name;
    USER_CLASS *window_class = user_find_class(class_name, instance);
    if (!window_class) {
        SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }

    for (int i = 0; i < USER_MAX_WINDOWS; i++) {
        USER_WINDOW *window = &g_user_windows[i];
        if (!window->Used) {
            window->Class = window_class;
            window->ExStyle = ex_style;
            window->Style = style;
            window->X = x;
            window->Y = y;
            window->Width = width;
            window->Height = height;
            window->Parent = parent;
            window->Menu = menu;
            window->Instance = instance;
            window->CreateParameter = parameter;
            window->Band = band;
            window->Used = 1;
            SetLastError(0);
            return window;
        }
    }

    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return 0;
}

__declspec(dllexport) BOOL GetMessageW(void *message, HANDLE window,
                                       DWORD minimum, DWORD maximum)
{
    (void)window;
    (void)minimum;
    (void)maximum;
    if (!message) {
        SetLastError(87);
        return -1;
    }

    if (!g_user_message_event) {
        HANDLE event = CreateEventW(0, 0, 0, 0);
        if (!event)
            return -1;
        if (!__sync_bool_compare_and_swap(&g_user_message_event, 0, event))
            CloseHandle(event);
    }

    /* There is no WM_QUIT and no queued input yet.  Waiting is the correct
     * observable behavior: returning zero would falsely tell Explorer to tear
     * down its GUI thread and immediately start another one. */
    for (;;) {
        NTSTATUS status = NtWaitForSingleObject(g_user_message_event, 0, 0);
        if (!NT_SUCCESS(status)) {
            SetLastError(ntstatus_to_win32(status));
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* PE resource directory                                              */
/* ------------------------------------------------------------------ */

typedef struct _RES_DIRECTORY {
    WORD Characteristics;
    WORD TimeDateStamp;
    WORD MajorVersion;
    WORD MinorVersion;
    WORD NumberOfNamedEntries;
    WORD NumberOfIdEntries;
} RES_DIRECTORY;

typedef struct _RES_ENTRY {
    DWORD NameOrId;
    DWORD OffsetToData;
} RES_ENTRY;

typedef struct _RES_DATA_ENTRY {
    DWORD DataRva;
    DWORD Size;
    DWORD CodePage;
    DWORD Reserved;
} RES_DATA_ENTRY;

#define RES_NAME_IS_STRING 0x80000000u
#define RES_SUBDIR         0x80000000u

static BYTE *res_section(BYTE *base)
{
    if (!base)
        base = (BYTE *)NtCurrentPeb()->ImageBaseAddress;
    DWORD pe = *(DWORD *)(base + 0x3C);
    BYTE *opt = base + pe + 24;
    DWORD rva = *(DWORD *)(opt + 112 + 2 * 8);
    return rva ? base + rva : 0;
}

static const RES_ENTRY *res_find_entry(const RES_DIRECTORY *dir,
                                       const void *id)
{
    const RES_ENTRY *entries = (const RES_ENTRY *)(dir + 1);
    WORD total = (WORD)(dir->NumberOfNamedEntries + dir->NumberOfIdEntries);
    BOOL by_name = ((ULONGLONG)id >> 16) != 0;
    for (WORD i = 0; i < total; i++) {
        if (by_name) {
            if (entries[i].NameOrId & RES_NAME_IS_STRING) {
                const WCHAR *p = (const WCHAR *)((const BYTE *)dir +
                    (entries[i].NameOrId & 0x7FFFFFFFu));
                WORD len = *p++;
                const WCHAR *want = (const WCHAR *)id;
                WORD j = 0;
                while (j < len && want[j] && p[j] == want[j])
                    j++;
                if (j == len && !want[j])
                    return &entries[i];
            }
        } else {
            if (!(entries[i].NameOrId & RES_NAME_IS_STRING) &&
                (WORD)entries[i].NameOrId == (WORD)(ULONGLONG)id)
                return &entries[i];
        }
    }
    return 0;
}

static void *res_walk(BYTE *res, const void *type, const void *name,
                      WORD lang)
{
    const RES_ENTRY *e = res_find_entry((const RES_DIRECTORY *)res, type);
    if (!e || !(e->OffsetToData & RES_SUBDIR))
        return 0;
    e = res_find_entry((const RES_DIRECTORY *)(res +
                       (e->OffsetToData & 0x7FFFFFFFu)), name);
    if (!e || !(e->OffsetToData & RES_SUBDIR))
        return 0;
    const RES_DIRECTORY *lang_dir = (const RES_DIRECTORY *)(res +
                                    (e->OffsetToData & 0x7FFFFFFFu));
    const RES_ENTRY *le = (const RES_ENTRY *)(lang_dir + 1);
    WORD total = (WORD)(lang_dir->NumberOfNamedEntries +
                        lang_dir->NumberOfIdEntries);
    for (WORD i = 0; i < total; i++)
        if ((WORD)le[i].NameOrId == lang)
            return res + le[i].OffsetToData;
    return total ? (void *)(res + le[0].OffsetToData) : 0;
}

__declspec(dllexport) HANDLE FindResourceExW(HANDLE module, const WCHAR *type,
                                             const WCHAR *name, WORD lang)
{
    BYTE *res = res_section((BYTE *)module);
    if (!res) {
        SetLastError(1815); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return 0;
    }
    void *found = res_walk(res, type, name, lang);
    if (!found)
        SetLastError(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return (HANDLE)found;
}

__declspec(dllexport) HANDLE FindResourceW(HANDLE module, const WCHAR *name,
                                           const WCHAR *type)
{
    return FindResourceExW(module, type, name, 0);
}

__declspec(dllexport) HANDLE LoadResource(HANDLE module, HANDLE res)
{
    (void)module;
    return res;
}

__declspec(dllexport) void *LockResource(HANDLE res)
{
    return res;
}

__declspec(dllexport) DWORD SizeofResource(HANDLE module, HANDLE res)
{
    (void)module;
    return res ? ((RES_DATA_ENTRY *)res)->Size : 0;
}

__declspec(dllexport) BOOL FreeResource(HANDLE res)
{
    (void)res;
    return 1;
}

typedef BOOL (*ENUMRESLANGPROCW_K)(HANDLE, const WCHAR *, const WCHAR *,
                                   WORD, LONG_PTR);

__declspec(dllexport) BOOL EnumResourceLanguagesW(HANDLE module,
                                                  const WCHAR *type,
                                                  const WCHAR *name,
                                                  ENUMRESLANGPROCW_K callback,
                                                  LONG_PTR param)
{
    BYTE *res = res_section((BYTE *)module);
    if (!res)
        return 0;
    const RES_ENTRY *t = res_find_entry((const RES_DIRECTORY *)res, type);
    if (!t || !(t->OffsetToData & RES_SUBDIR))
        return 0;
    const RES_ENTRY *n = res_find_entry((const RES_DIRECTORY *)(res +
                        (t->OffsetToData & 0x7FFFFFFFu)), name);
    if (!n || !(n->OffsetToData & RES_SUBDIR))
        return 0;
    const RES_DIRECTORY *ld = (const RES_DIRECTORY *)(res +
                              (n->OffsetToData & 0x7FFFFFFFu));
    const RES_ENTRY *le = (const RES_ENTRY *)(ld + 1);
    WORD total = (WORD)(ld->NumberOfNamedEntries + ld->NumberOfIdEntries);
    for (WORD i = 0; i < total; i++)
        if (!callback(module, type, name, (WORD)le[i].NameOrId, param))
            break;
    return total != 0;
}

__declspec(dllexport) int LoadStringW(HANDLE instance, UINT id, WCHAR *buf,
                                      int max_chars)
{
    BYTE *res = res_section((BYTE *)instance);
    if (!res)
        return 0;
    RES_DATA_ENTRY *block = (RES_DATA_ENTRY *)res_walk(res,
        (const void *)(ULONGLONG)(((UINT)id >> 4) + 1),
        (const void *)(ULONGLONG)((UINT)id & 15), 0);
    if (!block)
        return 0;
    BYTE *image = instance ? (BYTE *)instance
                           : (BYTE *)NtCurrentPeb()->ImageBaseAddress;
    const WCHAR *p = (const WCHAR *)(image + block->DataRva);
    WORD count = *p++;
    UINT index = (UINT)id & 15;
    if (index >= count)
        return 0;
    for (UINT i = 0; i < index; i++)
        p += 1 + p[0];
    WORD len = *p++;
    if (!max_chars)
        return len;
    if (!buf)
        return 0;
    int n = len < max_chars - 1 ? len : max_chars - 1;
    for (int i = 0; i < n; i++)
        buf[i] = p[i];
    buf[n] = 0;
    return n;
}

__declspec(dllexport) DWORD CheckForReadOnlyResource(HANDLE module, DWORD flag)
{
    (void)module;
    (void)flag;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Global / Local memory                                               */
/* ------------------------------------------------------------------ */

#define GMEM_ZEROINIT 0x0040u
#define LMEM_ZEROINIT 0x0040u

__declspec(dllexport) void *GlobalAlloc(UINT flags, SIZE_T bytes)
{
    if (!bytes)
        bytes = 1;
    void *p = HeapAlloc(GetProcessHeap(),
                        (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, bytes);
    if (!p)
        SetLastError(8);
    return p;
}

__declspec(dllexport) void *GlobalReAlloc(void *h, SIZE_T bytes, UINT flags)
{
    if (!h)
        return 0;
    if (!bytes)
        bytes = 1;
    void *p = HeapReAlloc(GetProcessHeap(),
                          (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0,
                          h, bytes);
    if (!p)
        SetLastError(8);
    return p;
}

__declspec(dllexport) void *GlobalFree(void *h)
{
    if (h)
        HeapFree(GetProcessHeap(), 0, h);
    return 0;
}

__declspec(dllexport) void *GlobalLock(void *h)
{
    return h;
}

__declspec(dllexport) BOOL GlobalUnlock(void *h)
{
    (void)h;
    return 1;
}

__declspec(dllexport) void *LocalReAlloc(void *h, SIZE_T bytes, UINT flags)
{
    if (!h)
        return 0;
    if (!bytes)
        bytes = 1;
    void *fresh = HeapReAlloc(GetProcessHeap(),
                              (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0,
                              h, bytes);
    if (!fresh)
        SetLastError(8);
    return fresh;
}

__declspec(dllexport) SIZE_T LocalSize(void *h)
{
    (void)h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* String comparison and misc helpers                                  */
/* ------------------------------------------------------------------ */

static int wide_cmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

static WCHAR wide_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
}

static WCHAR wide_upper(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
}

__declspec(dllexport) int lstrcmpW(const WCHAR *a, const WCHAR *b)
{
    return wide_cmp(a, b);
}

__declspec(dllexport) int lstrcmpiW(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR ca = wide_lower(*a++);
        WCHAR cb = wide_lower(*b++);
        if (ca != cb || !ca)
            return (int)ca - (int)cb;
    }
}

__declspec(dllexport) int lstrcmpA(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

__declspec(dllexport) int lstrcmpiA(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
}

__declspec(dllexport) int MulDiv(int a, int b, int c)
{
    if (!c)
        return -1;
    long long rounded = c > 0 ? c / 2 : -(c / 2);
    long long r = ((long long)a * b + rounded) / c;
    return (int)r;
}

/* ------------------------------------------------------------------ */
/* Fiber-local storage                                                 */
/* ------------------------------------------------------------------ */

static void *g_fls_slots[128];
static void *g_fls_callbacks[128];
static DWORD g_fls_taken[128];

__declspec(dllexport) DWORD FlsAlloc(void *callback)
{
    for (DWORD i = 1; i < 128; i++) {
        if (__sync_bool_compare_and_swap(&g_fls_taken[i], 0, 1)) {
            g_fls_callbacks[i] = callback;
            g_fls_slots[i] = 0;
            return i;
        }
    }
    SetLastError(87);
    return 0xFFFFFFFFu;
}

__declspec(dllexport) BOOL FlsFree(DWORD index)
{
    if (index >= 128 || !g_fls_taken[index]) {
        SetLastError(87);
        return 0;
    }
    g_fls_taken[index] = 0;
    g_fls_callbacks[index] = 0;
    g_fls_slots[index] = 0;
    return 1;
}

__declspec(dllexport) void *FlsGetValue(DWORD index)
{
    return index < 128 ? g_fls_slots[index] : 0;
}

__declspec(dllexport) BOOL FlsSetValue(DWORD index, void *value)
{
    if (index >= 128 || !g_fls_taken[index])
        return 0;
    g_fls_slots[index] = value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* System information and code pages                                   */
/* ------------------------------------------------------------------ */

typedef struct _SYSTEM_INFO_K {
    DWORD     dwOemId;
    DWORD     dwPageSize;
    void     *lpMinimumApplicationAddress;
    ULONGLONG lpMaximumApplicationAddress;
    ULONGLONG dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO_K;

__declspec(dllexport) void GetSystemInfo(SYSTEM_INFO_K *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    info->dwOemId = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    info->dwPageSize = 4096;
    info->lpMinimumApplicationAddress = (void *)0x10000;
    info->lpMaximumApplicationAddress = 0x7FFFFFFEFFFFULL;
    info->dwActiveProcessorMask = 1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 586;
    info->dwAllocationGranularity = 65536;
    info->wProcessorLevel = 6;
    info->wProcessorRevision = 1;
}

__declspec(dllexport) void GetNativeSystemInfo(SYSTEM_INFO_K *info)
{
    GetSystemInfo(info);
}

__declspec(dllexport) void GetStartupInfoW(void *info)
{
    if (!info)
        return;
    memset(info, 0, 104);
    *(DWORD *)info = 104;
}

__declspec(dllexport) UINT GetACP(void)
{
    return 1252;
}

__declspec(dllexport) UINT GetOEMCP(void)
{
    return 437;
}

__declspec(dllexport) BOOL IsValidCodePage(UINT code_page)
{
    return code_page == 437 || code_page == 1252 || code_page == 65001;
}

typedef struct _CPINFO_K {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} CPINFO_K;

__declspec(dllexport) BOOL GetCPInfo(UINT code_page, CPINFO_K *out)
{
    if (!out || !IsValidCodePage(code_page)) {
        SetLastError(87);
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->MaxCharSize = code_page == 65001 ? 4 : 1;
    out->DefaultChar[0] = '?';
    return 1;
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

__declspec(dllexport) int LCMapStringW(DWORD locale, DWORD flags,
                                       const WCHAR *src, int src_len,
                                       WCHAR *dst, int dst_len)
{
    (void)locale;
    if (!src || (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE)))
        return 0;
    int n = src_len < 0 ? lstrlenW(src) : src_len;
    int cap = dst_len < 0 ? n : dst_len;
    int m = n < cap ? n : cap;
    if (dst) {
        for (int i = 0; i < m; i++)
            dst[i] = (flags & LCMAP_UPPERCASE) ? wide_upper(src[i])
                                               : wide_lower(src[i]);
    }
    return m;
}

__declspec(dllexport) BOOL GetStringTypeW(DWORD type, const WCHAR *src,
                                          int len, WORD *out)
{
    if (type != 1 || !src || !out)
        return 0;
    if (len < 0)
        len = lstrlenW(src);
    for (int i = 0; i < len; i++) {
        WCHAR c = src[i];
        WORD f;
        if (c >= 'A' && c <= 'Z') f = 0x0101;
        else if (c >= 'a' && c <= 'z') f = 0x0102;
        else if (c >= '0' && c <= '9') f = 0x0104;
        else if (c == ' ' || (c >= 0x09 && c <= 0x0D)) f = 0x0108;
        else if (c < 0x20 || c == 0x7F) f = 0x0020;
        else f = 0x0010;
        out[i] = f;
    }
    return 1;
}

__declspec(dllexport) BOOL GetStringTypeExW(DWORD locale, DWORD type,
                                            const WCHAR *src, int len,
                                            WORD *out)
{
    (void)locale;
    return GetStringTypeW(type, src, len, out);
}

__declspec(dllexport) void OutputDebugStringA(const char *text)
{
    (void)text;
}

/* ------------------------------------------------------------------ */
/* File mapping                                                        */
/* ------------------------------------------------------------------ */

typedef struct _FILE_MAPPING_K {
    void *Base;
    ULONGLONG Size;
    int Used;
} FILE_MAPPING_K;

static FILE_MAPPING_K g_file_mappings[16];

__declspec(dllexport) HANDLE CreateFileMappingW(HANDLE file, void *sa,
                                                DWORD protect,
                                                DWORD max_high, DWORD max_low,
                                                const WCHAR *name)
{
    (void)sa;
    (void)protect;
    (void)name;
    ULONGLONG size = ((ULONGLONG)max_high << 32) | max_low;
    if (!size && file && file != NT_INVALID_HANDLE) {
        DWORD high = 0;
        DWORD low = GetFileSize(file, &high);
        size = ((ULONGLONG)high << 32) | low;
    }
    if (!size) {
        SetLastError(1006); /* ERROR_FILE_INVALID */
        return 0;
    }
    for (int i = 0; i < 16; i++) {
        if (g_file_mappings[i].Used)
            continue;
        void *base = nt_alloc(size);
        if (!base) {
            SetLastError(8);
            return 0;
        }
        if (file && file != NT_INVALID_HANDLE) {
            DWORD high = 0;
            DWORD low = GetFileSize(file, &high);
            ULONGLONG file_size = ((ULONGLONG)high << 32) | low;
            ULONGLONG remaining = file_size < size ? file_size : size;
            SetFilePointerEx(file, 0, 0, 0);
            BYTE *dst = (BYTE *)base;
            while (remaining) {
                DWORD chunk = remaining > 0x10000 ? 0x10000 : (DWORD)remaining;
                DWORD done = 0;
                if (!ReadFile(file, dst, chunk, &done, 0) || !done)
                    break;
                dst += done;
                remaining -= done;
            }
        }
        g_file_mappings[i].Used = 1;
        g_file_mappings[i].Base = base;
        g_file_mappings[i].Size = size;
        return (HANDLE)(ULONGLONG)(i + 1);
    }
    SetLastError(8);
    return 0;
}

__declspec(dllexport) void *MapViewOfFile(HANDLE mapping, DWORD access,
                                          DWORD off_high, DWORD off_low,
                                          SIZE_T bytes)
{
    (void)access;
    UINT index = (UINT)(ULONGLONG)mapping;
    if (!index || index > 16 || !g_file_mappings[index - 1].Used) {
        SetLastError(6);
        return 0;
    }
    FILE_MAPPING_K *m = &g_file_mappings[index - 1];
    ULONGLONG off = ((ULONGLONG)off_high << 32) | off_low;
    if (off > m->Size || (!bytes && off == m->Size)) {
        SetLastError(113); /* ERROR_OFFSET_ALIGNMENT */
        return 0;
    }
    if (!bytes)
        bytes = (SIZE_T)(m->Size - off);
    if (off + bytes > m->Size) {
        SetLastError(113);
        return 0;
    }
    return (BYTE *)m->Base + off;
}

__declspec(dllexport) BOOL UnmapViewOfFile(const void *view)
{
    for (int i = 0; i < 16; i++) {
        if (g_file_mappings[i].Used &&
            (const BYTE *)view >= (const BYTE *)g_file_mappings[i].Base &&
            (const BYTE *)view < (const BYTE *)g_file_mappings[i].Base +
                                 g_file_mappings[i].Size)
            return 1;
    }
    SetLastError(158); /* ERROR_NOT_LOCKED */
    return 0;
}

/* ------------------------------------------------------------------ */
/* File misc                                                           */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL FlushFileBuffers(HANDLE file)
{
    (void)file;
    return 1;
}

__declspec(dllexport) DWORD SetFilePointer(HANDLE file, LONG distance,
                                           LONG *high, DWORD method)
{
    long long d = distance;
    if (high)
        d |= (long long)*(volatile LONG *)high << 32;
    long long position = 0;
    if (!SetFilePointerEx(file, d, &position, method))
        return 0xFFFFFFFFu;
    if (high)
        *high = (LONG)((ULONGLONG)position >> 32);
    return (DWORD)position;
}

__declspec(dllexport) BOOL WritePrivateProfileStringW(const WCHAR *section,
                                                      const WCHAR *key,
                                                      const WCHAR *value,
                                                      const WCHAR *file)
{
    (void)section;
    (void)key;
    (void)value;
    (void)file;
    return 1;
}

__declspec(dllexport) BOOL DeleteFileW(const WCHAR *path)
{
    (void)path;
    SetLastError(5); /* ERROR_ACCESS_DENIED: the FAT driver is read-only */
    return 0;
}

__declspec(dllexport) BOOL MoveFileW(const WCHAR *from, const WCHAR *to)
{
    (void)from;
    (void)to;
    SetLastError(5);
    return 0;
}

__declspec(dllexport) BOOL CopyFileW(const WCHAR *from, const WCHAR *to,
                                     BOOL fail_if_exists)
{
    HANDLE src = CreateFileW(from, 0x80000000u, 1, 0, 3, 0, 0);
    if (src == NT_INVALID_HANDLE)
        return 0;
    DWORD high = 0;
    DWORD low = GetFileSize(src, &high);
    ULONGLONG remaining = ((ULONGLONG)high << 32) | low;
    HANDLE dst = CreateFileW(to, 0x40000000u, 0, 0,
                             fail_if_exists ? 1 : 2, 0, 0);
    if (dst == NT_INVALID_HANDLE) {
        CloseHandle(src);
        return 0;
    }
    BYTE *buffer = nt_alloc(0x10000);
    BOOL ok = buffer != 0;
    while (ok && remaining) {
        DWORD chunk = remaining > 0x10000 ? 0x10000 : (DWORD)remaining;
        DWORD done = 0;
        ok = ReadFile(src, buffer, chunk, &done, 0) && done;
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(dst, buffer, done, &written, 0) &&
                 written == done;
        }
        remaining -= done;
    }
    if (buffer)
        HeapFree(GetProcessHeap(), 0, buffer);
    CloseHandle(src);
    CloseHandle(dst);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Virtual query                                                       */
/* ------------------------------------------------------------------ */

typedef struct _MBI_K {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    DWORD PartitionId;
    ULONGLONG RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    DWORD Reserved;
} MBI_K;

__declspec(dllexport) SIZE_T VirtualQuery(const void *address, MBI_K *buffer,
                                          SIZE_T length)
{
    if (!buffer || length < sizeof(MBI_K))
        return 0;
    memset(buffer, 0, sizeof(*buffer));
    ULONGLONG addr = (ULONGLONG)address & ~0xFFFULL;
    buffer->BaseAddress = (void *)addr;
    buffer->AllocationBase = (void *)addr;
    buffer->AllocationProtect = PAGE_READWRITE;
    buffer->RegionSize = 0x10000;
    if (addr >= 0x10000 && addr < 0x800000000000ULL) {
        buffer->State = 0x1000;  /* MEM_COMMIT */
        buffer->Protect = PAGE_READWRITE;
        buffer->Type = 0x20000;  /* MEM_PRIVATE */
    } else {
        buffer->State = 0x10000; /* MEM_FREE */
        buffer->Protect = 0x01;
    }
    return sizeof(MBI_K);
}

/* ------------------------------------------------------------------ */
/* Machine, power, packages, timer queue                               */
/* ------------------------------------------------------------------ */

__declspec(dllexport) BOOL GetComputerNameW(WCHAR *buffer, DWORD *size)
{
    static const WCHAR name[] = { 'N', 'T', 'O', 'S', 0 };
    if (!size) {
        SetLastError(87);
        return 0;
    }
    if (!buffer || *size < 5) {
        *size = 5;
        SetLastError(122); /* ERROR_BUFFER_OVERFLOW */
        return 0;
    }
    for (int i = 0; i <= 4; i++)
        buffer[i] = name[i];
    *size = 4;
    return 1;
}

__declspec(dllexport) DWORD RegisterApplicationRestart(const WCHAR *cmdline,
                                                       DWORD flags)
{
    (void)cmdline;
    (void)flags;
    return 0;
}

__declspec(dllexport) BOOL SetTermsrvAppInstallMode(BOOL mode)
{
    (void)mode;
    return 1;
}

__declspec(dllexport) HANDLE PowerCreateRequest(void *context)
{
    (void)context;
    return nt_alloc(64);
}

__declspec(dllexport) BOOL PowerSetRequest(HANDLE request, DWORD type)
{
    (void)request;
    (void)type;
    return 1;
}

__declspec(dllexport) DWORD GetPackageFullName(HANDLE process, DWORD *length,
                                               WCHAR *name)
{
    (void)process;
    (void)name;
    if (length)
        *length = 0;
    return 15700; /* APPMODEL_ERROR_NO_PACKAGE */
}

__declspec(dllexport) DWORD ParseApplicationUserModelId(
    const WCHAR *package_id, WCHAR **package, WCHAR **app)
{
    (void)package_id;
    (void)package;
    (void)app;
    return 15700;
}

__declspec(dllexport) LONG FindPackagesByPackageFamily(
    const WCHAR *family, DWORD pin_options, DWORD *count, void **tokens,
    UINT *total)
{
    (void)family;
    (void)pin_options;
    (void)tokens;
    if (count)
        *count = 0;
    if (total)
        *total = 0;
    return 15700;
}

__declspec(dllexport) LONG GetPackagesByPackageFamily(const WCHAR *family,
                                                      UINT *count, void **tokens,
                                                      UINT *total)
{
    (void)family;
    (void)tokens;
    if (count)
        *count = 0;
    if (total)
        *total = 0;
    return 15700;
}

__declspec(dllexport) BOOL CheckElevationEnabled(DWORD *enabled)
{
    if (enabled)
        *enabled = 0;
    return 1;
}

typedef void (*WAITORTIMERCALLBACK_K)(void *param, int fired);

typedef struct _TIMERQ_K {
    WAITORTIMERCALLBACK_K Callback;
    void *Param;
    DWORD Due;
    DWORD Period;
    volatile LONG Stop;
    HANDLE Thread;
} TIMERQ_K;

static DWORD WINAPI timerq_thread(LPVOID argument)
{
    TIMERQ_K *timer = (TIMERQ_K *)argument;
    if (timer->Due)
        Sleep(timer->Due);
    while (!timer->Stop) {
        timer->Callback(timer->Param, 0);
        if (!timer->Period)
            break;
        Sleep(timer->Period);
    }
    timer->Callback(timer->Param, 1);
    return 0;
}

__declspec(dllexport) BOOL CreateTimerQueueTimer(PHANDLE *out, HANDLE queue,
                                                 WAITORTIMERCALLBACK_K callback,
                                                 void *param, DWORD due,
                                                 DWORD period, DWORD flags)
{
    (void)queue;
    (void)flags;
    if (!out || !callback)
        return 0;
    TIMERQ_K *timer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*timer));
    if (!timer)
        return 0;
    timer->Callback = callback;
    timer->Param = param;
    timer->Due = due;
    timer->Period = period;
    timer->Thread = CreateThread(0, 0, timerq_thread, timer, 0, 0);
    if (!timer->Thread) {
        HeapFree(GetProcessHeap(), 0, timer);
        return 0;
    }
    *out = (PHANDLE)timer;
    return 1;
}

__declspec(dllexport) BOOL DeleteTimerQueueTimer(HANDLE queue, HANDLE timer,
                                                 HANDLE event)
{
    (void)queue;
    TIMERQ_K *t = (TIMERQ_K *)timer;
    if (!t)
        return 0;
    __sync_fetch_and_or(&t->Stop, 1);
    if (event == (HANDLE)(ULONGLONG)-1)
        WaitForSingleObject(t->Thread, 0xFFFFFFFFu);
    return 1;
}

__declspec(dllexport) BOOL ChangeTimerQueueTimer(HANDLE queue, HANDLE timer,
                                                 DWORD due, DWORD period)
{
    (void)queue;
    TIMERQ_K *t = (TIMERQ_K *)timer;
    if (!t)
        return 0;
    t->Due = due;
    t->Period = period;
    return 1;
}

typedef struct _WAITQ_K {
    WAITORTIMERCALLBACK_K Callback;
    void *Param;
    HANDLE Wait;
    DWORD Timeout;
    volatile LONG Stop;
} WAITQ_K;

static DWORD WINAPI waitq_thread(LPVOID argument)
{
    WAITQ_K *waiter = (WAITQ_K *)argument;
    DWORD status = WaitForSingleObject(waiter->Wait, waiter->Timeout);
    if (!waiter->Stop)
        waiter->Callback(waiter->Param, status != 0);
    return 0;
}

__declspec(dllexport) BOOL RegisterWaitForSingleObject(PHANDLE *out,
                                                       HANDLE object,
                                                       WAITORTIMERCALLBACK_K callback,
                                                       void *param,
                                                       DWORD timeout,
                                                       DWORD flags)
{
    (void)flags;
    if (!out || !callback)
        return 0;
    WAITQ_K *waiter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*waiter));
    if (!waiter)
        return 0;
    waiter->Callback = callback;
    waiter->Param = param;
    waiter->Wait = object;
    waiter->Timeout = timeout;
    HANDLE thread = CreateThread(0, 0, waitq_thread, waiter, 0, 0);
    if (!thread) {
        HeapFree(GetProcessHeap(), 0, waiter);
        return 0;
    }
    *out = (PHANDLE)waiter;
    return 1;
}

__declspec(dllexport) BOOL UnregisterWaitEx(HANDLE wait, HANDLE event)
{
    (void)event;
    WAITQ_K *w = (WAITQ_K *)wait;
    if (!w)
        return 0;
    __sync_fetch_and_or(&w->Stop, 1);
    return 1;
}

__declspec(dllexport) BOOL DisableThreadLibraryCalls(HANDLE module)
{
    (void)module;
    return 1;
}

__declspec(dllexport) BOOL SetStdHandle(DWORD which, HANDLE handle)
{
    PEB *peb = NtCurrentPeb();
    unsigned char *pp = (unsigned char *)peb->ProcessParameters;
    if (!pp) {
        SetLastError(87);
        return 0;
    }
    DWORD offset = which == (DWORD)-10 ? 0x20 :
                   which == (DWORD)-11 ? 0x28 :
                   which == (DWORD)-12 ? 0x30 : 0;
    if (!offset) {
        SetLastError(87);
        return 0;
    }
    *(HANDLE *)(pp + offset) = handle;
    return 1;
}
