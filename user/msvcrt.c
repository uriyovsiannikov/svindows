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
typedef unsigned short     WCHAR;

#define STD_OUTPUT_HANDLE ((DWORD)-11)

__declspec(dllimport) HANDLE GetStdHandle(DWORD which);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void  *HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL   HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) void   ExitProcess(DWORD code);
__declspec(dllimport) DWORD  GetEnvironmentVariableW(const WCHAR *name,
                                                      WCHAR *out, DWORD size);

/* Data exports used by the classic MSVC startup linked into Windows inbox
 * utilities.  They are writable through the executable's IAT. */
__declspec(dllexport) int _fmode;
__declspec(dllexport) int _commode;

/* MSVCRT exposes the raw wide command line as a data symbol, not a function.
 * Microsoft CRT startup code imports the address of this pointer and
 * dereferences it before calling main/wWinMain.  Keep a valid process-default
 * value available even before we have DLL entry-point initialization. */
static WCHAR g_wcmdline[] = L"explorer.exe";
__declspec(dllexport) WCHAR *_wcmdln = g_wcmdline;

static int g_errno;

__declspec(dllexport) int *_errno(void)
{
    return &g_errno;
}

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

__declspec(dllexport) int memcmp(const void *left, const void *right, SIZE_T n)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (SIZE_T i = 0; i < n; i++)
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    return 0;
}

__declspec(dllexport) int memcpy_s(void *dst, SIZE_T dst_size,
                                   const void *src, SIZE_T count)
{
    if (!count)
        return 0;
    if (!dst || !src) {
        if (dst && dst_size)
            memset(dst, 0, dst_size);
        g_errno = 22; /* EINVAL */
        return 22;
    }
    if (count > dst_size) {
        if (dst_size)
            memset(dst, 0, dst_size);
        g_errno = 34; /* ERANGE */
        return 34;
    }
    memcpy(dst, src, count);
    return 0;
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

__declspec(dllexport) char *strchr(const char *text, int character)
{
    if (!text)
        return 0;
    char c = (char)character;
    do {
        if (*text == c)
            return (char *)text;
    } while (*text++);
    return 0;
}

__declspec(dllexport) int wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

/* ntdll's heap/critical-section leaf exports are resolved to msvcrt by the
 * kernel loader when our compact ntdll does not provide them. Inbox USER and
 * shell DLLs use these names directly during class/activation setup. */
__declspec(dllexport) void *RtlAllocateHeap(void *heap, DWORD flags, SIZE_T size)
{
    return HeapAlloc(heap ? heap : GetProcessHeap(), flags, size ? size : 1);
}

__declspec(dllexport) unsigned char RtlFreeHeap(void *heap, DWORD flags,
                                                 void *memory)
{
    return (unsigned char)HeapFree(heap ? heap : GetProcessHeap(), flags,
                                   memory);
}

__declspec(dllexport) long RtlEnterCriticalSection(void *critical_section)
{
    (void)critical_section;
    return 0;
}

__declspec(dllexport) long RtlLeaveCriticalSection(void *critical_section)
{
    (void)critical_section;
    return 0;
}

typedef struct _RTL_UNICODE_STRING_LOCAL {
    unsigned short Length;
    unsigned short MaximumLength;
    WCHAR *Buffer;
} RTL_UNICODE_STRING_LOCAL;

__declspec(dllexport) void RtlInitUnicodeString(RTL_UNICODE_STRING_LOCAL *out,
                                                const WCHAR *text)
{
    if (!out)
        return;
    SIZE_T chars = 0;
    if (text)
        while (text[chars]) chars++;
    out->Length = (unsigned short)(chars * sizeof(WCHAR));
    out->MaximumLength = (unsigned short)((chars + (text ? 1 : 0)) *
                                           sizeof(WCHAR));
    out->Buffer = (WCHAR *)text;
}

__declspec(dllexport) unsigned char RtlGetIntegerAtom(const WCHAR *name,
                                                       unsigned short *atom)
{
    SIZE_T value = (SIZE_T)name;
    if ((value >> 16) == 0) {
        if (atom) *atom = (unsigned short)value;
        return value != 0;
    }
    if (!name || name[0] != '#')
        return 0;
    unsigned int parsed = 0;
    SIZE_T i = 1;
    if (!name[i])
        return 0;
    for (; name[i] >= '0' && name[i] <= '9'; i++)
        parsed = parsed * 10 + (unsigned int)(name[i] - '0');
    if (name[i] || parsed == 0 || parsed > 0xbfff)
        return 0;
    if (atom) *atom = (unsigned short)parsed;
    return 1;
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

__declspec(dllexport) void exit(int code)
{
    ExitProcess((DWORD)code);
}

/* atoi: parse a leading optional-sign decimal integer. */
__declspec(dllexport) int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

/* ------------------------------------------------------------------ */
/* Classic MSVCRT compatibility used by Windows 11 console utilities  */
/* ------------------------------------------------------------------ */

static unsigned char g_iob[3][48]; /* old MSVCRT FILE objects are 48 bytes */

__declspec(dllexport) void *__iob_func(void) { return g_iob; }

__declspec(dllexport) int _fileno(void *stream)
{
    long long index = ((unsigned char *)stream - &g_iob[0][0]) / 48;
    return (index >= 0 && index < 3) ? (int)index : -1;
}

__declspec(dllexport) long long _get_osfhandle(int fd)
{
    DWORD which = fd == 0 ? (DWORD)-10 : (fd == 2 ? (DWORD)-12
                                                    : STD_OUTPUT_HANDLE);
    return (long long)(ULONGLONG)GetStdHandle(which);
}

__declspec(dllexport) int _write(int fd, const void *buffer, unsigned count)
{
    DWORD written = 0;
    HANDLE h = (HANDLE)(ULONGLONG)_get_osfhandle(fd);
    return WriteFile(h, buffer, count, &written, 0) ? (int)written : -1;
}

/* MSVCRT's wide formatter is used by inbox tools for paths and resource keys.
 * This compact implementation covers strings, characters, signed/unsigned
 * integers, hexadecimal values, field width and zero padding. */
__declspec(dllexport) int _vsnwprintf(WCHAR *out, SIZE_T count,
                                      const WCHAR *fmt,
                                      __builtin_va_list ap)
{
    SIZE_T n = 0;
    for (const WCHAR *p = fmt; *p; p++) {
        if (*p != '%') {
            if (n + 1 < count) out[n] = *p;
            n++;
            continue;
        }
        p++;
        if (*p == '%') {
            if (n + 1 < count) out[n] = '%';
            n++;
            continue;
        }

        WCHAR pad = ' ';
        int left_align = 0;
        int force_sign = 0;
        int space_sign = 0;
        for (;;) {
            if (*p == '-') { left_align = 1; p++; continue; }
            if (*p == '+') { force_sign = 1; p++; continue; }
            if (*p == ' ') { space_sign = 1; p++; continue; }
            if (*p == '#') { p++; continue; }
            if (*p == '0') { pad = '0'; p++; continue; }
            break;
        }
        int width = 0;
        if (*p == '*') {
            width = __builtin_va_arg(ap, int);
            if (width < 0) { left_align = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = __builtin_va_arg(ap, int);
                p++;
            } else while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }
        enum { LEN_DEFAULT, LEN_LONG, LEN_LLONG } length = LEN_DEFAULT;
        if (*p == 'l') {
            p++;
            length = LEN_LONG;
            if (*p == 'l') { p++; length = LEN_LLONG; }
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            p += 3;
            length = LEN_LLONG;
        }

        if (*p == 's') {
            const WCHAR *s = __builtin_va_arg(ap, const WCHAR *);
            SIZE_T len = 0;
            while (s && s[len]) len++;
            if (precision >= 0 && len > (SIZE_T)precision)
                len = (SIZE_T)precision;
            if (!left_align) while (width-- > (int)len) {
                if (n + 1 < count) out[n] = pad;
                n++;
            }
            for (SIZE_T i = 0; i < len; i++) {
                if (n + 1 < count) out[n] = s[i];
                n++;
            }
            if (left_align) while (width-- > (int)len) {
                if (n + 1 < count) out[n] = ' ';
                n++;
            }
            continue;
        }
        if (*p == 'c') {
            WCHAR c = (WCHAR)__builtin_va_arg(ap, int);
            if (n + 1 < count) out[n] = c;
            n++;
            continue;
        }

        unsigned long long value;
        int base = (*p == 'x' || *p == 'X') ? 16 : 10;
        int negative = 0;
        if (*p == 'd' || *p == 'i') {
            long long signed_value;
            if (length == LEN_LLONG)
                signed_value = __builtin_va_arg(ap, long long);
            else if (length == LEN_LONG)
                signed_value = __builtin_va_arg(ap, long);
            else
                signed_value = __builtin_va_arg(ap, int);
            if (signed_value < 0) {
                negative = 1;
                value = (unsigned long long)-signed_value;
            } else value = (unsigned long long)signed_value;
        } else if (*p == 'u' || *p == 'x' || *p == 'X') {
            if (length == LEN_LLONG)
                value = __builtin_va_arg(ap, unsigned long long);
            else if (length == LEN_LONG)
                value = __builtin_va_arg(ap, unsigned long);
            else
                value = __builtin_va_arg(ap, unsigned int);
        } else {
            if (n + 1 < count) out[n] = '?';
            n++;
            continue;
        }

        WCHAR digits[24];
        int len = 0;
        do {
            unsigned d = (unsigned)(value % (unsigned)base);
            digits[len++] = (WCHAR)(d < 10 ? '0' + d
                                            : ((*p == 'X' ? 'A' : 'a') + d - 10));
            value /= (unsigned)base;
        } while (value);
        WCHAR sign = negative ? '-' : (force_sign ? '+' : (space_sign ? ' ' : 0));
        int total = len + (sign != 0);
        if (sign && pad == '0' && !left_align) {
            if (n + 1 < count) out[n] = sign;
            n++;
            sign = 0;
        }
        if (!left_align) while (width-- > total) {
            if (n + 1 < count) out[n] = pad;
            n++;
        }
        if (sign) {
            if (n + 1 < count) out[n] = sign;
            n++;
        }
        while (len--) {
            if (n + 1 < count) out[n] = digits[len];
            n++;
        }
        if (left_align) while (width-- > total) {
            if (n + 1 < count) out[n] = ' ';
            n++;
        }
    }
    if (count)
        out[n < count ? n : count - 1] = 0;
    return n < count ? (int)n : -1;
}

/* Enough wide stdio for inbox console tools.  Hostname.exe uses exactly
 * fwprintf(stdout, L"%ls", text); keeping it here also makes error messages
 * with literal wide text visible. */
__declspec(dllexport) int fwprintf(void *stream, const WCHAR *fmt, ...)
{
    char out[512];
    int n = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const WCHAR *p = fmt; *p && n < (int)sizeof(out); p++) {
        if (*p != '%') {
            out[n++] = *p <= 0x7f ? (char)*p : '?';
            continue;
        }
        p++;
        if (*p == '%') {
            out[n++] = '%';
        } else if (*p == 'l' && p[1] == 's') {
            const WCHAR *s = __builtin_va_arg(ap, const WCHAR *);
            p++;
            while (s && *s && n < (int)sizeof(out)) {
                out[n++] = *s <= 0x7f ? (char)*s : '?';
                s++;
            }
        } else {
            /* Unsupported conversion: preserve it visibly for diagnostics. */
            out[n++] = '?';
        }
    }
    __builtin_va_end(ap);
    return _write(_fileno(stream), out, (unsigned)n) < 0 ? -1 : n;
}

__declspec(dllexport) int _setmode(int fd, int mode)
{
    (void)fd; (void)mode;
    return 0;
}

__declspec(dllexport) int fflush(void *stream) { (void)stream; return 0; }
__declspec(dllexport) int fgetpos(void *stream, long long *pos)
{
    (void)stream;
    if (pos) *pos = 0;
    return 0;
}

__declspec(dllexport) WCHAR *wcschr(const WCHAR *s, WCHAR c)
{
    while (*s) {
        if (*s == c) return (WCHAR *)s;
        s++;
    }
    return c == 0 ? (WCHAR *)s : 0;
}

static WCHAR fold_w(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + ('a' - 'A')) : c;
}

__declspec(dllexport) int _wcsicmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && fold_w(*a) == fold_w(*b)) { a++; b++; }
    return (int)fold_w(*a) - (int)fold_w(*b);
}

__declspec(dllexport) int _memicmp(const void *va, const void *vb, SIZE_T n)
{
    const unsigned char *a = va, *b = vb;
    while (n--) {
        unsigned char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        unsigned char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return 0;
}

__declspec(dllexport) WCHAR towupper(WCHAR c)
{
    return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
}

__declspec(dllexport) WCHAR *wcsrchr(const WCHAR *s, WCHAR c)
{
    const WCHAR *last = 0;
    do {
        if (*s == c) last = s;
    } while (*s++);
    return (WCHAR *)last;
}

__declspec(dllexport) WCHAR *wcspbrk(const WCHAR *s, const WCHAR *accept)
{
    for (; *s; s++)
        for (const WCHAR *a = accept; *a; a++)
            if (*s == *a) return (WCHAR *)s;
    return 0;
}

__declspec(dllexport) WCHAR *wcstok(WCHAR *text, const WCHAR *delimiters)
{
    static WCHAR *next;
    WCHAR *p = text ? text : next;
    if (!p) return 0;
    while (*p && wcspbrk(p, delimiters) == p) p++;
    if (!*p) { next = 0; return 0; }
    WCHAR *token = p;
    while (*p && wcspbrk(p, delimiters) != p) p++;
    if (*p) { *p = 0; next = p + 1; }
    else next = 0;
    return token;
}

static unsigned long parse_wide_uint(const WCHAR *s, WCHAR **end, int base,
                                     int *negative)
{
    while (*s == ' ' || *s == '\t') s++;
    *negative = 0;
    if (*s == '-') { *negative = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) base = 10;
    unsigned long value = 0;
    const WCHAR *first = s;
    for (;;) {
        int digit = (*s >= '0' && *s <= '9') ? *s - '0' :
                    (*s >= 'A' && *s <= 'Z') ? *s - 'A' + 10 :
                    (*s >= 'a' && *s <= 'z') ? *s - 'a' + 10 : -1;
        if (digit < 0 || digit >= base) break;
        value = value * (unsigned)base + (unsigned)digit;
        s++;
    }
    if (end) *end = (WCHAR *)(s == first ? first : s);
    return value;
}

__declspec(dllexport) long wcstol(const WCHAR *s, WCHAR **end, int base)
{
    int negative;
    unsigned long value = parse_wide_uint(s, end, base, &negative);
    return negative ? -(long)value : (long)value;
}

__declspec(dllexport) unsigned long wcstoul(const WCHAR *s, WCHAR **end,
                                            int base)
{
    int negative;
    unsigned long value = parse_wide_uint(s, end, base, &negative);
    return negative ? (unsigned long)(-(long)value) : value;
}

__declspec(dllexport) WCHAR *_wgetenv(const WCHAR *name)
{
    typedef struct _ENV_CACHE_ENTRY {
        WCHAR name[32];
        WCHAR value[512];
    } ENV_CACHE_ENTRY;
    static ENV_CACHE_ENTRY cache[8];

    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (!cache[i].name[0]) {
            if (slot < 0) slot = i;
            continue;
        }
        int j = 0;
        while (name[j] && fold_w(name[j]) == fold_w(cache[i].name[j])) j++;
        if (!name[j] && !cache[i].name[j]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return 0;
    if (!cache[slot].name[0]) {
        int i = 0;
        while (name[i] && i < 31) {
            cache[slot].name[i] = name[i];
            i++;
        }
        cache[slot].name[i] = 0;
    }
    DWORD n = GetEnvironmentVariableW(name, cache[slot].value, 512);
    return n && n < 512 ? cache[slot].value : 0;
}

/* Build the classic wide argv array from the PEB command line. */
__declspec(dllexport) int __wgetmainargs(int *argc, WCHAR ***argv,
                                         WCHAR ***env, int wildcard,
                                         void *startup_info)
{
    static WCHAR command_line[512];
    static WCHAR *args[32];
    static WCHAR *empty_env[] = { 0 };
    (void)wildcard; (void)startup_info;

    unsigned long long peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    unsigned char *params = *(unsigned char **)(peb + 0x20);
    WCHAR length = params ? *(WCHAR *)(params + 0x70) / 2 : 0;
    WCHAR *source = params ? *(WCHAR **)(params + 0x78) : 0;
    int n = 0;
    while (source && n < length && n < 511) {
        command_line[n] = source[n];
        n++;
    }
    command_line[n] = 0;

    int count = 0;
    WCHAR *p = command_line;
    while (*p && count < 31) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        args[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    args[count] = 0;
    if (argc) *argc = count;
    if (argv) *argv = args;
    if (env) *env = empty_env;
    return 0;
}

__declspec(dllexport) void __set_app_type(int type) { (void)type; }
__declspec(dllexport) void __setusermatherr(void *fn) { (void)fn; }
__declspec(dllexport) void _initterm(void (**first)(void), void (**last)(void))
{
    while (first < last) {
        if (*first) (*first)();
        first++;
    }
}
__declspec(dllexport) void _cexit(void) {}
__declspec(dllexport) void _exit(int code) { ExitProcess((DWORD)code); }
__declspec(dllexport) void _amsg_exit(int code) { ExitProcess((DWORD)code); }
__declspec(dllexport) int _XcptFilter(unsigned long code, void *info)
{ (void)code; (void)info; return 0; }
__declspec(dllexport) int __C_specific_handler(void *a, void *b, void *c, void *d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }

__declspec(dllexport) int _purecall(void)
{
    return 0;
}
