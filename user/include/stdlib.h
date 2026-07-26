/* stdlib.h - the sliver of stdlib NTOS's msvcrt provides. */
#ifndef _NTOS_STDLIB_H_
#define _NTOS_STDLIB_H_
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void  free(void *p);
void  exit(int code);
int   atoi(const char *s);
#endif
