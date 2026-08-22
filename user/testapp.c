/*
 * user/testapp.c - a normal Win32 console program for NTOS.
 *
 * It uses only the Win32 API (kernel32) - no raw system calls - just as a
 * program written for Windows would. It reads a file, prints to the console,
 * spawns a worker thread, and waits for it, then exits. Built for the x86-64
 * Windows target and linked against kernel32's import library.
 */

typedef void              *HANDLE;
typedef void              *LPVOID;
typedef unsigned char      BYTE;
typedef unsigned long      DWORD;
typedef unsigned long long ULONGLONG;
typedef unsigned long long SIZE_T;
typedef int                BOOL;
typedef long               LONG;
typedef unsigned short     WCHAR;
typedef void             (*FARPROC)(void);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INFINITE          0xFFFFFFFF
#define WAIT_OBJECT_0     0
#define HEAP_ZERO_MEMORY  0x00000008

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) BOOL   ReadFile(HANDLE, void *, DWORD, DWORD *, LPVOID);
__declspec(dllimport) HANDLE CreateFileA(const char *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, LPVOID,
                                         DWORD, DWORD, HANDLE);
__declspec(dllimport) DWORD  GetFileSize(HANDLE, DWORD *);
typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    struct { DWORD Low, High; } CreationTime, LastAccessTime, LastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh, nFileSizeLow, nNumberOfLinks;
    DWORD nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;
typedef struct _SYSTEMTIME {
    unsigned short Year, Month, DayOfWeek, Day;
    unsigned short Hour, Minute, Second, Milliseconds;
} SYSTEMTIME;
__declspec(dllimport) BOOL GetFileInformationByHandle(
    HANDLE, BY_HANDLE_FILE_INFORMATION *);
__declspec(dllimport) BOOL FileTimeToSystemTime(const void *, SYSTEMTIME *);
__declspec(dllimport) BOOL SetFilePointerEx(HANDLE, long long,
                                            long long *, DWORD);
__declspec(dllimport) HANDLE CreateThread(LPVOID, unsigned long long, LPVOID,
                                          LPVOID, DWORD, DWORD *);
__declspec(dllimport) DWORD  WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) DWORD  WaitForMultipleObjects(DWORD, const HANDLE *,
                                                    BOOL, DWORD);
__declspec(dllimport) HANDLE CreateEventW(LPVOID, BOOL, BOOL, const WCHAR *);
__declspec(dllimport) BOOL   SetEvent(HANDLE);
__declspec(dllimport) BOOL   ResetEvent(HANDLE);
__declspec(dllimport) HANDLE CreateSemaphoreW(LPVOID, LONG, LONG,
                                               const WCHAR *);
__declspec(dllimport) BOOL   ReleaseSemaphore(HANDLE, LONG, LONG *);
__declspec(dllimport) void  *CreateThreadpoolWork(LPVOID, LPVOID, LPVOID);
__declspec(dllimport) void   SubmitThreadpoolWork(LPVOID);
__declspec(dllimport) BOOL   CloseHandle(HANDLE);
__declspec(dllimport) void   ExitProcess(DWORD);

/* The dynamic runtime we're exercising here. */
__declspec(dllimport) HANDLE  GetModuleHandleA(const char *name);
__declspec(dllimport) FARPROC GetProcAddress(HANDLE module, const char *name);
__declspec(dllimport) HANDLE  GetProcessHeap(void);
__declspec(dllimport) LPVOID  HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
__declspec(dllimport) BOOL    HeapFree(HANDLE heap, DWORD flags, LPVOID ptr);
__declspec(dllimport) HANDLE  LoadLibraryA(const char *name);

/* Registry (advapi32). */
typedef void *HKEY;
#define HKEY_CURRENT_USER  ((HKEY)(ULONGLONG)0x80000001ULL)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONGLONG)0x80000002ULL)
#define REG_SZ     1
#define REG_DWORD  4
#define KEY_READ   0x20019

__declspec(dllimport) LONG RegOpenKeyExA(HKEY, const char *, DWORD, DWORD, HKEY *);
__declspec(dllimport) LONG RegCreateKeyExA(HKEY, const char *, DWORD, char *, DWORD,
                                           DWORD, void *, HKEY *, DWORD *);
__declspec(dllimport) LONG RegSetValueExA(HKEY, const char *, DWORD, DWORD,
                                          const BYTE *, DWORD);
__declspec(dllimport) LONG RegQueryValueExA(HKEY, const char *, DWORD *, DWORD *,
                                            BYTE *, DWORD *);
__declspec(dllimport) LONG RegCloseKey(HKEY);

/* More Win32 (kernel32). */
__declspec(dllimport) DWORD GetTickCount(void);
__declspec(dllimport) void  Sleep(DWORD ms);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) int   wsprintfA(char *out, const char *fmt, ...);

/* Mini-CRT (msvcrt). */
__declspec(dllimport) int   printf(const char *fmt, ...);
__declspec(dllimport) void *malloc(SIZE_T n);
__declspec(dllimport) void  free(void *p);

/* Wider Win32 surface (kernel32). */
typedef struct { BYTE opaque[40]; } CRITICAL_SECTION;
#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define PAGE_READONLY  0x02
#define PAGE_READWRITE 0x04

__declspec(dllimport) DWORD  GetCurrentProcessId(void);
__declspec(dllimport) DWORD  GetCurrentThreadId(void);
__declspec(dllimport) DWORD  GetLastError(void);
__declspec(dllimport) void   SetLastError(DWORD);
__declspec(dllimport) DWORD  GetModuleFileNameA(HANDLE, char *, DWORD);
__declspec(dllimport) LPVOID VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
__declspec(dllimport) BOOL   VirtualProtect(LPVOID, SIZE_T, DWORD, DWORD *);
__declspec(dllimport) BOOL   QueryPerformanceCounter(long long *);
__declspec(dllimport) BOOL   QueryPerformanceFrequency(long long *);
__declspec(dllimport) LONG   InterlockedIncrement(LONG volatile *);
__declspec(dllimport) void   InitializeCriticalSection(void *);
__declspec(dllimport) void   EnterCriticalSection(void *);
__declspec(dllimport) void   LeaveCriticalSection(void *);
__declspec(dllimport) void   DeleteCriticalSection(void *);
__declspec(dllimport) DWORD  TlsAlloc(void);
__declspec(dllimport) BOOL   TlsFree(DWORD);
__declspec(dllimport) LPVOID TlsGetValue(DWORD);
__declspec(dllimport) BOOL   TlsSetValue(DWORD, LPVOID);

/* Imported from kernel32 via a mismatched import lib -- the real kernel32.dll
 * doesn't export this, so the loader stubs it (proves load-past-missing-import). */
__declspec(dllimport) int    NonexistentKernel32Function(void);

static HANDLE g_out;
static DWORD  g_tls_index = (DWORD)-1;
static DWORD  g_worker_tid;
static BOOL   g_worker_tls_ok;
static HANDLE g_pool_done;
static volatile LONG g_pool_calls;

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

static void print_hex(ULONGLONG v)
{
    char t[19];
    t[0] = '0'; t[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
        t[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    t[18] = 0;
    print(t);
}

static void print_ptr(const char *label, ULONGLONG v)
{
    print(label);
    print_hex(v);
    print("\n");
}

static void print_dec(DWORD v)
{
    char t[11];
    int i = 10;
    t[10] = 0;
    if (v == 0) {
        print("0");
        return;
    }
    while (v) {
        t[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    print(&t[i]);
}

/* Type of the GetProcessHeap we resolve dynamically. */
typedef HANDLE (*GetProcessHeap_t)(void);

/* Show the dynamic runtime working: look a module up by name, resolve a
 * function from it by name, call it, then use the process heap. */
static void demo_dynamic_runtime(void)
{
    print("\n-- dynamic runtime (GetModuleHandle / GetProcAddress / heap) --\n");

    HANDLE k32 = GetModuleHandleA("kernel32.dll");
    print_ptr("  GetModuleHandleA(\"kernel32.dll\") = ", (ULONGLONG)k32);

    /* Resolve GetProcessHeap by name and call the resolved pointer. */
    GetProcessHeap_t pGetProcessHeap =
        (GetProcessHeap_t)GetProcAddress(k32, "GetProcessHeap");
    print_ptr("  GetProcAddress(k32, \"GetProcessHeap\") = ",
              (ULONGLONG)(void *)pGetProcessHeap);

    HANDLE heap = pGetProcessHeap();
    print_ptr("  heap = ", (ULONGLONG)heap);

    char *buf = (char *)HeapAlloc(heap, HEAP_ZERO_MEMORY, 64);
    print_ptr("  HeapAlloc(heap, 64) = ", (ULONGLONG)buf);

    const char *msg = "  heap buffer holds: dynamically allocated memory works!\n";
    DWORD i = 0;
    for (; msg[i]; i++)
        buf[i] = msg[i];
    buf[i] = 0;
    print(buf);

    HeapFree(heap, 0, buf);
    print("  HeapFree ok\n");
}

/* Types of the functions we resolve out of the runtime-loaded extra.dll. */
typedef DWORD       (*ExtraAddNumbers_t)(DWORD, DWORD);
typedef const char *(*ExtraGreeting_t)(void);

/* Load a DLL that isn't in our import chain, resolve its exports, and call
 * them — the way a Windows program loads a plugin. */
static void demo_loadlibrary(void)
{
    print("\n-- LoadLibraryA: load extra.dll at runtime --\n");

    HANDLE extra = LoadLibraryA("extra.dll");
    print_ptr("  LoadLibraryA(\"extra.dll\") = ", (ULONGLONG)extra);
    if (!extra) {
        print("  load failed\n");
        return;
    }

    ExtraGreeting_t greet =
        (ExtraGreeting_t)GetProcAddress(extra, "ExtraGreeting");
    ExtraAddNumbers_t add =
        (ExtraAddNumbers_t)GetProcAddress(extra, "ExtraAddNumbers");

    if (greet)
        print(greet());
    if (add)
        print_ptr("  ExtraAddNumbers(40, 2) = ", (ULONGLONG)add(40, 2));
}

/* Read a preset registry value, then create a key, write a value, and read it
 * back — the classic Reg* flow, over our in-kernel registry. */
static void demo_registry(void)
{
    print("\n-- registry (advapi32 Reg* over Nt*Key) --\n");

    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NTOS", 0, KEY_READ, &hk) == 0) {
        char ver[64];
        DWORD sz = sizeof(ver), type = 0;
        if (RegQueryValueExA(hk, "Version", 0, &type, (BYTE *)ver, &sz) == 0) {
            print("  HKLM\\Software\\NTOS\\Version = ");
            print(ver);
            print("\n");
        }
        DWORD build = 0, bsz = sizeof(build);
        if (RegQueryValueExA(hk, "BuildNumber", 0, &type, (BYTE *)&build, &bsz) == 0) {
            print("  HKLM\\Software\\NTOS\\BuildNumber = ");
            print_dec(build);
            print("\n");
        }
        RegCloseKey(hk);
    } else {
        print("  (could not open HKLM\\Software\\NTOS)\n");
    }

    HKEY hk2;
    DWORD disp;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MyApp", 0, 0, 0, 0, 0,
                        &hk2, &disp) == 0) {
        RegSetValueExA(hk2, "Greeting", 0, REG_SZ,
                       (const BYTE *)"hello from the registry", 24);
        char got[64];
        DWORD gsz = sizeof(got);
        if (RegQueryValueExA(hk2, "Greeting", 0, 0, (BYTE *)got, &gsz) == 0) {
            print("  wrote + read HKCU\\Software\\MyApp\\Greeting = ");
            print(got);
            print("\n");
        }
        RegCloseKey(hk2);
    }
}

/* Exercise the broader Win32 surface and the mini-CRT. */
static void demo_win32_crt(void)
{
    print("\n-- more Win32 + CRT (tick count, Sleep, cmdline, printf) --\n");

    char line[160];
    wsprintfA(line, "  GetCommandLineA() = %s\n", GetCommandLineA());
    print(line);

    DWORD t0 = GetTickCount();
    Sleep(50);
    DWORD t1 = GetTickCount();
    wsprintfA(line, "  Sleep(50): GetTickCount %u -> %u (%u ms elapsed)\n",
              t0, t1, t1 - t0);
    print(line);

    /* msvcrt: printf and malloc/free. */
    printf("  msvcrt printf: 2+2=%d, hex=%x, str=%s\n", 2 + 2, 255, "works");
    int *arr = (int *)malloc(4 * sizeof(int));
    if (arr) {
        for (int i = 0; i < 4; i++)
            arr[i] = i * i;
        printf("  msvcrt malloc[4] = %d %d %d %d\n",
               arr[0], arr[1], arr[2], arr[3]);
        free(arr);
    }
}

/* Exercise the wider Win32 surface real programs lean on. */
static void demo_syswin(void)
{
    char line[160];
    print("\n-- wider Win32 (ids, VirtualAlloc/Protect, QPC, interlocked, crit) --\n");

    wsprintfA(line, "  GetCurrentProcessId=%u GetCurrentThreadId=%u\n",
              GetCurrentProcessId(), GetCurrentThreadId());
    print(line);

    char path[128];
    GetModuleFileNameA(0, path, sizeof(path));
    wsprintfA(line, "  GetModuleFileNameA -> %s\n", path);
    print(line);

    SetLastError(123);
    wsprintfA(line, "  SetLastError(123); GetLastError=%u\n", GetLastError());
    print(line);

    /* VirtualAlloc a page, write it, flip it read-only, flip it back. */
    int *mem = (int *)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD oldp = 0;
    if (mem) {
        mem[0] = 0xCAFE;
        BOOL ro = VirtualProtect(mem, 4096, PAGE_READONLY, &oldp);
        VirtualProtect(mem, 4096, PAGE_READWRITE, &oldp);
        wsprintfA(line, "  VirtualAlloc=%p mem[0]=0x%x VirtualProtect=%d\n",
                  (LPVOID)mem, mem[0], ro);
        print(line);
    }

    /* Time a Sleep with the performance counter. */
    long long freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    Sleep(20);
    QueryPerformanceCounter(&t1);
    wsprintfA(line, "  QPC: freq=%d, Sleep(20) took ~%d ms\n",
              (int)freq, (int)((t1 - t0) * 1000 / freq));
    print(line);

    /* Interlocked counter under a critical section. */
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    LONG counter = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < 5; i++)
        InterlockedIncrement(&counter);
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);
    wsprintfA(line, "  InterlockedIncrement x5 under a critical section = %d\n",
              (int)counter);
    print(line);

    /* Call an import the real kernel32 doesn't export: the loader stubbed it,
     * so this returns 0 rather than having failed the whole load. */
    int stubbed = NonexistentKernel32Function();
    wsprintfA(line, "  NonexistentKernel32Function() -> %d (loader stub)\n",
              stubbed);
    print(line);
}

static DWORD WorkerThread(LPVOID param)
{
    (void)param;
    g_worker_tid = GetCurrentThreadId();
    g_worker_tls_ok = TlsGetValue(g_tls_index) == 0 &&
                      GetLastError() == 0 &&
                      TlsSetValue(g_tls_index, (LPVOID)(ULONGLONG)0x2222) &&
                      TlsGetValue(g_tls_index) == (LPVOID)(ULONGLONG)0x2222;
    print("  [worker] hello from a Win32 thread (CreateThread)\n");
    return 0;
}

static void PoolCallback(void *instance, void *context, void *work)
{
    (void)instance; (void)context; (void)work;
    InterlockedIncrement(&g_pool_calls);
    SetEvent(g_pool_done);
}

int main(void)
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

    /* Verify that an ordinary UTF-16 Win32 path reaches NtCreateFile too. */
    static const WCHAR wide_name[] = {
        'C',':','\\','M','E','S','S','A','G','E','.','T','X','T',0
    };
    file = CreateFileW(wide_name, 0, 0, 0, 3, 0, 0); /* OPEN_EXISTING */
    read = 0;
    DWORD size_high = 0;
    DWORD size_low = file != (HANDLE)(ULONGLONG)-1
                         ? GetFileSize(file, &size_high) : (DWORD)-1;
    BY_HANDLE_FILE_INFORMATION file_info;
    BOOL have_info = file != (HANDLE)(ULONGLONG)-1 &&
                     GetFileInformationByHandle(file, &file_info);
    SYSTEMTIME write_time;
    BOOL have_time = have_info && FileTimeToSystemTime(
        &file_info.LastWriteTime, &write_time);
    long long new_position = -1;
    BOOL seek_ok = file != (HANDLE)(ULONGLONG)-1 &&
                   SetFilePointerEx(file, 5, &new_position, 0);
    if (file != (HANDLE)(ULONGLONG)-1 && size_low == 213 && !size_high &&
        have_info && file_info.nFileSizeLow == 213 &&
        !file_info.nFileSizeHigh && file_info.dwFileAttributes == 0x20 &&
        have_time && write_time.Year >= 2020 &&
        write_time.Month >= 1 && write_time.Month <= 12 &&
        write_time.Day >= 1 && write_time.Day <= 31 &&
        seek_ok && new_position == 5 &&
        ReadFile(file, buffer, 4, &read, 0) && read == 4 &&
        buffer[0] == 'l' && buffer[1] == 'i' &&
        buffer[2] == 'n' && buffer[3] == 'e') {
        print("[ok] wide file + metadata + FAT time + seek\n");
        CloseHandle(file);
    } else {
        print("[fail] CreateFileW\n");
    }

    /* Spawn a worker thread and verify both its TEB id and per-thread TLS. */
    DWORD main_tid = GetCurrentThreadId();
    g_tls_index = TlsAlloc();
    BOOL main_tls_ok = g_tls_index != (DWORD)-1 &&
                       TlsSetValue(g_tls_index, (LPVOID)(ULONGLONG)0x1111);
    HANDLE thread = CreateThread(0, 0, (LPVOID)WorkerThread, 0, 0, 0);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    print("main: worker finished\n");
    if (main_tls_ok && g_worker_tls_ok && g_worker_tid != 0 &&
        g_worker_tid != main_tid &&
        TlsGetValue(g_tls_index) == (LPVOID)(ULONGLONG)0x1111) {
        print("[ok] distinct child TEB id + thread-local Tls* storage\n");
    } else {
        print("[fail] thread id / TLS isolation\n");
    }
    TlsFree(g_tls_index);

    /* Dispatcher regression: wait-any index, signal consumption, wait-all,
     * and a real timer-driven timeout over multiple kernel objects. */
    HANDLE events[2];
    events[0] = CreateEventW(0, 0, 0, 0); /* auto-reset, clear */
    events[1] = CreateEventW(0, 0, 1, 0); /* auto-reset, signaled */
    DWORD any = WaitForMultipleObjects(2, events, 0, 0);
    DWORD consumed = WaitForMultipleObjects(2, events, 0, 0);
    DWORD wait_start = GetTickCount();
    DWORD timed = WaitForMultipleObjects(2, events, 0, 30);
    DWORD wait_elapsed = GetTickCount() - wait_start;
    CloseHandle(events[0]);
    CloseHandle(events[1]);

    events[0] = CreateEventW(0, 1, 1, 0); /* manual-reset, signaled */
    events[1] = CreateEventW(0, 1, 1, 0);
    DWORD all = WaitForMultipleObjects(2, events, 1, 0);
    CloseHandle(events[0]);
    CloseHandle(events[1]);
    if (any == WAIT_OBJECT_0 + 1 && consumed == 0x102 &&
        timed == 0x102 && wait_elapsed >= 30 && all == WAIT_OBJECT_0)
        print("[ok] wait-any/all + poll + dispatcher timeout\n");
    else
        print("[fail] multiple-object wait / timeout\n");

    HANDLE sync_event = CreateEventW(0, 1, 1, 0);
    BOOL reset_ok = ResetEvent(sync_event) &&
                    WaitForSingleObject(sync_event, 0) == 0x102;
    BOOL set_ok = SetEvent(sync_event) &&
                  WaitForSingleObject(sync_event, 0) == WAIT_OBJECT_0;
    CloseHandle(sync_event);
    HANDLE semaphore = CreateSemaphoreW(0, 0, 3, 0);
    LONG previous = -1;
    BOOL release_ok = ReleaseSemaphore(semaphore, 2, &previous);
    DWORD sem1 = WaitForSingleObject(semaphore, 0);
    DWORD sem2 = WaitForSingleObject(semaphore, 0);
    DWORD sem3 = WaitForSingleObject(semaphore, 0);
    CloseHandle(semaphore);
    if (reset_ok && set_ok && release_ok && previous == 0 &&
        sem1 == WAIT_OBJECT_0 && sem2 == WAIT_OBJECT_0 && sem3 == 0x102)
        print("[ok] event set/reset + counting semaphore\n");
    else
        print("[fail] event / semaphore semantics\n");

    g_pool_done = CreateEventW(0, 0, 0, 0);
    void *pool_work = CreateThreadpoolWork((LPVOID)PoolCallback, 0, 0);
    SubmitThreadpoolWork(pool_work);
    DWORD pool_wait = WaitForSingleObject(g_pool_done, 1000);
    if (pool_wait == WAIT_OBJECT_0 && g_pool_calls == 1)
        print("[ok] persistent thread-pool worker + semaphore wake\n");
    else
        print("[fail] thread-pool work dispatch\n");
    CloseHandle(g_pool_done);

    /* Exercise dynamic module/symbol resolution and the heap. */
    demo_dynamic_runtime();

    /* Load a DLL at runtime and call into it. */
    demo_loadlibrary();

    /* Read and write the registry. */
    demo_registry();

    /* Broader Win32 surface + the mini-CRT. */
    demo_win32_crt();

    /* The wider Win32 surface real programs rely on. */
    demo_syswin();

    /* Return through the CRT startup, which calls ExitProcess for us. */
    print("main: returning 0 (CRT startup will ExitProcess)\n");
    return 0;
}
