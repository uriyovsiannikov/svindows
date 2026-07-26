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
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef int                BOOL;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define WAIT_OBJECT_0     0

/* Native services imported from ntdll. */
extern HANDLE    NtCreateFile(const char *name);
extern ULONGLONG NtWriteFile(HANDLE h, const void *buf, ULONGLONG len);
extern ULONGLONG NtReadFile(HANDLE h, void *buf, ULONGLONG len);
extern long      NtClose(HANDLE h);
extern HANDLE    NtCreateThread(LPVOID entry, LPVOID arg);
extern long      NtWaitForSingleObject(HANDLE h);
extern void      NtTerminateThread(void);
extern ULONGLONG NtAllocateVirtualMemory(ULONGLONG size);

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
