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

/* Registry (advapi32). */
typedef void *HKEY;
typedef long  LONG;
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

    /* Read and write the registry. */
    demo_registry();

    /* Broader Win32 surface + the mini-CRT. */
    demo_win32_crt();

    print("main: exiting via ExitProcess\n");
    ExitProcess(0);
}
