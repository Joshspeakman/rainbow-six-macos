/* steam_api.dll: the Steamworks client as an offline Steam that owns the game. The game's flat imports start the API
 * and hand out interfaces (SteamInternal_FindOrCreateUserInterface); each interface is a C++ object whose methods are
 * thiscall (this in ecx, the callee pops its arguments), so every method served here declares its argument count.
 * Methods the game calls that are not listed stop with their interface and vtable slot named (steam_method_missing). */
#include "w32.h"
#include <string.h>

#define STEAM_ID 76561197960287930ull                  /* an individual account in the public universe */
#define SIMS_APP 3314060u

typedef struct { const char *iface; int slot, nargs; void (*fn)(Ctx *c); } Method;
static uint32_t str_const(const char *s)              /* a guest copy of a string that lives forever */
{
    uint32_t p = heap_alloc(w32_process_heap, 8, (uint32_t)strlen(s) + 1); strcpy((char *)GP(p), s); return p;
}
#define THIS_RET(v, n) do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (n); return; } while (0)

/* ISteamUser */
static void user_hsteamuser(Ctx *c) { THIS_RET(1, 0); }
static void user_logged_on(Ctx *c) { THIS_RET(1, 0); }
static void user_steam_id(Ctx *c)                     /* CSteamID GetSteamID(): returned through a hidden pointer */
{
    uint32_t out = ARG(0); rt_w32(G_MEM, out, (uint32_t)STEAM_ID); rt_w32(G_MEM, out + 4, (uint32_t)(STEAM_ID >> 32)); THIS_RET(out, 1);
}
/* ISteamApps */
static void apps_true(Ctx *c) { THIS_RET(1, 0); }
static void apps_false(Ctx *c) { THIS_RET(0, 0); }
static void apps_language(Ctx *c) { static uint32_t s; if (!s) s = str_const("english"); THIS_RET(s, 0); }
static void apps_languages(Ctx *c) { static uint32_t s; if (!s) s = str_const("english"); THIS_RET(s, 0); }
static void apps_subscribed_app(Ctx *c) { THIS_RET(1, 1); }        /* every app and DLC: the collection includes them all */
static void apps_dlc_installed(Ctx *c) { THIS_RET(1, 1); }

static const Method methods[] = {
    {"SteamUser", 0, 0, user_hsteamuser}, {"SteamUser", 1, 0, user_logged_on}, {"SteamUser", 2, 1, user_steam_id},
    {"STEAMAPPS_INTERFACE_VERSION", 0, 0, apps_true},            /* BIsSubscribed */
    {"STEAMAPPS_INTERFACE_VERSION", 1, 0, apps_false},           /* BIsLowViolence */
    {"STEAMAPPS_INTERFACE_VERSION", 2, 0, apps_false},           /* BIsCybercafe */
    {"STEAMAPPS_INTERFACE_VERSION", 3, 0, apps_false},           /* BIsVACBanned */
    {"STEAMAPPS_INTERFACE_VERSION", 4, 0, apps_language},        /* GetCurrentGameLanguage */
    {"STEAMAPPS_INTERFACE_VERSION", 5, 0, apps_languages},       /* GetAvailableGameLanguages */
    {"STEAMAPPS_INTERFACE_VERSION", 6, 1, apps_subscribed_app},  /* BIsSubscribedApp(AppId_t) */
    {"STEAMAPPS_INTERFACE_VERSION", 7, 1, apps_dlc_installed},   /* BIsDlcInstalled(AppId_t) */
};

/* a slot nobody has implemented: name it and stop (the argument count is unknown, so returning would corrupt the stack) */
static void steam_method_missing(Ctx *c)
{
    extern __thread uint32_t w32_cur_import; const char *w32_thunk_name(uint32_t);
    static __thread char m[160]; snprintf(m, sizeof m, "Steam method not implemented: %s", w32_thunk_name(w32_cur_import));
    rt_unhandled(c, rt_r32(G_MEM, c->esp), m);
}

#define SLOTS 96
static struct { char name[64]; uint32_t obj; } ifaces[32]; static int nifaces;
static pthread_mutex_t steam_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t interface_for(const char *version)
{
    pthread_mutex_lock(&steam_m);
    for (int k = 0; k < nifaces; k++) if (!strcmp(ifaces[k].name, version)) { pthread_mutex_unlock(&steam_m); return ifaces[k].obj; }
    uint32_t obj = heap_alloc(w32_process_heap, 8, 8 + 4 * SLOTS), vt = obj + 8;
    rt_w32(G_MEM, obj, vt);
    for (int s = 0; s < SLOTS; s++) {
        void (*fn)(Ctx *) = steam_method_missing;
        for (size_t m = 0; m < sizeof methods / sizeof *methods; m++)
            if (methods[m].slot == s && !strncmp(version, methods[m].iface, strlen(methods[m].iface))) fn = methods[m].fn;
        char nm[96]; snprintf(nm, sizeof nm, "steam_api!%s.%d", version, s);
        rt_w32(G_MEM, vt + 4u * (uint32_t)s, w32_thunk_register(nm, fn));
    }
    if (nifaces < 32) { snprintf(ifaces[nifaces].name, sizeof ifaces[nifaces].name, "%s", version); ifaces[nifaces++].obj = obj; }
    pthread_mutex_unlock(&steam_m);
    if (getenv("W32_STEAMLOG")) fprintf(stderr, "steam: interface %s -> %08x\n", version, obj);
    return obj;
}

/* the flat API (cdecl) */
IMPL(steam_api, SteamInternal_SteamAPI_Init)          /* (interface versions, SteamErrMsg *): k_ESteamAPIInitResult_OK */
{
    if (getenv("W32_STEAMLOG")) fprintf(stderr, "steam: init (app %u, offline)\n", SIMS_APP);
    if (ARG(1)) rt_w8(G_MEM, ARG(1), 0);
    RETC(0);
}
IMPL(steam_api, SteamAPI_GetHSteamUser) { RETC(1); }
/* ContextInit(data): data = { void (*fill)(void *ptr), counter, ptr }: fill(&ptr) once, return &ptr */
IMPL(steam_api, SteamInternal_ContextInit)
{
    uint32_t d = ARG(0);
    if (rt_r32(G_MEM, d + 4) != 1) {
        rt_w32(G_MEM, d + 4, 1);
        uint32_t a[1] = {d + 8}; w32_callback(c, rt_r32(G_MEM, d), 1, a);   /* cdecl: w32_callback restores esp */
    }
    RETC(d + 8);
}
IMPL(steam_api, SteamInternal_FindOrCreateUserInterface) { RETC(interface_for(GS(ARG(1)))); }
IMPL(steam_api, SteamAPI_RegisterCallback) { RETC(0); }      /* no callbacks are ever posted offline */
IMPL(steam_api, SteamAPI_UnregisterCallback) { RETC(0); }
IMPL(steam_api, SteamAPI_RunCallbacks) { RETC(0); }
IMPL(steam_api, SteamAPI_Shutdown) { RETC(0); }
