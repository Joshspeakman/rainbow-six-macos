/* Compiled with the game code on the player's Mac. The runtime's thread-local that the recompiled code uses directly lives
 * here: the linker can't bind a thread-local across the split between the ready-made runtime and the game. */
#include <stdint.h>
#ifndef RT_HOST_RC_INIT
#define RT_HOST_RC_INIT 0
#endif
__thread uint32_t rt_host_rc = RT_HOST_RC_INIT;
