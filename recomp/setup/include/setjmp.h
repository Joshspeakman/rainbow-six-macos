/* See string.h in this folder. arm64 macOS's jmp_buf: 48 ints (the SDK's _JBLEN). */
#ifndef SETUP_SETJMP_H
#define SETUP_SETJMP_H
typedef int jmp_buf[48];
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));
int _setjmp(jmp_buf) __attribute__((returns_twice));
void _longjmp(jmp_buf, int) __attribute__((noreturn));
#endif
