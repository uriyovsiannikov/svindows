/*
 * user/msvcrt.c - a tiny C runtime (msvcrt.dll).
 *
 * The handful of CRT functions a small C program needs, implemented over the
 * Win32 API: printf/puts write through WriteFile, malloc/free run on the
 * process heap, plus the usual string and memory helpers. Built and linked like
 * a real msvcrt import; programs link against it instead of a host libc.
 */

typedef void              *HANDLE;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONGLONG;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void  *HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL   HeapFree(HANDLE, DWORD, void *);

/* ------------------------------------------------------------------ */
/* mem* / str*                                                         */
/* ------------------------------------------------------------------ */

__declspec(dllexport) void *memset(void *dst, int v, SIZE_T n)
{
    unsigned char *p = dst;
    while (n--)
        *p++ = (unsigned char)v;
    return dst;
}

__declspec(dllexport) void *memcpy(void *dst, const void *src, SIZE_T n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

__declspec(dllexport) void *memmove(void *dst, const void *src, SIZE_T n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

__declspec(dllexport) SIZE_T strlen(const char *s)
{
    SIZE_T n = 0;
    while (s[n])
        n++;
    return n;
}

__declspec(dllexport) char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

__declspec(dllexport) int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ------------------------------------------------------------------ */
/* malloc / free (over the process heap)                               */
/* ------------------------------------------------------------------ */

__declspec(dllexport) void *malloc(SIZE_T n)
{
    return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}

__declspec(dllexport) void free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

__declspec(dllexport) void *calloc(SIZE_T count, SIZE_T size)
{
    SIZE_T total = count * size;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* ------------------------------------------------------------------ */
/* printf / puts                                                       */
/* ------------------------------------------------------------------ */

static int fmt_num(char *buf, ULONGLONG v, int base, int is_signed, long long sv)
{
    char tmp[24];
    int n = 0, len = 0, neg = 0;
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

/* Format into `out`; supports %d %u %x %p %s %c %%. */
static int format(char *out, const char *fmt, __builtin_va_list ap)
{
    int o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out[o++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'd': o += fmt_num(out + o, 0, 10, 1, __builtin_va_arg(ap, int)); break;
        case 'u': o += fmt_num(out + o, __builtin_va_arg(ap, unsigned), 10, 0, 0); break;
        case 'x': o += fmt_num(out + o, __builtin_va_arg(ap, unsigned), 16, 0, 0); break;
        case 'p': {
            out[o++] = '0'; out[o++] = 'x';
            o += fmt_num(out + o, (ULONGLONG)__builtin_va_arg(ap, void *), 16, 0, 0);
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s)
                out[o++] = *s++;
            break;
        }
        case 'c': out[o++] = (char)__builtin_va_arg(ap, int); break;
        case '%': out[o++] = '%'; break;
        default:  out[o++] = '%'; out[o++] = *p; break;
        }
    }
    out[o] = 0;
    return o;
}

static void write_stdout(const char *s, int len)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)len, &written, 0);
}

__declspec(dllexport) int printf(const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = format(buf, fmt, ap);
    __builtin_va_end(ap);
    write_stdout(buf, n);
    return n;
}

__declspec(dllexport) int puts(const char *s)
{
    int n = (int)strlen(s);
    write_stdout(s, n);
    write_stdout("\n", 1);
    return n + 1;
}
