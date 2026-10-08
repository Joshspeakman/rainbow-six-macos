/* vulkan-1.dll for the recompiled game: the commands that need more than a structure conversion (vulkan_gen.c has
 * the rest). The host's Vulkan is the system loader on Linux and MoltenVK on macOS; the host program provides the
 * window's surface (w32_host_vk_*).
 *
 * Mapped memory. The guest can only address its own 4 GB, so a mapping must appear inside it:
 *  - placed (VK_EXT_map_memory_placed): the driver maps the allocation at a guest address we reserve. No copies.
 *  - shadowed (no such extension, e.g. MoltenVK): the guest gets a copy; it is written to the device memory before
 *    every submission and at flushes, and read back at invalidations and, for host-cached memory (where the GPU's
 *    results are read), after the waits that make them visible.
 * Every allocation is mapped whole the first time and stays mapped until vkUnmapMemory or vkFreeMemory. */
#include "vulkan_rt.h"
#include <dlfcn.h>
#include <sys/mman.h>

int vk_trace;
static uint32_t present_family;                        /* 1 + the first queue family the game takes a queue from */
/* host hooks (host/linux.c, host/main.m) */
PFN_vkGetInstanceProcAddr (*w32_host_vk_loader)(void);                         /* the host's vkGetInstanceProcAddr */
unsigned (*w32_host_vk_extensions)(const char **names, unsigned cap);          /* instance extensions for surfaces */
int (*w32_host_vk_surface)(VkInstance instance, VkSurfaceKHR *surface);        /* the game window's surface */
void (*w32_host_vk_presented)(void);                                           /* a frame went to the screen */

static PFN_vkGetInstanceProcAddr gipa;
static int load_host(void)
{
    if (gipa) return 1;
    if (w32_host_vk_loader) gipa = w32_host_vk_loader();
    if (!gipa) {
#ifdef __APPLE__
        static const char *libs[] = {"libvulkan.1.dylib", "libMoltenVK.dylib", 0};
#else
        static const char *libs[] = {"libvulkan.so.1", "libvulkan.so", 0};
#endif
        for (int k = 0; libs[k] && !gipa; k++) { void *h = dlopen(libs[k], RTLD_NOW | RTLD_LOCAL); if (h) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr"); }
    }
    vk_trace = getenv("W32_VKLOG") != 0;
    if (!gipa) fprintf(stderr, "vulkan: no Vulkan on this system\n");
    return gipa != 0;
}
static void load_procs(VkInstance inst)
{
    void **slot = (void **)&vkp;
    for (int k = 0; vk_proc_names[k]; k++) {
        const char *n = vk_proc_names[k]; PFN_vkVoidFunction f = gipa(inst, n);
        if (!f) {                                      /* a KHR name for what the core has (1.1+) */
            char core[96]; size_t l = strlen(n);
            if (l > 3 && !strcmp(n + l - 3, "KHR") && l < sizeof core) { memcpy(core, n, l - 3); core[l - 3] = 0; f = gipa(inst, core); }
        }
        slot[k] = (void *)f;
    }
}

/* ---- per-call arena ---- */
void *vk_arena(VkArena *a, size_t n)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->blocks = realloc(a->blocks, a->cap * sizeof *a->blocks); }
    return a->blocks[a->n++] = calloc(1, n ? n : 1);
}
void vk_arena_free(VkArena *a) { for (unsigned k = 0; k < a->n; k++) free(a->blocks[k]); free(a->blocks); a->blocks = 0; a->n = a->cap = 0; }

/* ---- dispatchable handles: guest ids are 16-byte guest allocations, so they are unique and never 0 ---- */
#define NH 8192
static struct { uint32_t g; const void *h; } htab[NH]; static pthread_mutex_t h_m = PTHREAD_MUTEX_INITIALIZER;
uint32_t vkh_guest(const void *host)
{
    if (!host) return 0;
    pthread_mutex_lock(&h_m);
    for (int k = 0; k < NH; k++) if (htab[k].h == host) { uint32_t g = htab[k].g; pthread_mutex_unlock(&h_m); return g; }
    uint32_t g = heap_alloc(w32_process_heap, 8, 16);
    for (int k = 0; k < NH; k++) if (!htab[k].h) { htab[k].g = g; htab[k].h = host; break; }
    pthread_mutex_unlock(&h_m);
    return g;
}
void *vkh_host(uint32_t guest)
{
    if (!guest) return 0;
    uint32_t k = (guest >> 4) % NH;                    /* fast path: look where the id hashes first */
    if (htab[k].g == guest) return (void *)htab[k].h;
    pthread_mutex_lock(&h_m);
    for (int j = 0; j < NH; j++) if (htab[j].g == guest) { void *h = (void *)htab[j].h; pthread_mutex_unlock(&h_m); return h; }
    pthread_mutex_unlock(&h_m);
    fprintf(stderr, "vulkan: unknown handle %08x\n", guest);
    return 0;
}
void vkh_forget(const void *host)
{
    pthread_mutex_lock(&h_m);
    for (int k = 0; k < NH; k++) if (htab[k].h == host) { heap_free(w32_process_heap, htab[k].g); htab[k].h = 0; htab[k].g = 0; }
    pthread_mutex_unlock(&h_m);
}
void vk_unknown_stype(uint32_t stype)
{
    static uint32_t seen[64]; static int n;
    for (int k = 0; k < n; k++) if (seen[k] == stype) return;
    if (n < 64) seen[n++] = stype;
    fprintf(stderr, "vulkan: structure type %u in a pNext chain is not converted: left out\n", stype);
}

/* ---- proc addresses ---- */
static uint32_t proc(const char *name)
{
    extern int w32_thunk_implemented(uint32_t);
    if (!strcmp(name, "vkCreateDebugUtilsMessengerEXT") || !strcmp(name, "vkDestroyDebugUtilsMessengerEXT")) return 0;
    uint32_t t = w32_thunk_for("vulkan-1", name);
    if (!t || !w32_thunk_implemented(t)) { if (vk_trace) fprintf(stderr, "vulkan: %s not available\n", name); return 0; }
    return t;
}
IMPL(vulkan_1, vkGetInstanceProcAddr) { load_host(); RET(proc(GS(ARG(1))), 2); }
IMPL(vulkan_1, vkGetDeviceProcAddr) { RET(proc(GS(ARG(1))), 2); }

/* ---- instance ---- */
static int has_name(const char *const *v, uint32_t n, const char *s) { for (uint32_t k = 0; k < n; k++) if (!strcmp(v[k], s)) return 1; return 0; }
static VkExtensionProperties *host_inst_ext; static uint32_t host_inst_next;
static void host_instance_extensions(void)
{
    if (host_inst_ext || !load_host()) return;
    PFN_vkEnumerateInstanceExtensionProperties e = (PFN_vkEnumerateInstanceExtensionProperties)gipa(0, "vkEnumerateInstanceExtensionProperties");
    e(0, &host_inst_next, 0); host_inst_ext = calloc(host_inst_next + 1, sizeof *host_inst_ext); e(0, &host_inst_next, host_inst_ext);
}
static int host_has_inst_ext(const char *n) { host_instance_extensions(); for (uint32_t k = 0; k < host_inst_next; k++) if (!strcmp(host_inst_ext[k].extensionName, n)) return 1; return 0; }
/* what the game is told it can enable: surfaces as on Windows, and the 1.1 helpers it may use */
IMPL(vulkan_1, vkEnumerateInstanceExtensionProperties)                 /* (layer, uint32_t *count, VkExtensionProperties *) */
{
    static const char *offer[] = {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_physical_device_properties2", "VK_KHR_get_surface_capabilities2", 0};
    if (ARG(0)) { rt_w32(G_MEM, ARG(1), 0); RET(VK_SUCCESS, 3); }
    host_instance_extensions();
    VkExtensionProperties list[8]; uint32_t n = 0;
    for (int k = 0; offer[k]; k++) if (!strcmp(offer[k], "VK_KHR_win32_surface") || host_has_inst_ext(offer[k])) {
        memset(&list[n], 0, sizeof list[n]); snprintf(list[n].extensionName, sizeof list[n].extensionName, "%s", offer[k]); list[n].specVersion = 1; n++;
    }
    uint32_t cap = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (!out) { rt_w32(G_MEM, ARG(1), n); RET(VK_SUCCESS, 3); }
    uint32_t w = cap < n ? cap : n; memcpy(G_MEM + out, list, w * sizeof *list); rt_w32(G_MEM, ARG(1), w);
    RET(w < n ? VK_INCOMPLETE : VK_SUCCESS, 3);
}
IMPL(vulkan_1, vkEnumerateInstanceLayerProperties) { rt_w32(G_MEM, ARG(0), 0); RET(VK_SUCCESS, 2); }
static int portability;                                 /* MoltenVK: VK_KHR_portability_enumeration */
IMPL(vulkan_1, vkCreateInstance)                                        /* (create info, allocator, VkInstance *) */
{
    if (!load_host()) RET(VK_ERROR_INITIALIZATION_FAILED, 3);
    VkArena a = {0}; VkInstanceCreateInfo ci; cin_VkInstanceCreateInfo(&a, ARG(0), &ci);
    const char **ext = vk_arena(&a, sizeof *ext * (ci.enabledExtensionCount + 16)); uint32_t n = 0;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char *e = ci.ppEnabledExtensionNames[k];
        if (!strcmp(e, "VK_KHR_win32_surface")) continue;                  /* replaced by the host's surface */
        if (!host_has_inst_ext(e)) { fprintf(stderr, "vulkan: instance extension %s not available: left out\n", e); continue; }
        if (!has_name(ext, n, e)) ext[n++] = e;
    }
    const char *hs[8]; unsigned nh = w32_host_vk_extensions ? w32_host_vk_extensions(hs, 8) : 0;
    for (unsigned k = 0; k < nh; k++) if (!has_name(ext, n, hs[k])) ext[n++] = hs[k];
    if (!has_name(ext, n, "VK_KHR_surface")) ext[n++] = "VK_KHR_surface";
    if (host_has_inst_ext("VK_KHR_portability_enumeration")) { ext[n++] = "VK_KHR_portability_enumeration"; ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; portability = 1; }
    if (host_has_inst_ext("VK_KHR_get_physical_device_properties2") && !has_name(ext, n, "VK_KHR_get_physical_device_properties2")) ext[n++] = "VK_KHR_get_physical_device_properties2";
    ci.ppEnabledExtensionNames = ext; ci.enabledExtensionCount = n;
    ci.enabledLayerCount = 0; ci.ppEnabledLayerNames = 0;              /* layers: the host's own (VK_INSTANCE_LAYERS) */
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (ci.pApplicationInfo) app = *ci.pApplicationInfo;
    if (app.apiVersion < VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1;   /* properties2, map_memory2 */
    ci.pApplicationInfo = &app;
    if (vk_trace) for (uint32_t k = 0; k < n; k++) fprintf(stderr, "vulkan: instance extension %s\n", ext[k]);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    VkInstance inst = 0; VkResult r = create(&ci, 0, &inst);
    vk_arena_free(&a);
    VK_TRACE("vkCreateInstance", r);
    if (r == VK_SUCCESS) { load_procs(inst); rt_w32(G_MEM, ARG(2), vkh_guest(inst)); }
    RET(r, 3);
}
IMPL(vulkan_1, vkDestroyInstance) { VkInstance i = vkh_host(ARG(0)); if (i) { vkp.vkDestroyInstance(i, 0); vkh_forget(i); } RET(0, 2); }
IMPL(vulkan_1, vkEnumeratePhysicalDevices)                              /* (instance, uint32_t *count, VkPhysicalDevice *) */
{
    VkInstance i = vkh_host(ARG(0)); uint32_t n = 0;
    VkResult r = vkp.vkEnumeratePhysicalDevices(i, &n, 0);
    if (r < 0) RET(r, 3);
    VkPhysicalDevice *pd = calloc(n + 1, sizeof *pd); r = vkp.vkEnumeratePhysicalDevices(i, &n, pd);
    uint32_t out = ARG(2);
    if (!out) rt_w32(G_MEM, ARG(1), n);
    else {
        uint32_t cap = rt_r32(G_MEM, ARG(1)), w = cap < n ? cap : n;
        for (uint32_t k = 0; k < w; k++) rt_w32(G_MEM, out + 4 * k, vkh_guest(pd[k]));
        rt_w32(G_MEM, ARG(1), w); if (w < n) r = VK_INCOMPLETE;
    }
    free(pd); VK_TRACE("vkEnumeratePhysicalDevices", r); RET(r, 3);
}
IMPL(vulkan_1, vkCreateWin32SurfaceKHR)                                 /* (instance, info, allocator, VkSurfaceKHR *) */
{
    VkInstance i = vkh_host(ARG(0)); VkSurfaceKHR s = 0;
    if (!w32_host_vk_surface || w32_host_vk_surface(i, &s)) { fprintf(stderr, "vulkan: no surface for the game window\n"); RET(VK_ERROR_INITIALIZATION_FAILED, 4); }
    rt_w64(G_MEM, ARG(3), (uint64_t)s); RET(VK_SUCCESS, 4);
}
IMPL(vulkan_1, vkCreateDebugUtilsMessengerEXT) { RET(VK_ERROR_EXTENSION_NOT_PRESENT, 4); }
IMPL(vulkan_1, vkDestroyDebugUtilsMessengerEXT) { RET(0, 4); }

/* ---- device ---- */
typedef struct DevInfo { VkDevice dev; VkPhysicalDevice pd; int placed; VkDeviceSize place_align; VkPhysicalDeviceMemoryProperties mem; } DevInfo;
static DevInfo devs[8]; static int ndevs;
static DevInfo *dev_info(VkDevice d) { for (int k = 0; k < ndevs; k++) if (devs[k].dev == d) return &devs[k]; return 0; }
IMPL(vulkan_1, vkEnumerateDeviceExtensionProperties)                   /* (pd, layer, uint32_t *count, VkExtensionProperties *) */
{
    VkPhysicalDevice pd = vkh_host(ARG(0));
    if (ARG(1)) { rt_w32(G_MEM, ARG(2), 0); RET(VK_SUCCESS, 4); }
    VkResult r = vkp.vkEnumerateDeviceExtensionProperties(pd, 0, VKPTR(ARG(2)), VKPTR(ARG(3)));
    VK_TRACE("vkEnumerateDeviceExtensionProperties", r); RET(r, 4);
}
IMPL(vulkan_1, vkEnumerateDeviceLayerProperties) { rt_w32(G_MEM, ARG(1), 0); RET(VK_SUCCESS, 3); }
static int dev_has_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0; vkp.vkEnumerateDeviceExtensionProperties(pd, 0, &n, 0);
    VkExtensionProperties *e = calloc(n + 1, sizeof *e); vkp.vkEnumerateDeviceExtensionProperties(pd, 0, &n, e);
    int found = 0; for (uint32_t k = 0; k < n; k++) if (!strcmp(e[k].extensionName, name)) found = 1;
    free(e); return found;
}
IMPL(vulkan_1, vkCreateDevice)                                          /* (pd, info, allocator, VkDevice *) */
{
    VkPhysicalDevice pd = vkh_host(ARG(0));
    VkArena a = {0}; VkDeviceCreateInfo ci; cin_VkDeviceCreateInfo(&a, ARG(1), &ci);
    const char **ext = vk_arena(&a, sizeof *ext * (ci.enabledExtensionCount + 8)); uint32_t n = 0;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char *e = ci.ppEnabledExtensionNames[k];
        if (!dev_has_ext(pd, e)) { fprintf(stderr, "vulkan: device extension %s not available: left out\n", e); continue; }
        if (!has_name(ext, n, e)) ext[n++] = e;
    }
    if (portability && dev_has_ext(pd, "VK_KHR_portability_subset") && !has_name(ext, n, "VK_KHR_portability_subset")) ext[n++] = "VK_KHR_portability_subset";
    /* features the game asks for that this device lacks are switched off (logged) rather than failing */
    VkPhysicalDeviceFeatures have; vkp.vkGetPhysicalDeviceFeatures(pd, &have);
    VkPhysicalDeviceFeatures *want = (VkPhysicalDeviceFeatures *)ci.pEnabledFeatures, fix;
    if (want) {
        fix = *want; VkBool32 *w = (VkBool32 *)&fix; const VkBool32 *h = (const VkBool32 *)&have;
        for (unsigned k = 0; k < sizeof fix / sizeof *w; k++) if (w[k] && !h[k]) { fprintf(stderr, "vulkan: device feature %u not available: off\n", k); w[k] = 0; }
        ci.pEnabledFeatures = &fix;
    }
    /* mappings inside the guest's address space */
    int placed = !getenv("W32_VK_SHADOW") && dev_has_ext(pd, "VK_EXT_map_memory_placed") && dev_has_ext(pd, "VK_KHR_map_memory2") && vkp.vkGetPhysicalDeviceFeatures2;
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT pf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed) {
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pf}; vkp.vkGetPhysicalDeviceFeatures2(pd, &f2);
        placed = pf.memoryMapPlaced && pf.memoryUnmapReserve;
    }
    if (placed) {
        if (!has_name(ext, n, "VK_KHR_map_memory2")) ext[n++] = "VK_KHR_map_memory2";
        ext[n++] = "VK_EXT_map_memory_placed";
        pf.pNext = (void *)ci.pNext; pf.memoryMapRangePlaced = VK_FALSE; ci.pNext = &pf;
    }
    ci.ppEnabledExtensionNames = ext; ci.enabledExtensionCount = n; ci.enabledLayerCount = 0; ci.ppEnabledLayerNames = 0;
    VkDevice d = 0; VkResult r = vkp.vkCreateDevice(pd, &ci, 0, &d);
    vk_arena_free(&a);
    VK_TRACE("vkCreateDevice", r);
    if (r == VK_SUCCESS && ndevs < 8) {
        DevInfo *di = &devs[ndevs++]; di->dev = d; di->pd = pd; di->placed = placed; di->place_align = 4096;
        vkp.vkGetPhysicalDeviceMemoryProperties(pd, &di->mem);
        if (placed) {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pp}; vkp.vkGetPhysicalDeviceProperties2(pd, &p2);
            if (pp.minPlacedMemoryMapAlignment) di->place_align = pp.minPlacedMemoryMapAlignment;
        }
        fprintf(stderr, "vulkan: device ready; mapped memory %s\n", placed ? "placed in guest memory" : "shadowed");
        rt_w32(G_MEM, ARG(3), vkh_guest(d));
    }
    RET(r, 4);
}
IMPL(vulkan_1, vkDestroyDevice) { VkDevice d = vkh_host(ARG(0)); if (d) { vkp.vkDestroyDevice(d, 0); vkh_forget(d); } RET(0, 2); }
IMPL(vulkan_1, vkGetDeviceQueue)                                        /* (device, family, index, VkQueue *) */
{
    VkQueue q = 0; vkp.vkGetDeviceQueue(vkh_host(ARG(0)), ARG(1), ARG(2), &q);
    if (!present_family) present_family = ARG(1) + 1;
    rt_w32(G_MEM, ARG(3), vkh_guest(q)); RET(0, 4);
}
IMPL(vulkan_1, vkAllocateCommandBuffers)                                /* (device, info, VkCommandBuffer *) */
{
    VkArena a = {0}; VkCommandBufferAllocateInfo ai; cin_VkCommandBufferAllocateInfo(&a, ARG(1), &ai);
    VkCommandBuffer *cb = calloc(ai.commandBufferCount + 1, sizeof *cb);
    VkResult r = vkp.vkAllocateCommandBuffers(vkh_host(ARG(0)), &ai, cb);
    if (r == VK_SUCCESS) for (uint32_t k = 0; k < ai.commandBufferCount; k++) rt_w32(G_MEM, ARG(2) + 4 * k, vkh_guest(cb[k]));
    free(cb); vk_arena_free(&a); VK_TRACE("vkAllocateCommandBuffers", r); RET(r, 3);
}
IMPL(vulkan_1, vkFreeCommandBuffers)                                    /* (device, pool (2), count, const VkCommandBuffer *) */
{
    uint32_t n = ARG(3), p = ARG(4); VkCommandBuffer *cb = calloc(n + 1, sizeof *cb);
    for (uint32_t k = 0; k < n; k++) cb[k] = vkh_host(rt_r32(G_MEM, p + 4 * k));
    vkp.vkFreeCommandBuffers(vkh_host(ARG(0)), (VkCommandPool)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32), n, cb);
    for (uint32_t k = 0; k < n; k++) if (cb[k]) vkh_forget(cb[k]);
    free(cb); RET(0, 5);
}

/* ---- memory ---- */
typedef struct Mem { VkDeviceMemory m; VkDevice dev; VkDeviceSize size; uint32_t type; uint32_t guest, gsize; void *host; int placed; } Mem;
#define NMEM 16384
static Mem mems[NMEM]; static pthread_mutex_t mem_m = PTHREAD_MUTEX_INITIALIZER; static int nmapped_shadow;
static Mem *mem_find(VkDeviceMemory m) { for (int k = 0; k < NMEM; k++) if (mems[k].m == m) return &mems[k]; return 0; }
IMPL(vulkan_1, vkAllocateMemory)                                        /* (device, info, allocator, VkDeviceMemory *) */
{
    VkDevice d = vkh_host(ARG(0));
    VkArena a = {0}; VkMemoryAllocateInfo ai; cin_VkMemoryAllocateInfo(&a, ARG(1), &ai);
    static int checked;                                /* the guest layout, once: allocationSize at +8, type at +16 */
    if (!checked++ && vk_trace) fprintf(stderr, "vulkan: VkMemoryAllocateInfo size %llu type %u (guest dwords %08x %08x %08x %08x)\n",
        (unsigned long long)ai.allocationSize, ai.memoryTypeIndex, VKR32(ARG(1) + 8), VKR32(ARG(1) + 12), VKR32(ARG(1) + 16), VKR32(ARG(1) + 20));
    VkDeviceMemory m = 0; VkResult r = vkp.vkAllocateMemory(d, &ai, 0, &m);
    vk_arena_free(&a);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&mem_m);
        Mem *e = mem_find(0); if (e) { memset(e, 0, sizeof *e); e->m = m; e->dev = d; e->size = ai.allocationSize; e->type = ai.memoryTypeIndex; }
        pthread_mutex_unlock(&mem_m);
        if (!e) fprintf(stderr, "vulkan: too many allocations to track\n");
        rt_w64(G_MEM, ARG(3), (uint64_t)m);
    }
    VK_TRACE("vkAllocateMemory", r); RET(r, 4);
}
static void unmap(Mem *e)                                              /* mem_m held */
{
    if (!e->guest) return;
    if (e->placed) {
        VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, 0, VK_MEMORY_UNMAP_RESERVE_BIT_EXT, e->m};
        vkp.vkUnmapMemory2KHR(e->dev, &ui);
        mmap(G_MEM + e->guest, e->gsize, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);   /* ours again */
    } else {
        memcpy(e->host, G_MEM + e->guest, (size_t)e->size); vkp.vkUnmapMemory(e->dev, e->m); nmapped_shadow--;
    }
    vm_free(e->guest, 0, 0x8000);
    e->guest = 0; e->host = 0;
}
IMPL(vulkan_1, vkFreeMemory)                                            /* (device, memory (2), allocator) */
{
    VkDeviceMemory m = (VkDeviceMemory)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32);
    pthread_mutex_lock(&mem_m); Mem *e = m ? mem_find(m) : 0; if (e) { unmap(e); e->m = 0; } pthread_mutex_unlock(&mem_m);
    if (m) vkp.vkFreeMemory(vkh_host(ARG(0)), m, 0);
    RET(0, 4);
}
IMPL(vulkan_1, vkMapMemory)                                             /* (device, memory (2), offset (2), size (2), flags, void **) */
{
    VkDevice d = vkh_host(ARG(0)); DevInfo *di = dev_info(d);
    VkDeviceMemory m = (VkDeviceMemory)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32);
    VkDeviceSize off = (uint64_t)ARG(3) | (uint64_t)ARG(4) << 32;
    pthread_mutex_lock(&mem_m);
    Mem *e = mem_find(m); VkResult r = VK_SUCCESS;
    if (!e) { pthread_mutex_unlock(&mem_m); fprintf(stderr, "vulkan: vkMapMemory of unknown memory\n"); RET(VK_ERROR_MEMORY_MAP_FAILED, 9); }
    if (!e->guest) {
        VkDeviceSize al = di ? di->place_align : 4096; if (al < 65536) al = 65536;
        uint32_t gsize = (uint32_t)((e->size + al - 1) / al * al);
        uint32_t g = e->size < 0xc0000000ull ? vm_alloc(0, gsize, 0x3000, 4) : 0;
        if (!g) r = VK_ERROR_MEMORY_MAP_FAILED;
        else if (di && di->placed) {
            VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, 0, G_MEM + g};
            VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, &pi, VK_MEMORY_MAP_PLACED_BIT_EXT, m, 0, VK_WHOLE_SIZE};
            void *p = 0; r = vkp.vkMapMemory2KHR(d, &mi, &p);
            if (r == VK_SUCCESS && p != G_MEM + g) { fprintf(stderr, "vulkan: placed mapping landed at %p, not %p\n", p, (void *)(G_MEM + g)); r = VK_ERROR_MEMORY_MAP_FAILED; }
            e->placed = 1;
        } else {
            void *p = 0; r = vkp.vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &p);
            if (r == VK_SUCCESS) { e->host = p; memcpy(G_MEM + g, p, (size_t)e->size); nmapped_shadow++; }
            e->placed = 0;
        }
        if (r == VK_SUCCESS) { e->guest = g; e->gsize = gsize; }
        else if (g) vm_free(g, 0, 0x8000);
    }
    if (r == VK_SUCCESS) rt_w32(G_MEM, ARG(8), e->guest + (uint32_t)off);
    pthread_mutex_unlock(&mem_m);
    VK_TRACE("vkMapMemory", r); RET(r, 9);
}
IMPL(vulkan_1, vkUnmapMemory)                                           /* (device, memory (2)) */
{
    VkDeviceMemory m = (VkDeviceMemory)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32);
    pthread_mutex_lock(&mem_m); Mem *e = mem_find(m); if (e) unmap(e); pthread_mutex_unlock(&mem_m);
    RET(0, 3);
}
/* shadowed mappings: guest -> device memory for every mapping (before submitting), or device -> guest for host-cached
 * ones (after the GPU is known to be done) */
static void shadow_out(void)
{
    if (!nmapped_shadow) return;
    pthread_mutex_lock(&mem_m);
    for (int k = 0; k < NMEM; k++) if (mems[k].guest && !mems[k].placed) memcpy(mems[k].host, G_MEM + mems[k].guest, (size_t)mems[k].size);
    pthread_mutex_unlock(&mem_m);
}
static void shadow_in(void)
{
    if (!nmapped_shadow) return;
    pthread_mutex_lock(&mem_m);
    for (int k = 0; k < NMEM; k++) {
        Mem *e = &mems[k]; if (!e->guest || e->placed) continue;
        DevInfo *di = dev_info(e->dev);
        if (di && (di->mem.memoryTypes[e->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) memcpy(G_MEM + e->guest, e->host, (size_t)e->size);
    }
    pthread_mutex_unlock(&mem_m);
}
static VkMappedMemoryRange *ranges(VkArena *a, uint32_t n, uint32_t g)
{
    VkMappedMemoryRange *v = vk_arena(a, sizeof *v * (n ? n : 1));
    for (uint32_t k = 0; k < n; k++) cin_VkMappedMemoryRange(a, g + VKG_VkMappedMemoryRange * k, &v[k]);
    return v;
}
IMPL(vulkan_1, vkFlushMappedMemoryRanges)                               /* (device, count, ranges) */
{
    VkArena a = {0}; VkMappedMemoryRange *v = ranges(&a, ARG(1), ARG(2));
    pthread_mutex_lock(&mem_m);
    for (uint32_t k = 0; k < ARG(1); k++) {
        Mem *e = mem_find(v[k].memory); if (!e || !e->guest || e->placed) continue;
        VkDeviceSize n = v[k].size == VK_WHOLE_SIZE ? e->size - v[k].offset : v[k].size;
        memcpy((uint8_t *)e->host + v[k].offset, G_MEM + e->guest + (uint32_t)v[k].offset, (size_t)n);
    }
    pthread_mutex_unlock(&mem_m);
    VkResult r = vkp.vkFlushMappedMemoryRanges(vkh_host(ARG(0)), ARG(1), v);
    vk_arena_free(&a); RET(r, 3);
}
IMPL(vulkan_1, vkInvalidateMappedMemoryRanges)
{
    VkArena a = {0}; VkMappedMemoryRange *v = ranges(&a, ARG(1), ARG(2));
    VkResult r = vkp.vkInvalidateMappedMemoryRanges(vkh_host(ARG(0)), ARG(1), v);
    pthread_mutex_lock(&mem_m);
    for (uint32_t k = 0; k < ARG(1); k++) {
        Mem *e = mem_find(v[k].memory); if (!e || !e->guest || e->placed) continue;
        VkDeviceSize n = v[k].size == VK_WHOLE_SIZE ? e->size - v[k].offset : v[k].size;
        memcpy(G_MEM + e->guest + (uint32_t)v[k].offset, (uint8_t *)e->host + v[k].offset, (size_t)n);
    }
    pthread_mutex_unlock(&mem_m);
    vk_arena_free(&a); RET(r, 3);
}

/* ---- queues and synchronisation ---- */
IMPL(vulkan_1, vkQueueSubmit)                                           /* (queue, count, submits, fence (2)) */
{
    shadow_out();
    VkArena a = {0}; uint32_t n = ARG(1), g = ARG(2);
    VkSubmitInfo *s = vk_arena(&a, sizeof *s * (n ? n : 1));
    for (uint32_t k = 0; k < n; k++) cin_VkSubmitInfo(&a, g + VKG_VkSubmitInfo * k, &s[k]);
    VkResult r = vkp.vkQueueSubmit(vkh_host(ARG(0)), n, s, (VkFence)((uint64_t)ARG(3) | (uint64_t)ARG(4) << 32));
    vk_arena_free(&a); VK_TRACE("vkQueueSubmit", r); RET(r, 5);
}
static uint32_t presented;
struct Swap; int shot_due(uint32_t frame);
static VkSemaphore shot_copy(VkQueue q, struct Swap *s, uint32_t index, const VkPresentInfoKHR *pi);
static void shot_save(struct Swap *s, uint32_t frame);
static struct Swap *swap_find(VkSwapchainKHR sc);
IMPL(vulkan_1, vkQueuePresentKHR)                                       /* (queue, present info) */
{
    VkArena a = {0}; VkPresentInfoKHR pi; cin_VkPresentInfoKHR(&a, ARG(1), &pi);
    VkQueue q = vkh_host(ARG(0)); uint32_t frame = ++presented; struct Swap *s = 0; VkSemaphore sem = 0;
    if (getenv("W32_VK_SHOT") && pi.swapchainCount && shot_due(frame) && (s = swap_find(pi.pSwapchains[0])) && pi.pImageIndices[0] < 16)
        if ((sem = shot_copy(q, s, pi.pImageIndices[0], &pi))) { pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sem; }
    VkResult r = vkp.vkQueuePresentKHR(q, &pi);
    if (sem) shot_save(s, frame);
    vk_arena_free(&a);
    if (r >= 0 && w32_host_vk_presented) w32_host_vk_presented();
    VK_TRACE("vkQueuePresentKHR", r); RET(r, 2);
}
IMPL(vulkan_1, vkWaitForFences)                                         /* (device, count, fences, waitAll, timeout (2)) */
{
    VkResult r = vkp.vkWaitForFences(vkh_host(ARG(0)), ARG(1), VKPTR(ARG(2)), ARG(3), (uint64_t)ARG(4) | (uint64_t)ARG(5) << 32);
    if (r == VK_SUCCESS) shadow_in();
    RET(r, 6);
}
IMPL(vulkan_1, vkGetFenceStatus)                                        /* (device, fence (2)) */
{
    VkResult r = vkp.vkGetFenceStatus(vkh_host(ARG(0)), (VkFence)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32));
    if (r == VK_SUCCESS) shadow_in();
    RET(r, 3);
}
IMPL(vulkan_1, vkQueueWaitIdle) { VkResult r = vkp.vkQueueWaitIdle(vkh_host(ARG(0))); if (r == VK_SUCCESS) shadow_in(); RET(r, 1); }
IMPL(vulkan_1, vkDeviceWaitIdle) { VkResult r = vkp.vkDeviceWaitIdle(vkh_host(ARG(0))); if (r == VK_SUCCESS) shadow_in(); RET(r, 1); }

/* ---- swapchains, and screenshots of what is presented (W32_VK_SHOT=<path prefix>, W32_VK_SHOT_AT=<frame>,<frame>...:
 * <prefix><frame>.png). The image is copied to a buffer between the game's rendering and the present: a submission
 * waits on the present's semaphores and signals one the present then waits on. ---- */
#include <zlib.h>
struct Swap { VkSwapchainKHR sc; VkDevice dev; VkFormat fmt; VkExtent2D ext; VkImage img[16]; uint32_t nimg; };
static struct Swap swaps[8]; static int nswaps;
static struct Swap *swap_find(VkSwapchainKHR sc) { for (int k = 0; k < nswaps; k++) if (swaps[k].sc == sc) return &swaps[k]; return 0; }
IMPL(vulkan_1, vkCreateSwapchainKHR)                                    /* (device, info, allocator, VkSwapchainKHR *) */
{
    VkDevice d = vkh_host(ARG(0)); VkArena a = {0}; VkSwapchainCreateInfoKHR ci; cin_VkSwapchainCreateInfoKHR(&a, ARG(1), &ci);
    if (getenv("W32_VK_SHOT")) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkSwapchainKHR sc = 0; VkResult r = vkp.vkCreateSwapchainKHR(d, &ci, 0, &sc);
    fprintf(stderr, "vulkan: swapchain %ux%u format %d, %u images, present mode %d -> %d\n", ci.imageExtent.width, ci.imageExtent.height, ci.imageFormat, ci.minImageCount, ci.presentMode, r);
    if (r == VK_SUCCESS) {
        struct Swap *s = swap_find(ci.oldSwapchain); if (!s && nswaps < 8) s = &swaps[nswaps++];
        if (s) { memset(s, 0, sizeof *s); s->sc = sc; s->dev = d; s->fmt = ci.imageFormat; s->ext = ci.imageExtent; }
        rt_w64(G_MEM, ARG(3), (uint64_t)sc);
    }
    vk_arena_free(&a); RET(r, 4);
}
IMPL(vulkan_1, vkGetSwapchainImagesKHR)                                 /* (device, swapchain (2), uint32_t *count, VkImage *) */
{
    VkSwapchainKHR sc = (VkSwapchainKHR)((uint64_t)ARG(1) | (uint64_t)ARG(2) << 32);
    VkResult r = vkp.vkGetSwapchainImagesKHR(vkh_host(ARG(0)), sc, VKPTR(ARG(3)), VKPTR(ARG(4)));
    struct Swap *s = swap_find(sc);
    if (s && ARG(4) && r >= 0) { s->nimg = rt_r32(G_MEM, ARG(3)); if (s->nimg > 16) s->nimg = 16; memcpy(s->img, G_MEM + ARG(4), 8 * s->nimg); }
    RET(r, 5);
}
static void png_write(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    size_t raw_n = (size_t)(w * 3 + 1) * h; uint8_t *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; memcpy(raw + y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, w * 3); }
    uLongf zn = compressBound(raw_n); uint8_t *z = malloc(zn); compress2(z, &zn, raw, raw_n, 6);
    FILE *f = fopen(path, "wb"); if (!f) { free(raw); free(z); return; }
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    #define BE32(v) (uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)
    uint8_t ihdr[17] = {'I', 'H', 'D', 'R', BE32(w), BE32(h), 8, 2, 0, 0, 0};
    uint8_t len[4] = {BE32(13u)}; fwrite(len, 1, 4, f); fwrite(ihdr, 1, 17, f);
    uint8_t cb[4] = {BE32((uint32_t)crc32(0, ihdr, 17))}; fwrite(cb, 1, 4, f);
    uint8_t len2[4] = {BE32((uint32_t)zn)}; fwrite(len2, 1, 4, f); fwrite("IDAT", 1, 4, f); fwrite(z, 1, zn, f);
    uint8_t cb2[4] = {BE32((uint32_t)crc32(crc32(0, (const Bytef *)"IDAT", 4), z, (uInt)zn))}; fwrite(cb2, 1, 4, f);
    uint8_t len3[4] = {0, 0, 0, 0}; fwrite(len3, 1, 4, f); fwrite("IEND", 1, 4, f);
    uint8_t cb3[4] = {BE32((uint32_t)crc32(0, (const Bytef *)"IEND", 4))}; fwrite(cb3, 1, 4, f);
    #undef BE32
    fclose(f); free(raw); free(z);
}
int shot_due(uint32_t frame)
{
    const char *at = getenv("W32_VK_SHOT_AT"); if (!at) return 0;
    for (const char *p = at; *p; ) { char *e; unsigned long v = strtoul(p, &e, 10); if (e == p) break; if (v == frame) return 1; p = *e ? e + 1 : e; }
    return 0;
}
/* copy the image about to be presented; returns the semaphore the present must wait on instead, or 0 */
static struct { VkDevice d; VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore sem; VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; } shot;
static VkSemaphore shot_copy(VkQueue q, struct Swap *s, uint32_t index, const VkPresentInfoKHR *pi)
{
    VkDevice d = s->dev; VkDeviceSize need = (VkDeviceSize)s->ext.width * s->ext.height * 4;
    if (shot.d != d || shot.size < need) {
        if (shot.d == d) { vkp.vkDeviceWaitIdle(d); vkp.vkDestroyBuffer(d, shot.buf, 0); vkp.vkFreeMemory(d, shot.mem, 0); }
        else {
            VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, present_family - 1};
            vkp.vkCreateCommandPool(d, &pc, 0, &shot.pool);
            VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, shot.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            vkp.vkAllocateCommandBuffers(d, &ca, &shot.cb);
            VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkp.vkCreateFence(d, &fc, 0, &shot.fence);
            VkSemaphoreCreateInfo sc = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkp.vkCreateSemaphore(d, &sc, 0, &shot.sem);
        }
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        vkp.vkCreateBuffer(d, &bc, 0, &shot.buf);
        VkMemoryRequirements mr; vkp.vkGetBufferMemoryRequirements(d, shot.buf, &mr);
        DevInfo *di = dev_info(d); uint32_t type = 0, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t k = 0; di && k < di->mem.memoryTypeCount; k++)
            if ((mr.memoryTypeBits >> k & 1) && (di->mem.memoryTypes[k].propertyFlags & want) == want) { type = k; break; }
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, type};
        vkp.vkAllocateMemory(d, &ma, 0, &shot.mem); vkp.vkBindBufferMemory(d, shot.buf, shot.mem, 0);
        shot.d = d; shot.size = need;
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkp.vkResetCommandBuffer(shot.cb, 0); vkp.vkBeginCommandBuffer(shot.cb, &bi);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->img[index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkp.vkCmdPipelineBarrier(shot.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {s->ext.width, s->ext.height, 1}};
    vkp.vkCmdCopyImageToBuffer(shot.cb, s->img[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkp.vkCmdPipelineBarrier(shot.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b);
    vkp.vkEndCommandBuffer(shot.cb);
    VkPipelineStageFlags st[8]; for (int k = 0; k < 8; k++) st[k] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, pi->waitSemaphoreCount < 8 ? pi->waitSemaphoreCount : 8, pi->pWaitSemaphores, st, 1, &shot.cb, 1, &shot.sem};
    vkp.vkResetFences(d, 1, &shot.fence);
    return vkp.vkQueueSubmit(q, 1, &si, shot.fence) == VK_SUCCESS ? shot.sem : 0;
}
static void shot_save(struct Swap *s, uint32_t frame)
{
    vkp.vkWaitForFences(shot.d, 1, &shot.fence, VK_TRUE, ~0ull);
    void *p = 0; if (vkp.vkMapMemory(shot.d, shot.mem, 0, VK_WHOLE_SIZE, 0, &p)) return;
    uint32_t w = s->ext.width, h = s->ext.height; uint8_t *rgb = malloc((size_t)w * h * 3); const uint8_t *src = p;
    int bgr = s->fmt == VK_FORMAT_B8G8R8A8_UNORM || s->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t k = 0; k < (size_t)w * h; k++) { rgb[3 * k] = src[4 * k + (bgr ? 2 : 0)]; rgb[3 * k + 1] = src[4 * k + 1]; rgb[3 * k + 2] = src[4 * k + (bgr ? 0 : 2)]; }
    vkp.vkUnmapMemory(shot.d, shot.mem);
    char path[1024]; snprintf(path, sizeof path, "%s%u.png", getenv("W32_VK_SHOT"), frame); png_write(path, rgb, w, h); free(rgb);
    fprintf(stderr, "vulkan: frame %u (%ux%u) saved to %s\n", frame, w, h, path);
}
