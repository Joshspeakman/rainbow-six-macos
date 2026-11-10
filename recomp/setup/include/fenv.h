/* See string.h in this folder. arm64 macOS's rounding modes (the SDK's values), for the recompiled game code. */
#ifndef SETUP_FENV_H
#define SETUP_FENV_H
#define FE_TONEAREST 0x00000000
#define FE_UPWARD 0x00400000
#define FE_DOWNWARD 0x00800000
#define FE_TOWARDZERO 0x00C00000
int fesetround(int);
int fegetround(void);
#endif
