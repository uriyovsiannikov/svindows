/*
 * rtl/print.c - the formatted-output engine behind KeLog / DbgPrint.
 *
 * A compact, self-contained printf. It writes into a caller buffer, always
 * NUL-terminates when there is room, and returns the number of characters the
 * full output would occupy (like C99 vsnprintf), so callers can detect
 * truncation.
 *
 * Supported: %c %s %% and integers %d %i %u %x %X %p %b, with an optional
 * field width, '0' zero-padding, and the l / ll / z length modifiers.
 */
#include <ntos/rtl.h>

typedef struct {
    char  *buf;
    SIZE_T size;   /* capacity including the NUL slot */
    SIZE_T count;  /* characters emitted so far (may exceed size-1) */
} FMT_SINK;

static void sink_putc(FMT_SINK *s, char c)
{
    if (s->count + 1 < s->size)
        s->buf[s->count] = c;
    s->count++;
}

/* Emit an unsigned value in the given base with optional zero/space padding. */
static void sink_number(FMT_SINK *s, ULONGLONG value, unsigned base,
                        BOOLEAN uppercase, int width, BOOLEAN zero_pad,
                        BOOLEAN negative)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? upper : lower;
    char tmp[32];
    int len = 0;

    do {
        tmp[len++] = digits[value % base];
        value /= base;
    } while (value && len < (int)sizeof(tmp));

    int total = len + (negative ? 1 : 0);
    char pad = zero_pad ? '0' : ' ';

    /* Space padding goes before the sign; zero padding goes after it. */
    if (!zero_pad) {
        for (int i = total; i < width; i++)
            sink_putc(s, pad);
        if (negative)
            sink_putc(s, '-');
    } else {
        if (negative)
            sink_putc(s, '-');
        for (int i = total; i < width; i++)
            sink_putc(s, pad);
    }

    while (len > 0)
        sink_putc(s, tmp[--len]);
}

int RtlFormatV(char *buffer, SIZE_T size, const char *fmt, va_list ap)
{
    FMT_SINK s = { buffer, size, 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_putc(&s, *p);
            continue;
        }

        p++; /* consume '%' */

        /* Flags: only '0' (zero pad) is meaningful here. */
        BOOLEAN zero_pad = FALSE;
        while (*p == '0') {
            zero_pad = TRUE;
            p++;
        }

        /* Minimum field width. */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Length modifiers: l, ll, z. */
        int longness = 0; /* 0=int, 1=long, 2=long long / size_t */
        if (*p == 'z') {
            longness = 2;
            p++;
        } else {
            while (*p == 'l') {
                longness++;
                p++;
            }
        }

        switch (*p) {
        case '%':
            sink_putc(&s, '%');
            break;

        case 'c':
            sink_putc(&s, (char)va_arg(ap, int));
            break;

        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            SIZE_T len = strlen(str);
            for (SIZE_T i = (SIZE_T)len; (int)i < width; i++)
                sink_putc(&s, ' ');
            while (*str)
                sink_putc(&s, *str++);
            break;
        }

        case 'd':
        case 'i': {
            LONGLONG v;
            if (longness >= 2)
                v = va_arg(ap, LONGLONG);
            else if (longness == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);
            BOOLEAN neg = v < 0;
            ULONGLONG mag = neg ? (ULONGLONG)(-(v + 1)) + 1ULL : (ULONGLONG)v;
            sink_number(&s, mag, 10, FALSE, width, zero_pad, neg);
            break;
        }

        case 'u': {
            ULONGLONG v;
            if (longness >= 2)
                v = va_arg(ap, ULONGLONG);
            else if (longness == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            sink_number(&s, v, 10, FALSE, width, zero_pad, FALSE);
            break;
        }

        case 'x':
        case 'X': {
            ULONGLONG v;
            if (longness >= 2)
                v = va_arg(ap, ULONGLONG);
            else if (longness == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            sink_number(&s, v, 16, (BOOLEAN)(*p == 'X'), width, zero_pad, FALSE);
            break;
        }

        case 'b': {
            ULONGLONG v;
            if (longness >= 2)
                v = va_arg(ap, ULONGLONG);
            else if (longness == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            sink_number(&s, v, 2, FALSE, width, zero_pad, FALSE);
            break;
        }

        case 'p': {
            /* Pointers print as 0x-prefixed, zero-padded to 16 hex digits. */
            ULONGLONG v = (ULONGLONG)(ULONG_PTR)va_arg(ap, void *);
            sink_putc(&s, '0');
            sink_putc(&s, 'x');
            sink_number(&s, v, 16, FALSE, 16, TRUE, FALSE);
            break;
        }

        case '\0':
            /* Trailing '%' at end of string: stop. */
            p--;
            break;

        default:
            /* Unknown conversion: emit it verbatim. */
            sink_putc(&s, '%');
            sink_putc(&s, *p);
            break;
        }
    }

    /* NUL-terminate. */
    if (s.size > 0) {
        SIZE_T term = s.count < s.size ? s.count : s.size - 1;
        s.buf[term] = '\0';
    }

    return (int)s.count;
}

int RtlFormat(char *buffer, SIZE_T size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = RtlFormatV(buffer, size, fmt, ap);
    va_end(ap);
    return n;
}
