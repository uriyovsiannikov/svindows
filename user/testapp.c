/*
 * user/testapp.c - a normal Win32 console program for NTOS.
 *
 * It uses only the Win32 API (kernel32) - no raw system calls - just as a
 * program written for Windows would. It reads a file, prints to the console,
 * spawns a worker thread, and waits for it, then exits. Built for the x86-64
 * Windows target and linked against kernel32's import library.
 */

typedef void         *HANDLE;
typedef void         *LPVOID;
typedef unsigned long DWORD;
typedef int           BOOL;

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INFINITE          0xFFFFFFFF

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL   ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) HANDLE CreateFileA(const char *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) HANDLE CreateThread(LPVOID, unsigned long long, LPVOID,
                                          LPVOID, DWORD, DWORD *);
__declspec(dllimport) DWORD  WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL   CloseHandle(HANDLE);
__declspec(dllimport) void   ExitProcess(DWORD);

static HANDLE g_out;

static DWORD str_len(const char *s)
{
    DWORD n = 0;
    while (s[n])
        n++;
    return n;
}

static void print(const char *s)
{
    DWORD written;
    WriteFile(g_out, s, str_len(s), &written, 0);
}

static DWORD WorkerThread(LPVOID param)
{
    (void)param;
    print("  [worker] hello from a Win32 thread (CreateThread)\n");
    return 0;
}

void Start(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    print("Win32 program: app.exe -> kernel32.dll -> ntdll.dll -> syscall\n");

    /* Read a file through the Win32 file API. */
    HANDLE file = CreateFileA("message.txt", 0, 0, 0, 0, 0, 0);
    char buffer[256];
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, 0);
    buffer[read] = 0;
    print(buffer);
    CloseHandle(file);

    /* Spawn a worker thread and wait for it (CreateThread/WaitForSingleObject). */
    HANDLE thread = CreateThread(0, 0, (LPVOID)WorkerThread, 0, 0, 0);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    print("main: worker finished; exiting via ExitProcess\n");

    ExitProcess(0);
}
