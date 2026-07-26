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
typedef void             (*FARPROC)(void);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INFINITE          0xFFFFFFFF
#define HEAP_ZERO_MEMORY  0x00000008

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

/* The dynamic runtime we're exercising here. */
__declspec(dllimport) HANDLE  GetModuleHandleA(const char *name);
__declspec(dllimport) FARPROC GetProcAddress(HANDLE module, const char *name);
__declspec(dllimport) HANDLE  GetProcessHeap(void);
__declspec(dllimport) LPVOID  HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
__declspec(dllimport) BOOL    HeapFree(HANDLE heap, DWORD flags, LPVOID ptr);
__declspec(dllimport) HANDLE  LoadLibraryA(const char *name);

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
    print("main: worker finished\n");

    /* Exercise dynamic module/symbol resolution and the heap. */
    demo_dynamic_runtime();

    /* Load a DLL at runtime and call into it. */
    demo_loadlibrary();

    print("main: exiting via ExitProcess\n");
    ExitProcess(0);
}
