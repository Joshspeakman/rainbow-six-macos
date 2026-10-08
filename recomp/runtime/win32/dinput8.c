/* DirectInput 8 for Fallout: New Vegas: the system keyboard (DIK scan codes) and mouse (relative motion, buttons,
 * wheel), in immediate (GetDeviceState) and buffered (GetDeviceData) mode. The host feeds it from its input events
 * (w32_di_key / w32_di_mouse_*); devices other than the keyboard and mouse are reported as absent. */
#include "w32.h"
#include <pthread.h>

#define DI_OK 0
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DI_BUFFEROVERFLOW 1u
#define E_NOINTERFACE 0x80004002u
#define QN 512

typedef struct { uint32_t ofs, data, time, seq; } DiEvent;
typedef struct Dev {
    int mouse, acquired, buffer_size;
    DiEvent q[QN]; int qh, qt, overflow;
    int32_t dx, dy, dz;                    /* mouse motion since the last GetDeviceState */
} Dev;
static pthread_mutex_t di_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t keys[256], mbuttons[8];
static Dev *kbd, *mouse; static uint32_t seq;
static uint32_t vt_di, vt_dev, di_obj;

static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void enqueue(Dev *d, uint32_t ofs, uint32_t data)          /* di_lock held */
{
    if (!d || !d->acquired || !d->buffer_size) return;
    int n = (d->qt - d->qh + QN) % QN;
    if (n >= d->buffer_size || n >= QN - 1) { d->overflow = 1; return; }
    d->q[d->qt] = (DiEvent){ofs, data, now_ms(), ++seq}; d->qt = (d->qt + 1) % QN;
}
/* ---- host side ---- */
void w32_di_key(int dik, int down)
{
    if (dik <= 0 || dik > 255) return;
    pthread_mutex_lock(&di_lock);
    uint8_t v = down ? 0x80 : 0;
    if (keys[dik] != v) { keys[dik] = v; enqueue(kbd, (uint32_t)dik, v); }
    pthread_mutex_unlock(&di_lock);
}
void w32_di_mouse_move(int dx, int dy)
{
    pthread_mutex_lock(&di_lock);
    if (mouse && mouse->acquired) { mouse->dx += dx; mouse->dy += dy; if (dx) enqueue(mouse, 0, (uint32_t)dx); if (dy) enqueue(mouse, 4, (uint32_t)dy); }
    pthread_mutex_unlock(&di_lock);
}
void w32_di_mouse_wheel(int dz)
{
    pthread_mutex_lock(&di_lock);
    if (mouse && mouse->acquired) { mouse->dz += dz; enqueue(mouse, 8, (uint32_t)dz); }
    pthread_mutex_unlock(&di_lock);
}
void w32_di_mouse_button(int b, int down)
{
    if (b < 0 || b > 7) return;
    pthread_mutex_lock(&di_lock);
    uint8_t v = down ? 0x80 : 0;
    if (mbuttons[b] != v) { mbuttons[b] = v; enqueue(mouse, 12u + (uint32_t)b, v); }
    pthread_mutex_unlock(&di_lock);
}
void w32_di_release_all(void)                                       /* focus lost: every key and button up */
{
    for (int k = 1; k < 256; k++) if (keys[k]) w32_di_key(k, 0);
    for (int b = 0; b < 8; b++) if (mbuttons[b]) w32_di_mouse_button(b, 0);
}

/* ---- IDirectInputDevice8A ---- */
static Dev *dev_of(Ctx *c) { return *(Dev **)GP(ARG(0) + 4); }
static void d_QueryInterface(Ctx *c) { rt_w32(G_MEM, ARG(2), ARG(0)); RET(DI_OK, 3); }
static void d_AddRef(Ctx *c) { RET(1, 1); }
static void d_Release(Ctx *c) { RET(0, 1); }
static void d_GetCapabilities(Ctx *c)                 /* DIDEVCAPS: size, flags, type, axes, buttons, povs */
{
    Dev *d = dev_of(c); uint32_t p = ARG(1), n = rt_r32(G_MEM, p);
    memset(GP(p + 4), 0, n > 4 && n < 256 ? n - 4 : 40);
    rt_w32(G_MEM, p + 4, 5);                               /* DIDC_ATTACHED | DIDC_POLLEDDEVICE */
    rt_w32(G_MEM, p + 8, d->mouse ? 0x112 : 0x413);        /* DI8DEVTYPE_MOUSE | subtype / DI8DEVTYPE_KEYBOARD | PCENH */
    rt_w32(G_MEM, p + 12, d->mouse ? 3 : 0); rt_w32(G_MEM, p + 16, d->mouse ? 8 : 128);
    RET(DI_OK, 2);
}
static void d_EnumObjects(Ctx *c) { RET(DI_OK, 4); }
static void d_GetProperty(Ctx *c)
{
    Dev *d = dev_of(c); uint32_t guid = ARG(1), p = ARG(2);
    if (guid == 1) { rt_w32(G_MEM, p + 16, (uint32_t)d->buffer_size); RET(DI_OK, 3); }     /* DIPROP_BUFFERSIZE */
    if (guid == 2) { rt_w32(G_MEM, p + 16, 0); RET(DI_OK, 3); }                             /* DIPROP_AXISMODE: relative */
    RET(DIERR_INVALIDPARAM, 3);
}
static void d_SetProperty(Ctx *c)                     /* DIPROPDWORD: header (16 bytes) then the value */
{
    Dev *d = dev_of(c); uint32_t guid = ARG(1), p = ARG(2);
    if (guid == 1) { pthread_mutex_lock(&di_lock); int n = (int)rt_r32(G_MEM, p + 16); d->buffer_size = n > QN - 1 ? QN - 1 : n; d->qh = d->qt = 0; pthread_mutex_unlock(&di_lock); }
    RET(DI_OK, 3);
}
static void d_Acquire(Ctx *c)
{
    Dev *d = dev_of(c); pthread_mutex_lock(&di_lock);
    int was = d->acquired; d->acquired = 1; if (!was) { d->qh = d->qt = 0; d->dx = d->dy = d->dz = 0; }
    pthread_mutex_unlock(&di_lock); RET(was ? 1u /* S_FALSE */ : DI_OK, 1);
}
static void d_Unacquire(Ctx *c) { Dev *d = dev_of(c); pthread_mutex_lock(&di_lock); d->acquired = 0; pthread_mutex_unlock(&di_lock); RET(DI_OK, 1); }
static void d_GetDeviceState(Ctx *c)                  /* keyboard: 256 bytes; mouse: DIMOUSESTATE (16) or DIMOUSESTATE2 (20) */
{
    Dev *d = dev_of(c); uint32_t n = ARG(1), p = ARG(2);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    pthread_mutex_lock(&di_lock);
    if (!d->mouse) memcpy(GP(p), keys, n < 256 ? n : 256);
    else {
        int32_t v[3] = {d->dx, d->dy, d->dz}; d->dx = d->dy = d->dz = 0;
        memset(GP(p), 0, n); memcpy(GP(p), v, n < 12 ? n : 12);
        if (n > 12) memcpy(GP(p + 12), mbuttons, n - 12 < 8 ? n - 12 : 8);
    }
    pthread_mutex_unlock(&di_lock); RET(DI_OK, 3);
}
static void d_GetDeviceData(Ctx *c)                   /* (object data size, DIDEVICEOBJECTDATA *, in/out count, flags) */
{
    Dev *d = dev_of(c); uint32_t sz = ARG(1), out = ARG(2), pn = ARG(3), flags = ARG(4);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    if (!d->buffer_size) RET(DIERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&di_lock);
    uint32_t want = rt_r32(G_MEM, pn), got = 0; int peek = flags & 1;   /* DIGDD_PEEK */
    int h = d->qh;
    while (h != d->qt && got < want) {
        if (out) { memset(GP(out + got * sz), 0, sz); memcpy(GP(out + got * sz), &d->q[h], sz < 16 ? sz : 16); }
        h = (h + 1) % QN; got++;
    }
    if (!out && want == 0xffffffffu) { got = 0; for (h = d->qh; h != d->qt; h = (h + 1) % QN) got++; if (!peek) h = d->qt; }   /* flush */
    if (!peek) d->qh = h;
    uint32_t r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK; if (!peek) d->overflow = 0;
    pthread_mutex_unlock(&di_lock);
    rt_w32(G_MEM, pn, got); RET(r, 5);
}
static void d_SetDataFormat(Ctx *c) { RET(DI_OK, 2); }
static void d_SetEventNotification(Ctx *c) { RET(DI_OK, 2); }
static void d_SetCooperativeLevel(Ctx *c) { RET(DI_OK, 3); }
static void d_GetObjectInfo(Ctx *c) { RET(DIERR_INVALIDPARAM, 4); }
static void d_GetDeviceInfo(Ctx *c)                   /* DIDEVICEINSTANCEA: size, instance and product GUIDs, type, names */
{
    Dev *d = dev_of(c); uint32_t p = ARG(1), n = rt_r32(G_MEM, p);
    memset(GP(p + 4), 0, n > 4 && n < 1024 ? n - 4 : 0);
    rt_w32(G_MEM, p + 36, d->mouse ? 0x112 : 0x413);
    snprintf((char *)GP(p + 40), 260, "%s", d->mouse ? "Mouse" : "Keyboard"); snprintf((char *)GP(p + 300), 260, "%s", d->mouse ? "Mouse" : "Keyboard");
    RET(DI_OK, 2);
}
static void d_Poll(Ctx *c) { RET(DI_OK, 1); }
#define UNSUP(name, n) static void name(Ctx *c) { RET(0x80004001u, n); }   /* E_NOTIMPL */
UNSUP(d_RunControlPanel, 3) UNSUP(d_Initialize, 4) UNSUP(d_CreateEffect, 5) UNSUP(d_EnumEffects, 4) UNSUP(d_GetEffectInfo, 3)
UNSUP(d_GetForceFeedbackState, 2) UNSUP(d_SendForceFeedbackCommand, 2) UNSUP(d_EnumCreatedEffectObjects, 4) UNSUP(d_Escape, 2)
UNSUP(d_SendDeviceData, 5) UNSUP(d_EnumEffectsInFile, 5) UNSUP(d_WriteEffectToFile, 5) UNSUP(d_BuildActionMap, 4)
UNSUP(d_SetActionMap, 4) UNSUP(d_GetImageInfo, 2)

/* ---- IDirectInput8A ---- */
static void i_QueryInterface(Ctx *c) { rt_w32(G_MEM, ARG(2), ARG(0)); RET(DI_OK, 3); }
static void i_AddRef(Ctx *c) { RET(1, 1); }
static void i_Release(Ctx *c) { RET(0, 1); }
static void i_CreateDevice(Ctx *c)                    /* (this, GUID *, device **, outer): GUID_SysMouse 6f1d2b60, GUID_SysKeyboard 6f1d2b61 */
{
    uint32_t g = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (g != 0x6f1d2b60u && g != 0x6f1d2b61u) RET(DIERR_DEVICENOTREG, 4);
    Dev *d = calloc(1, sizeof *d); d->mouse = g == 0x6f1d2b60u;
    pthread_mutex_lock(&di_lock); if (d->mouse) mouse = d; else kbd = d; pthread_mutex_unlock(&di_lock);
    uint32_t o = heap_alloc(w32_process_heap, 8, 4 + sizeof(Dev *)); rt_w32(G_MEM, o, vt_dev); memcpy(GP(o + 4), &d, sizeof d);
    rt_w32(G_MEM, out, o); RET(DI_OK, 4);
}
static void i_EnumDevices(Ctx *c) { RET(DI_OK, 5); }                           /* no game controllers through DirectInput */
static void i_GetDeviceStatus(Ctx *c) { uint32_t g = rt_r32(G_MEM, ARG(1)); RET(g == 0x6f1d2b60u || g == 0x6f1d2b61u ? DI_OK : 1u, 2); }
UNSUP(i_RunControlPanel, 3) UNSUP(i_Initialize, 3) UNSUP(i_FindDevice, 4) UNSUP(i_EnumDevicesBySemantics, 6) UNSUP(i_ConfigureDevices, 5)

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "dinput8!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
IMPL(dinput8, DirectInput8Create)                     /* (hinstance, version, IID *, out **, outer) */
{
    if (!vt_di) {
        static void (*const di[])(Ctx *) = { i_QueryInterface, i_AddRef, i_Release, i_CreateDevice, i_EnumDevices, i_GetDeviceStatus,
            i_RunControlPanel, i_Initialize, i_FindDevice, i_EnumDevicesBySemantics, i_ConfigureDevices };
        static void (*const dv[])(Ctx *) = { d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects, d_GetProperty,
            d_SetProperty, d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData, d_SetDataFormat, d_SetEventNotification,
            d_SetCooperativeLevel, d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize, d_CreateEffect, d_EnumEffects,
            d_GetEffectInfo, d_GetForceFeedbackState, d_SendForceFeedbackCommand, d_EnumCreatedEffectObjects, d_Escape, d_Poll,
            d_SendDeviceData, d_EnumEffectsInFile, d_WriteEffectToFile, d_BuildActionMap, d_SetActionMap, d_GetImageInfo };
        _Static_assert(sizeof di / sizeof *di == 11 && sizeof dv / sizeof *dv == 32, "vtable sizes");
        vt_di = vtable("IDirectInput8A", di, 11); vt_dev = vtable("IDirectInputDevice8A", dv, 32);
        di_obj = heap_alloc(w32_process_heap, 8, 8); rt_w32(G_MEM, di_obj, vt_di);
    }
    rt_w32(G_MEM, ARG(3), di_obj); RET(DI_OK, 5);
}
