/*
 * ntos/rtl.h - Runtime Library (Rtl).
 *
 * Freestanding replacements for the handful of libc-ish primitives the kernel
 * needs, plus NT list helpers and the formatted-output engine used by the
 * console and the debugger.
 */
#ifndef _NTOS_RTL_H_
#define _NTOS_RTL_H_

#include <nt/ntdef.h>

/* Also provide the plain C names, since the compiler may emit calls to
 * memcpy/memset/memmove/memcmp for aggregate operations. */
void  *memcpy(void *dst, const void *src, SIZE_T n);
void  *memmove(void *dst, const void *src, SIZE_T n);
void  *memset(void *dst, int value, SIZE_T n);
int    memcmp(const void *a, const void *b, SIZE_T n);
SIZE_T strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, SIZE_T n);
char  *strcpy(char *dst, const char *src);

/* NT-flavoured aliases. */
#define RtlCopyMemory(d, s, n)   memcpy((d), (s), (n))
#define RtlMoveMemory(d, s, n)   memmove((d), (s), (n))
#define RtlZeroMemory(d, n)      memset((d), 0, (n))
#define RtlFillMemory(d, n, v)   memset((d), (v), (n))
#define RtlCompareMemory(a, b, n) memcmp((a), (b), (n))

/* ------------------------------------------------------------------ */
/* Doubly linked lists (LIST_ENTRY)                                   */
/* ------------------------------------------------------------------ */

static ALWAYS_INLINE void InitializeListHead(PLIST_ENTRY head)
{
    head->Flink = head->Blink = head;
}

static ALWAYS_INLINE BOOLEAN IsListEmpty(const LIST_ENTRY *head)
{
    return (BOOLEAN)(head->Flink == head);
}

static ALWAYS_INLINE void InsertHeadList(PLIST_ENTRY head, PLIST_ENTRY entry)
{
    PLIST_ENTRY first = head->Flink;
    entry->Flink = first;
    entry->Blink = head;
    first->Blink = entry;
    head->Flink  = entry;
}

static ALWAYS_INLINE void InsertTailList(PLIST_ENTRY head, PLIST_ENTRY entry)
{
    PLIST_ENTRY last = head->Blink;
    entry->Flink = head;
    entry->Blink = last;
    last->Flink  = entry;
    head->Blink  = entry;
}

static ALWAYS_INLINE BOOLEAN RemoveEntryList(PLIST_ENTRY entry)
{
    PLIST_ENTRY flink = entry->Flink;
    PLIST_ENTRY blink = entry->Blink;
    blink->Flink = flink;
    flink->Blink = blink;
    return (BOOLEAN)(flink == blink);
}

static ALWAYS_INLINE PLIST_ENTRY RemoveHeadList(PLIST_ENTRY head)
{
    PLIST_ENTRY entry = head->Flink;
    RemoveEntryList(entry);
    return entry;
}

/* ------------------------------------------------------------------ */
/* Formatted output                                                   */
/* ------------------------------------------------------------------ */

#include <stdarg.h>

/*
 * RtlFormatV renders `fmt` into `buffer` (always NUL-terminates when size > 0)
 * and returns the number of characters that would have been written had the
 * buffer been unbounded. Supported conversions:
 *   %c %s %% %d %i %u %x %X %p %b
 * with width, zero-pad, and the l / ll / z length modifiers.
 */
int RtlFormatV(char *buffer, SIZE_T size, const char *fmt, va_list ap);
int RtlFormat(char *buffer, SIZE_T size, const char *fmt, ...);

#endif /* _NTOS_RTL_H_ */
