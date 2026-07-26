/*
 * rtl/memory.c - freestanding memory primitives.
 *
 * The compiler is allowed to emit calls to memcpy/memset/memmove/memcmp for
 * struct assignments and the like, so these must exist even though we also use
 * them directly. Simple byte-at-a-time implementations; correctness over speed.
 */
#include <ntos/rtl.h>

void *memcpy(void *dst, const void *src, SIZE_T n)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, SIZE_T n)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        /* Copy backwards for overlapping forward moves. */
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int value, SIZE_T n)
{
    UCHAR *d = (UCHAR *)dst;
    UCHAR v = (UCHAR)value;
    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, SIZE_T n)
{
    const UCHAR *pa = (const UCHAR *)a;
    const UCHAR *pb = (const UCHAR *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}
