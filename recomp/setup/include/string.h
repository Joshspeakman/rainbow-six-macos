/* The few C library declarations the recompiled game code uses. The player's Mac compiles that code without Apple's SDK
 * (recomp/setup); these names resolve from the system's libSystem when the game is loaded. */
#ifndef SETUP_STRING_H
#define SETUP_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
#endif
