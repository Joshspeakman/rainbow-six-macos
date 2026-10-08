/* USER32: an internal window manager (classes, windows, message queues, focus, input state) with the semantics the
 * game relies on. A host backend (macOS window, input) attaches through the w32_host_* hooks; without one (the
 * harness) everything stays headless and deterministic. */
#include "w32.h"
#include <unistd.h>
#include <time.h>
#include "../platform.h"
#ifdef __linux__
#include "display_modes.h"
#endif

int w32_screen_w = 1920, w32_screen_h = 1080; double w32_screen_scale = 1.0;   /* set by the host from the main screen */
int w32_screen_hz = 60;
enum { WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8,
       WM_PAINT = 0xf, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1c,
       WM_SETCURSOR = 0x20, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47,
       WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCHITTEST = 0x84, WM_NCACTIVATE = 0x86 };

/* ---- classes ---- */
typedef struct WClass { char name[64]; uint32_t atom, style, wndproc, cls_extra, wnd_extra, hinst, icon, cursor, brush; } WClass;
static WClass classes[64]; static int nclasses;
static WClass *find_class(uint32_t name_or_atom)
{
    for (int k = 0; k < nclasses; k++)
        if (name_or_atom < 0x10000 ? classes[k].atom == name_or_atom : !strcasecmp(classes[k].name, GS(name_or_atom))) return &classes[k];
    return 0;
}
IMPL(user32, RegisterClassA)
{
    uint32_t wc = ARG(0); uint32_t f[10]; memcpy(f, GP(wc), 40);
    if (nclasses == 64) RET(0, 1);
    WClass *k = &classes[nclasses++]; memset(k, 0, sizeof *k);
    k->style = f[0]; k->wndproc = f[1]; k->cls_extra = f[2]; k->wnd_extra = f[3]; k->hinst = f[4]; k->icon = f[5]; k->cursor = f[6]; k->brush = f[7];
    if (f[9] < 0x10000) snprintf(k->name, sizeof k->name, "#%u", f[9]); else snprintf(k->name, sizeof k->name, "%s", GS(f[9]));
    k->atom = 0xc000 + (uint32_t)nclasses;
    RET(k->atom, 1);
}
IMPL(user32, UnregisterClassA) { RET(1, 2); }
IMPL(user32, SetClassLongA) { RET(0, 3); }

/* ---- windows ---- */
typedef struct Win {
    uint32_t hwnd, parent, owner, style, exstyle, wndproc, userdata, id, hinst, tid; WClass *cls;
    int x, y, w, h, visible, enabled, iconic, invalid; char text[256]; uint32_t extra[16];
} Win;
#define MAXW 256
static Win wins[MAXW];
static uint32_t focus_hwnd, active_hwnd;
static Win *W(uint32_t h) { uint32_t k = (h - 0x10010) / 4; return (h >= 0x10010 && !(h & 3) && k < MAXW && wins[k].hwnd == h) ? &wins[k] : 0; }
static uint32_t send(Ctx *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = W(hwnd); if (!w || !w->wndproc) return 0;
    uint32_t a[4] = {hwnd, msg, wp, lp};
    return w32_callback(c, w->wndproc, 4, a);
}
/* non-client metrics for overlapped windows: caption 19 + frame 4 */
static void nc_size(uint32_t style, uint32_t exstyle, int *l, int *t, int *r, int *b)
{
    (void)exstyle; *l = *t = *r = *b = 0;
    if (style & 0x00c00000u) *t += 19;                                      /* WS_CAPTION */
    if (style & 0x00040000u) { *l += 4; *t += 4; *r += 4; *b += 4; }        /* WS_THICKFRAME */
    else if (style & 0x00c00000u || style & 0x00400000u) { *l += 3; *t += 3; *r += 3; *b += 3; }   /* fixed/dialog frame */
    else if (style & 0x00800000u) { *l += 1; *t += 1; *r += 1; *b += 1; }   /* WS_BORDER */
}
IMPL(user32, AdjustWindowRect)
{
    uint32_t r = ARG(0); int l, t, rr, b; nc_size(ARG(1), 0, &l, &t, &rr, &b);
    if (ARG(2)) t += 19;
    rt_w32(G_MEM, r, rt_r32(G_MEM, r) - (uint32_t)l); rt_w32(G_MEM, r + 4, rt_r32(G_MEM, r + 4) - (uint32_t)t);
    rt_w32(G_MEM, r + 8, rt_r32(G_MEM, r + 8) + (uint32_t)rr); rt_w32(G_MEM, r + 12, rt_r32(G_MEM, r + 12) + (uint32_t)b);
    RET(1, 3);
}
IMPL(user32, AdjustWindowRectEx)                      /* (rect, style, menu, extended style) */
{
    uint32_t r = ARG(0); int l, t, rr, b; nc_size(ARG(1), 0, &l, &t, &rr, &b);
    if (ARG(2)) t += 19;
    rt_w32(G_MEM, r, rt_r32(G_MEM, r) - (uint32_t)l); rt_w32(G_MEM, r + 4, rt_r32(G_MEM, r + 4) - (uint32_t)t);
    rt_w32(G_MEM, r + 8, rt_r32(G_MEM, r + 8) + (uint32_t)rr); rt_w32(G_MEM, r + 12, rt_r32(G_MEM, r + 12) + (uint32_t)b);
    RET(1, 4);
}
void (*w32_host_window_created)(uint32_t hwnd, int x, int y, int w, int h, uint32_t style);
void (*w32_host_window_changed)(uint32_t hwnd, int x, int y, int w, int h, int visible);
IMPL(user32, CreateWindowExA)
{
    uint32_t ex = ARG(0), cname = ARG(1), title = ARG(2), style = ARG(3), parent = ARG(8), menu = ARG(9), hinst = ARG(10), param = ARG(11);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    WClass *k = find_class(cname);
    if (!k) { w32_set_last_error(c, 1407); RET(0, 12); }
    int slot = -1; for (int j = 0; j < MAXW; j++) if (!wins[j].hwnd) { slot = j; break; }
    if (slot < 0) RET(0, 12);
    if (x == (int)0x80000000) { x = 0; y = 0; }
    if (w == (int)0x80000000) { w = w32_screen_w; h = w32_screen_h; }
    Win *wn = &wins[slot]; memset(wn, 0, sizeof *wn);
    wn->hwnd = 0x10010 + 4 * (uint32_t)slot; wn->parent = (style & 0x40000000u) ? parent : 0; wn->owner = (style & 0x40000000u) ? 0 : parent;
    wn->style = style; wn->exstyle = ex; wn->wndproc = k->wndproc; wn->cls = k; wn->id = menu; wn->hinst = hinst;
    wn->x = x; wn->y = y; wn->w = w; wn->h = h; wn->enabled = !(style & 0x08000000u); wn->tid = w32_tid(c);
    if (title) snprintf(wn->text, sizeof wn->text, "%s", GS(title));
    /* CREATESTRUCTA on the guest stack area below esp */
    uint32_t cs = c->esp - 0x200, f[12] = {param, hinst, menu, parent, (uint32_t)h, (uint32_t)w, (uint32_t)y, (uint32_t)x, style, title, cname, ex};
    memcpy(GP(cs), f, sizeof f);
    uint32_t save = c->esp; c->esp -= 0x240;
    if (!send(c, wn->hwnd, WM_NCCREATE, 0, cs)) { c->esp = save; wn->hwnd = 0; RET(0, 12); }
    if ((int32_t)send(c, wn->hwnd, WM_CREATE, 0, cs) == -1) { c->esp = save; wn->hwnd = 0; RET(0, 12); }
    c->esp = save;
    if (w32_host_window_created && !wn->parent) w32_host_window_created(wn->hwnd, x, y, w, h, style);
    if (style & 0x10000000u) {                                              /* WS_VISIBLE: shown (and, top-level, activated) */
        wn->visible = 1;
        if (!wn->parent) {
            active_hwnd = focus_hwnd = wn->hwnd; c->esp -= 0x40;
            send(c, wn->hwnd, WM_SHOWWINDOW, 1, 0); send(c, wn->hwnd, WM_ACTIVATEAPP, 1, 0); send(c, wn->hwnd, WM_ACTIVATE, 1, 0);
            send(c, wn->hwnd, WM_SETFOCUS, 0, 0); send(c, wn->hwnd, WM_SIZE, 0, ((uint32_t)wn->h << 16) | (uint32_t)(wn->w & 0xffff));
            c->esp += 0x40; wn->invalid = 1;
            if (w32_host_window_changed) w32_host_window_changed(wn->hwnd, wn->x, wn->y, wn->w, wn->h, 1);
        }
    }
    RET(wn->hwnd, 12);
}
IMPL(user32, DestroyWindow)
{
    Win *w = W(ARG(0)); if (!w) RET(0, 1);
    send(c, w->hwnd, WM_DESTROY, 0, 0); send(c, w->hwnd, WM_NCDESTROY, 0, 0);
    if (focus_hwnd == w->hwnd) focus_hwnd = 0;
    if (active_hwnd == w->hwnd) active_hwnd = 0;
    w->hwnd = 0; RET(1, 1);
}
IMPL(user32, ShowWindow)
{
    Win *w = W(ARG(0)); uint32_t cmd = ARG(1); if (!w) RET(0, 2);
    int was = w->visible;
    w->visible = cmd != 0; w->iconic = cmd == 2 || cmd == 6 || cmd == 7;
    if (w->visible != was) send(c, w->hwnd, WM_SHOWWINDOW, (uint32_t)w->visible, 0);
    /* activation messages only when the activation changes, as on Windows: a window that shows itself while handling
     * WM_ACTIVATEAPP (The Sims does) would otherwise recurse until its stack runs out */
    if (w->visible && !w->parent && (cmd == 1 || cmd == 5 || cmd == 3 || cmd == 10) && (active_hwnd != w->hwnd || w->visible != was)) {
        int activate = active_hwnd != w->hwnd;
        active_hwnd = focus_hwnd = w->hwnd;
        if (activate) { send(c, w->hwnd, WM_ACTIVATEAPP, 1, 0); send(c, w->hwnd, WM_ACTIVATE, 1, 0); send(c, w->hwnd, WM_SETFOCUS, 0, 0); }
        send(c, w->hwnd, WM_SIZE, 0, ((uint32_t)w->h << 16) | (uint32_t)(w->w & 0xffff));
        w->invalid = 1;
    }
    if (w32_host_window_changed && !w->parent) w32_host_window_changed(w->hwnd, w->x, w->y, w->w, w->h, w->visible);
    RET(was, 2);
}
IMPL(user32, UpdateWindow) { Win *w = W(ARG(0)); if (w && w->invalid) { w->invalid = 0; send(c, w->hwnd, WM_PAINT, 0, 0); } RET(w != 0, 1); }
IMPL(user32, InvalidateRect) { Win *w = W(ARG(0)); if (w) w->invalid = 1; RET(1, 3); }
static void move(Ctx *c, Win *w, int x, int y, int cx, int cy, int nomove, int nosize)
{
    if (!nomove) { w->x = x; w->y = y; }
    if (!nosize) { w->w = cx; w->h = cy; send(c, w->hwnd, WM_SIZE, 0, ((uint32_t)cy << 16) | (uint32_t)(cx & 0xffff)); }
    if (w32_host_window_changed && !w->parent) w32_host_window_changed(w->hwnd, w->x, w->y, w->w, w->h, w->visible);
}
/* for other modules (DirectDraw's SetDisplayMode resizes the game window, as Wine's emulated mode change does) */
void w32_set_window_pos(Ctx *c, uint32_t hwnd, int x, int y, int cx, int cy, uint32_t fl)
{
    Win *w = W(hwnd); if (!w) return;
    move(c, w, x, y, cx, cy, fl & 2, fl & 1);
    if (fl & 0x40) w->visible = 1;
}
IMPL(user32, SetWindowPos)
{
    Win *w = W(ARG(0)); uint32_t fl = ARG(6); if (!w) RET(0, 7);
    move(c, w, (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), fl & 2, fl & 1);
    if (fl & 0x40) w->visible = 1;
    if (fl & 0x80) w->visible = 0;
    RET(1, 7);
}
IMPL(user32, MoveWindow) { Win *w = W(ARG(0)); if (!w) RET(0, 6); move(c, w, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), 0, 0); RET(1, 6); }
static void client_origin(Win *w, int *x, int *y)
{
    int l, t, r, b; nc_size(w->style, w->exstyle, &l, &t, &r, &b);
    *x = w->x + l; *y = w->y + t;
    for (Win *p = W(w->parent); p; p = W(p->parent)) { int px, py; nc_size(p->style, p->exstyle, &l, &t, &r, &b); px = p->x + l; py = p->y + t; *x += px; *y += py; }
}
IMPL(user32, GetWindowRect)
{
    Win *w = W(ARG(0)); uint32_t r = ARG(1);
    if (!w) { if (ARG(0) == 0x10000) { uint32_t v[4] = {0, 0, (uint32_t)w32_screen_w, (uint32_t)w32_screen_h}; memcpy(GP(r), v, 16); RET(1, 2); } RET(0, 2); }
    int x = w->x, y = w->y;
    if (w->parent) { int ox, oy; client_origin(W(w->parent), &ox, &oy); x += ox; y += oy; }
    uint32_t v[4] = {(uint32_t)x, (uint32_t)y, (uint32_t)(x + w->w), (uint32_t)(y + w->h)}; memcpy(GP(r), v, 16);
    RET(1, 2);
}
IMPL(user32, GetClientRect)
{
    Win *w = W(ARG(0)); uint32_t r = ARG(1);
    if (!w) { if (ARG(0) == 0x10000) { uint32_t v[4] = {0, 0, (uint32_t)w32_screen_w, (uint32_t)w32_screen_h}; memcpy(GP(r), v, 16); RET(1, 2); } RET(0, 2); }
    int l, t, rr, b; nc_size(w->style, w->exstyle, &l, &t, &rr, &b);
    uint32_t v[4] = {0, 0, (uint32_t)(w->w - l - rr), (uint32_t)(w->h - t - b)}; memcpy(GP(r), v, 16);
    RET(1, 2);
}
IMPL(user32, ClientToScreen)
{
    Win *w = W(ARG(0)); uint32_t p = ARG(1); if (!w) RET(0, 2);
    int x, y; client_origin(w, &x, &y);
    rt_w32(G_MEM, p, rt_r32(G_MEM, p) + (uint32_t)x); rt_w32(G_MEM, p + 4, rt_r32(G_MEM, p + 4) + (uint32_t)y); RET(1, 2);
}
IMPL(user32, ScreenToClient)
{
    Win *w = W(ARG(0)); uint32_t p = ARG(1); if (!w) RET(0, 2);
    int x, y; client_origin(w, &x, &y);
    rt_w32(G_MEM, p, rt_r32(G_MEM, p) - (uint32_t)x); rt_w32(G_MEM, p + 4, rt_r32(G_MEM, p + 4) - (uint32_t)y); RET(1, 2);
}
IMPL(user32, MapWindowPoints)
{
    int fx = 0, fy = 0, tx = 0, ty = 0; Win *a = W(ARG(0)), *b = W(ARG(1));
    if (a) client_origin(a, &fx, &fy);
    if (b) client_origin(b, &tx, &ty);
    uint32_t p = ARG(2), n = ARG(3);
    for (uint32_t k = 0; k < n; k++) { rt_w32(G_MEM, p + 8 * k, rt_r32(G_MEM, p + 8 * k) + (uint32_t)(fx - tx)); rt_w32(G_MEM, p + 8 * k + 4, rt_r32(G_MEM, p + 8 * k + 4) + (uint32_t)(fy - ty)); }
    RET(((uint32_t)(fy - ty) << 16) | ((uint32_t)(fx - tx) & 0xffff), 4);
}
IMPL(user32, GetWindowLongA)
{
    Win *w = W(ARG(0)); int32_t i = (int32_t)ARG(1); if (!w) RET(0, 2);
    switch (i) {
    case -4: RET(w->wndproc, 2); case -6: RET(w->hinst, 2); case -8: RET(w->parent, 2); case -12: RET(w->id, 2);
    case -16: RET(w->style, 2); case -20: RET(w->exstyle, 2); case -21: RET(w->userdata, 2);
    }
    RET(i >= 0 && i < 64 ? w->extra[i / 4] : 0, 2);
}
IMPL(user32, SetWindowLongA)
{
    Win *w = W(ARG(0)); int32_t i = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0; if (!w) RET(0, 3);
    switch (i) {
    case -4: old = w->wndproc; w->wndproc = v; break;
    case -16: old = w->style; w->style = v; break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->userdata; w->userdata = v; break;
    case -12: old = w->id; w->id = v; break;
    default: if (i >= 0 && i < 64) { old = w->extra[i / 4]; w->extra[i / 4] = v; }
    }
    RET(old, 3);
}
IMPL(user32, CallWindowProcA)
{
    uint32_t a[4] = {ARG(1), ARG(2), ARG(3), ARG(4)}, fn = ARG(0);
    RET(w32_callback(c, fn, 4, a), 5);
}
IMPL(user32, DefWindowProcA)
{
    uint32_t hwnd = ARG(0), msg = ARG(1);
    switch (msg) {
    case WM_NCCREATE: RET(1, 4);
    case WM_CLOSE: { uint32_t a = hwnd; (void)a; Win *w = W(hwnd); if (w) { send(c, hwnd, WM_DESTROY, 0, 0); w->hwnd = 0; } RET(0, 4); }
    case WM_ERASEBKGND: RET(1, 4);
    case WM_NCHITTEST: RET(1, 4);
    case WM_NCACTIVATE: RET(1, 4);
    case WM_PAINT: { Win *w = W(hwnd); if (w) w->invalid = 0; RET(0, 4); }
    }
    RET(0, 4);
}
IMPL(user32, SetWindowTextA) { Win *w = W(ARG(0)); if (w) snprintf(w->text, sizeof w->text, "%s", GS(ARG(1))); RET(w != 0, 2); }
IMPL(user32, GetWindowTextA)
{
    Win *w = W(ARG(0)); uint32_t buf = ARG(1), cap = ARG(2);
    if (!w || !cap) RET(0, 3);
    uint32_t n = (uint32_t)strlen(w->text); if (n >= cap) n = cap - 1;
    memcpy(GP(buf), w->text, n); rt_w8(G_MEM, buf + n, 0); RET(n, 3);
}
IMPL(user32, GetWindowTextLengthA) { Win *w = W(ARG(0)); RET(w ? strlen(w->text) : 0, 1); }
IMPL(user32, IsWindow) { RET(W(ARG(0)) != 0, 1); }
IMPL(user32, IsWindowVisible) { Win *w = W(ARG(0)); RET(w && w->visible, 1); }
IMPL(user32, IsIconic) { Win *w = W(ARG(0)); RET(w && w->iconic, 1); }
IMPL(user32, IsWindowEnabled) { Win *w = W(ARG(0)); RET(w && w->enabled, 1); }
IMPL(user32, EnableWindow) { Win *w = W(ARG(0)); int was = w && !w->enabled; if (w) w->enabled = ARG(1) != 0; RET(was, 2); }
IMPL(user32, GetParent) { Win *w = W(ARG(0)); RET(w ? (w->parent ? w->parent : w->owner) : 0, 1); }
IMPL(user32, GetWindow)
{
    Win *w = W(ARG(0)); uint32_t cmd = ARG(1); if (!w) RET(0, 2);
    if (cmd == 4) RET(w->owner, 2);                                         /* GW_OWNER */
    if (cmd == 5) { for (int k = 0; k < MAXW; k++) if (wins[k].hwnd && wins[k].parent == w->hwnd) RET(wins[k].hwnd, 2); RET(0, 2); }
    RET(0, 2);
}
IMPL(user32, GetDesktopWindow) { RET(0x10000, 0); }
IMPL(user32, FindWindowA)
{
    uint32_t cn = ARG(0), tn = ARG(1);
    for (int k = 0; k < MAXW; k++) {
        Win *w = &wins[k]; if (!w->hwnd || w->parent) continue;
        if (cn && (cn < 0x10000 ? w->cls->atom != cn : strcasecmp(w->cls->name, GS(cn)))) continue;
        if (tn && strcmp(w->text, GS(tn))) continue;
        RET(w->hwnd, 2);
    }
    RET(0, 2);
}
IMPL(user32, SetFocus) { uint32_t o = focus_hwnd; focus_hwnd = ARG(0); RET(o, 1); }
IMPL(user32, GetFocus) { RET(focus_hwnd, 0); }
IMPL(user32, SetActiveWindow) { uint32_t o = active_hwnd; active_hwnd = ARG(0); RET(o, 1); }
IMPL(user32, GetActiveWindow) { RET(active_hwnd, 0); }
IMPL(user32, SetForegroundWindow) { active_hwnd = ARG(0); RET(1, 1); }
IMPL(user32, GetForegroundWindow) { RET(active_hwnd, 0); }
IMPL(user32, BringWindowToTop) { RET(1, 1); }
IMPL(user32, GetLastActivePopup) { RET(ARG(0), 1); }
IMPL(user32, GetDlgCtrlID) { Win *w = W(ARG(0)); RET(w ? w->id : 0, 1); }

/* ---- messages: one queue per thread id ---- */
#define WM_TIMER_ 0x113u
typedef struct Msg { uint32_t hwnd, msg, wp, lp, time, x, y; } Msg;
typedef struct Queue { uint32_t tid; Msg q[512]; int head, n; int quit; uint32_t quit_code; } Queue;
static Queue queues[MAX_THREADS]; static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static Queue *Q(uint32_t tid)
{
    for (int k = 0; k < MAX_THREADS; k++) if (queues[k].tid == tid) return &queues[k];
    for (int k = 0; k < MAX_THREADS; k++) if (!queues[k].tid) { queues[k].tid = tid; return &queues[k]; }
    return &queues[0];
}
static uint32_t msg_time(void) { static uint32_t t = 1000; return w32_deterministic ? (t += 1) : (uint32_t)(ds_clock_ns()/1000000); }
static int cursor_x, cursor_y;
static pthread_mutex_t cursor_lock = PTHREAD_MUTEX_INITIALIZER;
/* The guest reads then warps its virtual pointer. Keep host motion received
 * between those calls: discarding it makes high-polling mice feel uneven. */
static int cursor_relative;
static uint64_t cursor_epoch;
static _Thread_local struct { int x,y,valid; uint64_t epoch; } cursor_read;
void w32_relative_cursor(int enabled) {
    pthread_mutex_lock(&cursor_lock); cursor_relative=enabled; cursor_epoch++; pthread_mutex_unlock(&cursor_lock);
}
void w32_post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    pthread_mutex_lock(&qlock);
    Queue *q = Q(tid);
    /* Consecutive motion messages describe the latest position. Avoid filling
     * the queue and losing button/key transitions with 1–8 kHz mice. */
    Msg *m=NULL;
    if(msg==0x200 && q->n) { Msg *last=&q->q[(q->head+q->n-1)%512]; if(last->msg==msg && last->hwnd==hwnd && last->wp==wp) m=last; }
    if(!m && q->n<512) m=&q->q[(q->head+q->n++)%512];
    if(m) { m->hwnd=hwnd; m->msg=msg; m->wp=wp; m->lp=lp; m->time=msg_time();
        pthread_mutex_lock(&cursor_lock); m->x=(uint32_t)cursor_x; m->y=(uint32_t)cursor_y; pthread_mutex_unlock(&cursor_lock); }
    pthread_mutex_unlock(&qlock);
}
void (*w32_host_pump)(void);           /* backend: turn host events into posted messages */
/* timers (SetTimer): WM_TIMER is made when a thread looks at its queue and a timer of it is due (after posted
 * messages, as on Windows); DispatchMessage calls the TIMERPROC when the timer has one */
typedef struct Timer { uint32_t tid, hwnd, id, ms, proc; double due; } Timer;
static Timer timers[64]; static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int timer_due(Ctx *c, uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    if ((lo || hi) && (WM_TIMER_ < lo || WM_TIMER_ > hi)) return 0;
    double t = now_ms(); uint32_t tid = w32_tid(c);
    pthread_mutex_lock(&tlock);
    for (int k = 0; k < 64; k++) {
        Timer *m = &timers[k];
        if (!m->ms || t < m->due || (m->hwnd ? (hwnd && m->hwnd != hwnd) : m->tid != tid)) continue;
        if (m->hwnd) { Win *w = W(m->hwnd); if (!w) continue; }
        Msg g = {m->hwnd, WM_TIMER_, m->id, m->proc, msg_time(), 0, 0}; memcpy(GP(out), &g, sizeof g);
        if (remove) m->due = t + m->ms;
        pthread_mutex_unlock(&tlock); return 1;
    }
    pthread_mutex_unlock(&tlock);
    return 0;
}
static int peek(Ctx *c, uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    if (w32_host_pump) w32_host_pump();
    pthread_mutex_lock(&qlock);
    Queue *q = Q(w32_tid(c));
    for (int k = 0; k < q->n; k++) {
        Msg *m = &q->q[(q->head + k) % 512];
        if (hwnd && m->hwnd != hwnd) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        memcpy(GP(out), m, sizeof *m);
        if (remove) { for (int j = k; j < q->n - 1; j++) q->q[(q->head + j) % 512] = q->q[(q->head + j + 1) % 512]; q->n--; }
        pthread_mutex_unlock(&qlock); return 1;
    }
    if (q->quit) {
        Msg m = {0, WM_QUIT, q->quit_code, 0, msg_time(), 0, 0}; memcpy(GP(out), &m, sizeof m);
        if (remove) q->quit = 0;
        pthread_mutex_unlock(&qlock); return 1;
    }
    pthread_mutex_unlock(&qlock);
    if (timer_due(c, out, hwnd, lo, hi, remove)) return 1;
    for (int k = 0; k < MAXW; k++) if (wins[k].hwnd && wins[k].invalid && wins[k].visible && (!hwnd || wins[k].hwnd == hwnd) && (!(lo || hi) || (WM_PAINT >= lo && WM_PAINT <= hi))) {
        Msg m = {wins[k].hwnd, WM_PAINT, 0, 0, msg_time(), 0, 0}; memcpy(GP(out), &m, sizeof m); return 1;
    }
    return 0;
}
void w32_crash_heartbeat(int which); void w32_crash_wait_begin(int, uint32_t, uint32_t); void w32_crash_wait_end(void);
void w32_lab_poll(Ctx *c);
IMPL(user32, PeekMessageA) { w32_crash_heartbeat(1); w32_lab_poll(c); RET(peek(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1), 5); }
IMPL(user32, GetMessageA)
{
    uint32_t out = ARG(0);
    while (w32_crash_heartbeat(1), !peek(c, out, ARG(1), ARG(2), ARG(3), 1)) {
        if (w32_deterministic) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "GetMessageA would block (harness)"); }
        w32_crash_wait_begin(4, 0, 0); usleep(2000); w32_crash_wait_end();
    }
    RET(rt_r32(G_MEM, out + 4) != WM_QUIT, 4);
}
IMPL(user32, PostMessageA) { Win *w = W(ARG(0)); (void)w; w32_post(w32_tid(c), ARG(0), ARG(1), ARG(2), ARG(3)); RET(1, 4); }
IMPL(user32, SendMessageA) { RET(send(c, ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
IMPL(user32, PostQuitMessage) { pthread_mutex_lock(&qlock); Queue *q = Q(w32_tid(c)); q->quit = 1; q->quit_code = ARG(0); pthread_mutex_unlock(&qlock); RET(0, 1); }
IMPL(user32, TranslateMessage) { RET(0, 1); }
IMPL(user32, DispatchMessageA)
{
    uint32_t m = ARG(0), hwnd = rt_r32(G_MEM, m), msg = rt_r32(G_MEM, m + 4);
    Win *w = W(hwnd);
    { static int log = -1; if (log < 0) log = getenv("W32_MSGLOG") != 0;    /* input messages the game dispatches */
      if (log && ((msg >= 0x100 && msg <= 0x109) || (msg >= 0x201 && msg <= 0x20e))) fprintf(stderr, "w32: dispatch %08x msg %04x wp %x lp %x to %08x\n", hwnd, msg, rt_r32(G_MEM, m + 8), rt_r32(G_MEM, m + 12), w ? w->wndproc : 0); }
    if (msg == WM_TIMER_ && rt_r32(G_MEM, m + 12)) {         /* a TIMERPROC(hwnd, WM_TIMER, id, time) */
        uint32_t a[4] = {hwnd, WM_TIMER_, rt_r32(G_MEM, m + 8), rt_r32(G_MEM, m + 16)};
        w32_callback(c, rt_r32(G_MEM, m + 12), 4, a); RET(0, 1);
    }
    if (msg == WM_PAINT && w) w->invalid = 0;
    RET(w ? send(c, hwnd, msg, rt_r32(G_MEM, m + 8), rt_r32(G_MEM, m + 12)) : 0, 1);
}
IMPL(user32, TranslateAcceleratorA) { RET(0, 3); }
IMPL(user32, IsDialogMessageA) { RET(0, 2); }
IMPL(user32, RegisterWindowMessageA) { static uint32_t next = 0xc100; RET(next++, 1); }
IMPL(user32, MsgWaitForMultipleObjects)
{
    uint32_t n = ARG(0), hs = ARG(1), ms = ARG(3);
    extern int w32_wait_one(Ctx *, uint32_t, uint32_t);
    for (uint32_t k = 0; k < n; k++) if (w32_wait_one(c, rt_r32(G_MEM, hs + 4 * k), 0) == 0) RET(k, 5);
    pthread_mutex_lock(&qlock); int any = Q(w32_tid(c))->n > 0; pthread_mutex_unlock(&qlock);
    if (any) RET(n, 5);
    if (!w32_deterministic && ms) usleep(ms == 0xffffffffu ? 1000 : (ms < 5 ? ms : 5) * 1000);
    RET(0x102, 5);
}
IMPL(user32, SetWindowsHookExA) { static uint32_t h = 0x7000; RET(h += 4, 4); }
IMPL(user32, CallNextHookEx) { RET(0, 4); }
IMPL(user32, BeginPaint)
{
    Win *w = W(ARG(0)); uint32_t ps = ARG(1); memset(GP(ps), 0, 64);
    if (w) { w->invalid = 0; rt_w32(G_MEM, ps, 0x20000 + 4); rt_w32(G_MEM, ps + 16, (uint32_t)w->w); rt_w32(G_MEM, ps + 20, (uint32_t)w->h); }
    RET(0x20004, 2);
}
IMPL(user32, EndPaint) { RET(1, 2); }

/* ---- input state (the backend updates these) ---- */
uint8_t w32_keys[256];                 /* bit 7: down, bit 0: toggled */
int w32_cursor_visible_count = 0;
IMPL(user32, GetAsyncKeyState) { RET((w32_keys[ARG(0) & 0xff] & 0x80) ? 0x8000 : 0, 1); }
IMPL(user32, GetKeyState) { uint8_t k = w32_keys[ARG(0) & 0xff]; RET(((k & 0x80) ? 0xff80u : 0) | (k & 1), 1); }
IMPL(user32, GetCursorPos)
{
    pthread_mutex_lock(&cursor_lock); int x = cursor_x, y = cursor_y;
    cursor_read.x=x; cursor_read.y=y; cursor_read.epoch=cursor_epoch; cursor_read.valid=1;
    pthread_mutex_unlock(&cursor_lock);
    rt_w32(G_MEM, ARG(0), (uint32_t)x); rt_w32(G_MEM, ARG(0) + 4, (uint32_t)y); RET(1, 1);
}
void (*w32_host_set_cursor_pos)(int x, int y);
IMPL(user32, SetCursorPos)
{
    pthread_mutex_lock(&cursor_lock);
    int dx=0,dy=0;
    if(cursor_relative && cursor_read.valid && cursor_read.epoch==cursor_epoch) { dx=cursor_x-cursor_read.x; dy=cursor_y-cursor_read.y; }
    cursor_x=(int)ARG(0)+dx; cursor_y=(int)ARG(1)+dy; cursor_read.valid=0; cursor_epoch++;
    pthread_mutex_unlock(&cursor_lock);
    if (w32_host_set_cursor_pos) w32_host_set_cursor_pos((int)ARG(0), (int)ARG(1));
    RET(1, 2);
}
void w32_set_cursor(int x, int y) { pthread_mutex_lock(&cursor_lock); cursor_x = x; cursor_y = y; cursor_epoch++; pthread_mutex_unlock(&cursor_lock); }
/* relative motion from the host, applied to the cursor the game last read or set (it re-centres it every frame) */
void w32_move_cursor(int dx, int dy, int w, int h, int *ox, int *oy)
{
    pthread_mutex_lock(&cursor_lock);
    cursor_x += dx; cursor_y += dy;
    if (cursor_x < 0) cursor_x = 0; if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > w - 1) cursor_x = w - 1; if (cursor_y > h - 1) cursor_y = h - 1;
    *ox = cursor_x; *oy = cursor_y;
    pthread_mutex_unlock(&cursor_lock);
}
void w32_get_cursor(int *x, int *y) { pthread_mutex_lock(&cursor_lock); *x = cursor_x; *y = cursor_y; pthread_mutex_unlock(&cursor_lock); }
IMPL(user32, ShowCursor) { w32_cursor_visible_count += ARG(0) ? 1 : -1; RET((uint32_t)w32_cursor_visible_count, 1); }
IMPL(user32, SetCursor) { static uint32_t cur; uint32_t o = cur; cur = ARG(0); RET(o, 1); }
IMPL(user32, LoadCursorA) { RET(0x30000 + (ARG(1) & 0xffff), 2); }
IMPL(user32, LoadIconA) { RET(0x31000 + (ARG(1) & 0xffff), 2); }
IMPL(user32, GetDoubleClickTime) { RET(500, 0); }
IMPL(user32, GetCaretBlinkTime) { RET(530, 0); }
IMPL(user32, GetKeyboardLayout) { RET(0x04090409, 1); }
/* US keyboard: scan code (set 1) -> virtual key */
static const uint8_t scan_vk[0x59] = {
    0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, 0x08, 0x09,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, 0x0d, 0x11, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0xc0, 0x10, 0xdc, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', 0xbc, 0xbe, 0xbf, 0x10, 0x6a, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6d, 0x25, 0x0c, 0x27, 0x6b, 0x23,
    0x28, 0x22, 0x2d, 0x2e, 0, 0, 0xe2, 0x7a, 0x7b};
static const char *scan_name[0x59] = {
    0, "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Tab",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Ctrl", "A", "S",
    "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "Shift", "\\", "Z", "X", "C", "V",
    "B", "N", "M", ",", ".", "/", "Right Shift", "Num *", "Alt", "Space", "Caps Lock", "F1", "F2", "F3", "F4", "F5",
    "F6", "F7", "F8", "F9", "F10", "Pause", "Scroll Lock", "Num 7", "Num 8", "Num 9", "Num -", "Num 4", "Num 5", "Num 6", "Num +", "Num 1",
    "Num 2", "Num 3", "Num 0", "Num Del", 0, 0, "\\", "F11", "F12"};
static int vk_to_char(int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return vk;
    switch (vk) { case 0xbd: return '-'; case 0xbb: return '='; case 0xdb: return '['; case 0xdd: return ']'; case 0xba: return ';';
    case 0xde: return '\''; case 0xc0: return '`'; case 0xdc: return '\\'; case 0xbc: return ','; case 0xbe: return '.'; case 0xbf: return '/';
    case 0x20: return ' '; case 0x0d: return '\r'; case 0x08: return 8; case 0x09: return 9; case 0x1b: return 27;
    case 0x6a: return '*'; case 0x6b: return '+'; case 0x6d: return '-'; }
    return 0;
}
IMPL(user32, MapVirtualKeyA)
{
    uint32_t code = ARG(0), type = ARG(1);
    if (type == 0) { for (int s = 1; s < 0x59; s++) if (scan_vk[s] == code) RET((uint32_t)s, 2); RET(0, 2); }
    if (type == 1 || type == 3) RET(code < 0x59 ? scan_vk[code] : 0, 2);
    if (type == 2) RET((uint32_t)vk_to_char((int)code), 2);
    RET(0, 2);
}
IMPL(user32, VkKeyScanA)
{
    int ch = (int)(int8_t)ARG(0) & 0xff;
    if (ch >= 'a' && ch <= 'z') RET((uint32_t)(ch - 32), 1);
    if (ch >= 'A' && ch <= 'Z') RET(0x100u | (uint32_t)ch, 1);
    static const char shifted[] = ")!@#$%^&*(";
    const char *p = strchr(shifted, ch); if (p && ch) RET(0x100u | (uint32_t)('0' + (p - shifted)), 1);
    for (int vk = 0; vk < 256; vk++) if (vk_to_char(vk) == ch) RET((uint32_t)vk, 1);
    RET(0xffff, 1);
}
IMPL(user32, GetKeyNameTextA)
{
    uint32_t lp = ARG(0), buf = ARG(1), cap = ARG(2), sc = (lp >> 16) & 0xff, ext = (lp >> 24) & 1;
    const char *n = sc < 0x59 ? scan_name[sc] : 0;
    if (ext) { if (sc == 0x1d) n = "Right Ctrl"; else if (sc == 0x38) n = "Right Alt"; else if (sc == 0x47) n = "Home"; else if (sc == 0x48) n = "Up";
               else if (sc == 0x49) n = "Page Up"; else if (sc == 0x4b) n = "Left"; else if (sc == 0x4d) n = "Right"; else if (sc == 0x4f) n = "End";
               else if (sc == 0x50) n = "Down"; else if (sc == 0x51) n = "Page Down"; else if (sc == 0x52) n = "Insert"; else if (sc == 0x53) n = "Delete";
               else if (sc == 0x35) n = "Num /"; else if (sc == 0x1c) n = "Num Enter"; }
    if (!n || !cap) RET(0, 3);
    uint32_t l = (uint32_t)strlen(n); if (l >= cap) l = cap - 1;
    memcpy(GP(buf), n, l); rt_w8(G_MEM, buf + l, 0); RET(l, 3);
}

/* ---- system ---- */
IMPL(user32, GetSystemMetrics)
{
    switch (ARG(0)) {
    case 0: case 16: case 78: RET((uint32_t)w32_screen_w, 1);
    case 1: case 79: RET((uint32_t)w32_screen_h, 1);
    case 17: RET((uint32_t)w32_screen_h - 19, 1);
    case 2: case 3: case 20: case 21: RET(16, 1);
    case 4: case 15: RET(19, 1);
    case 5: case 6: RET(1, 1);
    case 7: case 8: RET(3, 1);
    case 11: case 12: case 13: case 14: RET(32, 1);
    case 19: case 75: RET(1, 1);
    case 32: case 33: RET(4, 1);
    case 36: case 37: RET(4, 1);
    case 43: RET(3, 1);
    case 80: RET(1, 1);
    }
    RET(0, 1);
}
IMPL(user32, SystemParametersInfoA)
{
    uint32_t act = ARG(0), pv = ARG(2);
    switch (act) {
    case 0x30: { uint32_t v[4] = {0, 0, (uint32_t)w32_screen_w, (uint32_t)w32_screen_h}; memcpy(GP(pv), v, 16); break; }   /* SPI_GETWORKAREA */
    case 0x10: case 0x0e: if (pv) rt_w32(G_MEM, pv, 0); break;
    case 0x03: { uint32_t v[3] = {6, 10, 1}; memcpy(GP(pv), v, 12); break; }
    case 0x32: case 0x34: case 0x3a: { uint32_t sz = rt_r32(G_MEM, pv); memset(GP(pv + 4), 0, sz > 4 ? sz - 4 : 0); break; }
    case 0x0a: case 0x16: if (pv) rt_w32(G_MEM, pv, 31); break;
    case 0x68: if (pv) rt_w32(G_MEM, pv, 3); break;                            /* SPI_GETWHEELSCROLLLINES */
    }
    RET(1, 4);
}
IMPL(user32, EnumDisplaySettingsA)
{
#ifdef __linux__
    uint32_t modes[DS_DISPLAY_MODE_CAP][2];
    int n = ds_display_modes(modes, w32_screen_w, w32_screen_h, getenv("DS_RESOLUTION"));
    uint32_t mode = ARG(1), dm = ARG(2); int w, h, bpp;
    if (mode == 0xffffffffu || mode == 0xfffffffeu) { w = w32_screen_w; h = w32_screen_h; bpp = 32; }
    else {
        if (mode >= (uint32_t)n * 2) RET(0, 3);
        w = modes[mode / 2][0]; h = modes[mode / 2][1]; bpp = (mode & 1) ? 32 : 16;
    }
#else
    static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 800}, {1280, 1024}, {1440, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {0, 0}};
    uint32_t mode = ARG(1), dm = ARG(2); int w, h, bpp;
    if (mode == 0xffffffffu || mode == 0xfffffffeu) { w = w32_screen_w; h = w32_screen_h; bpp = 32; }
    else {
        int n = 0; while (modes[n][0]) n++;
        int total = 2 * (n + 1); if ((int)mode >= total) RET(0, 3);
        int k = (int)mode / 2; bpp = (mode & 1) ? 32 : 16;
        if (k < n) { w = modes[k][0]; h = modes[k][1]; } else { w = w32_screen_w; h = w32_screen_h; }
    }
#endif
    memset(GP(dm + 32), 0, 124); rt_w16(G_MEM, dm + 36, 156); rt_w32(G_MEM, dm + 40, 0x5c0000);
    rt_w32(G_MEM, dm + 104, (uint32_t)bpp); rt_w32(G_MEM, dm + 108, (uint32_t)w); rt_w32(G_MEM, dm + 112, (uint32_t)h); rt_w32(G_MEM, dm + 120, (uint32_t)w32_screen_hz);
    RET(1, 3);
}
IMPL(user32, GetSysColor) { RET(0xc0c0c0, 1); }
IMPL(user32, MessageBeep) { RET(1, 1); }
IMPL(user32, MessageBoxA)
{
    fprintf(stderr, "w32: MessageBox \"%s\": %s\n", ARG(2) ? GS(ARG(2)) : "", ARG(1) ? GS(ARG(1)) : "");
    RET(1, 4);
}
IMPL(user32, MessageBoxW)
{
    char t[512], m[2048]; w32_wide_to_mb(1252, GW(ARG(1)), -1, (uint8_t *)m, sizeof m); if (ARG(2)) w32_wide_to_mb(1252, GW(ARG(2)), -1, (uint8_t *)t, sizeof t); else t[0] = 0;
    fprintf(stderr, "w32: MessageBox \"%s\": %s\n", t, m); RET(1, 4);
}
IMPL(user32, GetDC) { RET(0x20004, 1); }
IMPL(user32, ReleaseDC) { RET(1, 2); }
IMPL(user32, FillRect) { RET(1, 3); }
IMPL(user32, DrawFrameControl) { RET(1, 4); }
IMPL(user32, DrawTextA) { RET(16, 5); }
IMPL(user32, OpenClipboard) { RET(1, 1); }
IMPL(user32, CloseClipboard) { RET(1, 0); }
IMPL(user32, EmptyClipboard) { RET(1, 0); }
IMPL(user32, GetClipboardData) { RET(0, 1); }
IMPL(user32, SetClipboardData) { RET(ARG(1), 2); }

/* ---- dialogs: log the template's text (the game uses dialogs for error reports and asserts) ---- */
static void dump_dialog(uint32_t t)
{
    /* DLGTEMPLATE: style, exstyle, cdit(u16), x, y, cx, cy (u16 each), then menu, class, title (sz_Or_Ord, UTF-16) */
    uint32_t style = rt_r32(G_MEM, t); uint16_t n = (uint16_t)rt_r16(G_MEM, t + 8); uint32_t p = t + 18;
    for (int k = 0; k < 2; k++) { uint16_t w = (uint16_t)rt_r16(G_MEM, p); if (w == 0xffff) p += 4; else { while (rt_r16(G_MEM, p)) p += 2; p += 2; } }
    char s[512]; w32_wide_to_mb(1252, GW(p), -1, (uint8_t *)s, sizeof s); fprintf(stderr, "w32: dialog \"%s\" (%u items)\n", s, n);
    while (rt_r16(G_MEM, p)) p += 2; p += 2;
    if (style & 0x40) { p += 2; while (rt_r16(G_MEM, p)) p += 2; p += 2; }                  /* DS_SETFONT */
    for (int k = 0; k < n; k++) {
        p = (p + 3) & ~3u; p += 18;                                                          /* DLGITEMTEMPLATE */
        uint16_t cls = (uint16_t)rt_r16(G_MEM, p); if (cls == 0xffff) p += 4; else { while (rt_r16(G_MEM, p)) p += 2; p += 2; }
        uint16_t tw = (uint16_t)rt_r16(G_MEM, p);
        if (tw == 0xffff) p += 4; else { w32_wide_to_mb(1252, GW(p), -1, (uint8_t *)s, sizeof s); if (*s) fprintf(stderr, "w32:   item: %s\n", s); while (rt_r16(G_MEM, p)) p += 2; p += 2; }
        p += 2 + rt_r16(G_MEM, p);
    }
}
IMPL(user32, DialogBoxIndirectParamA) { dump_dialog(ARG(1)); RET(0xffffffffu, 5); }
IMPL(user32, CreateDialogIndirectParamA) { dump_dialog(ARG(1)); RET(0, 5); }
IMPL(user32, EndDialog) { RET(1, 2); }
IMPL(user32, GetDlgItem) { RET(0, 2); }
IMPL(user32, SetDlgItemTextA) { fprintf(stderr, "w32:   dialog item %u: %s\n", ARG(1), GS(ARG(2))); RET(1, 3); }
IMPL(user32, GetDlgItemInt) { RET(0, 4); }

/* for the host backend */
uint32_t w32_window_tid(uint32_t hwnd) { Win *w = W(hwnd); return w ? w->tid : 0; }
void w32_window_client_origin(uint32_t hwnd, int *x, int *y) { Win *w = W(hwnd); *x = *y = 0; if (w) client_origin(w, x, y); }
uint32_t w32_active_window(void) { return active_hwnd; }
uint32_t w32_top_window(void) { return active_hwnd; }

IMPL(user32, SetTimer)                                  /* (hwnd, id, ms, proc): window timers keep their id; thread timers get one */
{
    uint32_t hwnd = ARG(0), id = ARG(1), ms = ARG(2) < 10 ? 10 : ARG(2), proc = ARG(3), r = 0;
    pthread_mutex_lock(&tlock);
    int k = -1;
    if (hwnd) for (int j = 0; j < 64; j++) if (timers[j].ms && timers[j].hwnd == hwnd && timers[j].id == id) k = j;
    if (k < 0) for (int j = 0; j < 64; j++) if (!timers[j].ms) { k = j; break; }
    if (k >= 0) {
        static uint32_t next_id = 0x7000;
        Timer *m = &timers[k]; m->tid = w32_tid(c); m->hwnd = hwnd; m->id = hwnd ? id : ++next_id; m->ms = ms; m->proc = proc; m->due = now_ms() + ms;
        r = hwnd ? (id ? id : 1) : m->id;
    }
    pthread_mutex_unlock(&tlock);
    RET(r, 4);
}
IMPL(user32, KillTimer)
{
    uint32_t hwnd = ARG(0), id = ARG(1), ok = 0;
    pthread_mutex_lock(&tlock);
    for (int j = 0; j < 64; j++) if (timers[j].ms && timers[j].hwnd == hwnd && timers[j].id == id && (hwnd || timers[j].tid == w32_tid(c))) { timers[j].ms = 0; ok = 1; }
    pthread_mutex_unlock(&tlock);
    RET(ok, 2);
}
