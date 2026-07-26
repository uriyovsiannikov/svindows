/*
 * rtl/string.c - freestanding C string helpers.
 */
#include <ntos/rtl.h>

SIZE_T strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (SIZE_T)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(UCHAR)*a - (int)(UCHAR)*b;
}

int strncmp(const char *a, const char *b, SIZE_T n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(UCHAR)*a - (int)(UCHAR)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}
