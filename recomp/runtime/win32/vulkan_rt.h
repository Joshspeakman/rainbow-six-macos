/* Vulkan for the 32-bit guest: what the generated conversions (vulkan_gen.c) and the hand-written commands
 * (vulkan.c) share. Guest structures are read and written at guest addresses; host structures are native. */
#ifndef VULKAN_RT_H
#define VULKAN_RT_H
#include "w32.h"
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
/* the Win32 surface structure, with the Windows types it names (only carried, never used, on the host) */
typedef void *HINSTANCE, *HWND, *HMONITOR, *HANDLE;
typedef uint32_t DWORD;
typedef const uint16_t *LPCWSTR;
typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
#include <vulkan/vulkan_win32.h>

#define VKR8(a)   rt_r8(G_MEM, (uint32_t)(a))
#define VKR16(a)  rt_r16(G_MEM, (uint32_t)(a))
#define VKR32(a)  rt_r32(G_MEM, (uint32_t)(a))
#define VKR64(a)  rt_r64(G_MEM, (uint32_t)(a))
#define VKW8(a, v)  rt_w8(G_MEM, (uint32_t)(a), (uint32_t)(v))
#define VKW16(a, v) rt_w16(G_MEM, (uint32_t)(a), (uint32_t)(v))
#define VKW32(a, v) rt_w32(G_MEM, (uint32_t)(a), (uint32_t)(v))
#define VKW64(a, v) rt_w64(G_MEM, (uint32_t)(a), (uint64_t)(v))
#define VKPTR(a)  ((uint32_t)(a) ? (void *)(G_MEM + (uint32_t)(a)) : (void *)0)   /* guest data used in place */

/* per-call allocations for converted structures, freed when the command returns */
typedef struct VkArena { void **blocks; unsigned n, cap; } VkArena;
void *vk_arena(VkArena *a, size_t n);
void vk_arena_free(VkArena *a);

/* dispatchable handles (VkInstance, VkPhysicalDevice, VkDevice, VkQueue, VkCommandBuffer) are host pointers; the
 * guest holds a 32-bit id for each */
uint32_t vkh_guest(const void *host);
void *vkh_host(uint32_t guest);
void vkh_forget(const void *host);

void vk_unknown_stype(uint32_t stype);
extern int vk_trace;
#define VK_TRACE(name, r) do { if (vk_trace || ((int32_t)(r) < 0 && (int32_t)(r) != -1000001004)) fprintf(stderr, "vulkan: %s -> %d\n", name, (int32_t)(r)); } while (0)

#include "vulkan_gen.h"
#endif
