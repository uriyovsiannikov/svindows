/* string.h - the sliver of string.h NTOS's msvcrt provides. */
#ifndef _NTOS_STRING_H_
#define _NTOS_STRING_H_
#include <stddef.h>
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int v, size_t n);
#endif
