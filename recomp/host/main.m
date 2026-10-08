// DungeonSiegeNative: the recompiled game as a native macOS app. The game runs on its own thread (recompiled code +
// Win32 layer); this file provides the window, the Metal renderer consuming the game's D3D7 command stream in-process
// (src/renderer), and input (mouse/keyboard -> Windows messages and key state, relative mouse like the original).
//   DungeonSiegeNative --exe <DungeonSiege.exe> --game <game folder> --data <folder for C: and saved files>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#ifndef DS_BUILD
#define DS_BUILD "dev"
#endif
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach_time.h>
#import "../../src/renderer/renderer.h"
#include "../../src/dsr/dsr_proto.h"
#include "../../src/dsr/dsr_shm.h"
#include "../runtime/win32/w32.h"

int  w32_prepare(const char *exe, const char *game_dir, const char *drive_c, const char *overlay);
uint32_t w32_run(char *msg, size_t cap);
void dsr_attach_consumer(DsrShmHeader *h);
void dsr_mode_size(uint32_t *w, uint32_t *h);
void w32_post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void w32_set_cursor(int x, int y);
uint32_t w32_window_tid(uint32_t hwnd);
extern uint8_t w32_keys[256];
extern int w32_screen_w, w32_screen_h; extern double w32_screen_scale;
extern void (*w32_host_window_created)(uint32_t, int, int, int, int, uint32_t);
extern void (*w32_host_window_changed)(uint32_t, int, int, int, int, int);
extern char dsr_snapshot_path[1024];

static DsrShmHeader *H; static uint8_t *ring;
static CAMetalLayer *layer; static NSWindow *win; static uint32_t game_hwnd;
static volatile int game_running = 1;
static const char *script_tick(uint32_t fno);
static int test_mode;                  // DS_TEST=1: ordinary background window, never captures the mouse or takes focus

// ---------------------------------------------------------------- rendering
void w32_crash_init(const char *data_dir, const char *build); void w32_crash_thread(const char *name);
void w32_crash_heartbeat(int which); const char *w32_crash_last_path(void);
const char *w32_crash_report(const char *kind, const char *reason, Ctx *c, uint32_t guest_pc);
static void *render_thread(void *arg)
{
    (void)arg; w32_crash_thread("renderer");
    @autoreleasepool {
        DSRRenderer *r = dsr_renderer_create(layer.device);
        dsr_renderer_set_readback(r, (uint8_t *)H + DSR_READBACK_OFFSET, DSR_READBACK_SIZE, &H->readback_done);
        uint32_t rpos = H->read_pos, frames = 0, spins = 0;
        __atomic_store_n(&H->host_ready, 1, __ATOMIC_RELEASE);
        for (;;) {
            uint32_t wpos = __atomic_load_n(&H->write_pos, __ATOMIC_ACQUIRE);
            if (rpos == wpos) { if (++spins < 2000) { __asm__ volatile("yield"); continue; } usleep(50); continue; }
            spins = 0;
            @autoreleasepool {
                while (rpos != wpos) {
                    uint32_t off = rpos % DSR_RING_SIZE;
                    if (DSR_RING_SIZE - off < 8) { rpos += DSR_RING_SIZE - off; continue; }
                    uint32_t op = *(uint32_t *)(ring + off), size = *(uint32_t *)(ring + off + 4);
                    if (op == DSR_WRAP) { rpos += 8 + size; continue; }
                    int present = dsr_renderer_exec(r, op, ring + off + 8, size);
                    rpos += 8 + size;
                    __atomic_store_n(&H->read_pos, rpos, __ATOMIC_RELEASE);
                    if (!present) continue;
                    uint32_t w, h; dsr_renderer_presented_size(r, &w, &h);
                    if (w && h && (layer.drawableSize.width != w || layer.drawableSize.height != h)) {
                        [CATransaction begin]; [CATransaction setDisableActions:YES]; layer.drawableSize = CGSizeMake(w, h); [CATransaction commit];
                    }
                    uint32_t fno = ++frames;
                    if (test_mode) {   // one line per second: frames presented, the mode, GPU time per frame and frame-time spikes
                        static double t0, last; double now = CACurrentMediaTime(); static uint32_t f0; static double worst; static int spikes;
                        if (!t0) t0 = now;
                        if (last) { double ft = now - last; if (ft > worst) worst = ft; if (ft > 0.025) spikes++; } last = now;
                        if (now - t0 >= 1.0) {
                            extern long dsr_stat[8]; extern double dsr_gpu_ms[100000]; extern int dsr_frame;
                            double gsum = 0, gmax = 0; int gn = 0;   // frames whose command buffers have completed (two behind)
                            for (int k = (int)f0 - 2; k < dsr_frame - 2; k++) if (k >= 0 && k < 100000) { gsum += dsr_gpu_ms[k]; if (dsr_gpu_ms[k] > gmax) gmax = dsr_gpu_ms[k]; gn++; }
                            fprintf(stderr, "DungeonSiegeNative: frame %u (%ux%u) %.1f fps; draws %ld, dropped %ld/%ld/%ld/%ld; gpu %.2f ms (max %.2f); longest frame %.1f ms, %d over 25 ms\n",
                                    fno, w, h, (fno - f0) / (now - t0), dsr_stat[0], dsr_stat[1], dsr_stat[2], dsr_stat[3], dsr_stat[4],
                                    gn ? gsum / gn : 0, gmax, worst * 1000, spikes);
                            t0 = now; f0 = fno; worst = 0; spikes = 0; }
                    }
                    {   // DS_SHOT=<path>,<frame>: save that frame as a PNG (development)
                        static long shot_at = -2; static char shot_path[1024];
                        if (shot_at == -2) { const char *e = test_mode ? getenv("DS_SHOT") : 0; shot_at = -1;   /* test mode only: it writes anywhere */ if (e) { const char *cm = strrchr(e, ','); if (cm) { snprintf(shot_path, sizeof shot_path, "%.*s", (int)(cm - e), e); shot_at = atol(cm + 1); } } }
                        const char *sp = test_mode ? script_tick(fno) : 0;
                        if (sp) { snprintf(shot_path, sizeof shot_path, "%s", sp); shot_at = fno; }
                        if ((long)fno == shot_at) {
                            id<MTLTexture> tex = dsr_renderer_presented(r); dsr_renderer_flush(r, 1);
                            if (tex && tex.pixelFormat == MTLPixelFormatBGRA8Unorm) {
                                NSUInteger tw = tex.width, th = tex.height;
                                id<MTLBuffer> buf = [layer.device newBufferWithLength:tw * th * 4 options:MTLResourceStorageModeShared];
                                id<MTLCommandQueue> q = [layer.device newCommandQueue]; id<MTLCommandBuffer> cbs = [q commandBuffer];
                                id<MTLBlitCommandEncoder> bl = [cbs blitCommandEncoder];
                                [bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(tw, th, 1)
                                           toBuffer:buf destinationOffset:0 destinationBytesPerRow:tw * 4 destinationBytesPerImage:tw * th * 4];
                                [bl endEncoding]; [cbs commit]; [cbs waitUntilCompleted];
                                uint8_t *px = malloc(tw * th * 4); memcpy(px, buf.contents, tw * th * 4);
                                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                                CGContextRef cg = CGBitmapContextCreate(px, tw, th, 8, tw * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                                CGImageRef img = CGBitmapContextCreateImage(cg);
                                NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
                                [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:shot_path] atomically:YES];
                                CGImageRelease(img); CGContextRelease(cg); CGColorSpaceRelease(cs); free(px);
                                fprintf(stderr, "DungeonSiegeNative: frame %u saved to %s\n", fno, shot_path);
                            } else if (tex) fprintf(stderr, "DungeonSiegeNative: screenshot: unexpected pixel format %lu\n", (unsigned long)tex.pixelFormat);
                        }
                    }
                    id<CAMetalDrawable> d = [layer nextDrawable];
                    id<MTLCommandBuffer> cb = d ? dsr_renderer_present_to(r, d.texture) : nil;
                    if (cb) {
                        [cb presentDrawable:d]; w32_crash_heartbeat(3);
                        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; __atomic_store_n(&H->frames_done, fno, __ATOMIC_RELEASE); }];
                        dsr_renderer_flush(r, 0);
                    } else { dsr_renderer_flush(r, 0); __atomic_store_n(&H->frames_done, fno, __ATOMIC_RELEASE); }
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- test scripting (DS_SCRIPT, test mode only)
// DS_SCRIPT="1900:click:400,150;2100:shot:/tmp/a.png;2200:key:13" : at a presented frame, act through the same message
// path as real input. Coordinates are in the game's current display mode.
typedef struct { uint32_t frame; double at; char kind[8]; int x, y; char path[512]; } Act;   // at > 0: seconds from start
static Act acts[256]; static int nacts;
static double vcur_x, vcur_y;          // virtual cursor in game (display mode) pixels
static void post(uint32_t msg, uint32_t wp, uint32_t lp);
static void rel_motion(double gx, double gy);   /* relative motion in game pixels, the real-input path */
static uint32_t pos_lp(void);
static void parse_script(void)
{
    const char *e = getenv("DS_SCRIPT"); if (!e) return;
    char *s = strdup(e), *save, *tok = strtok_r(s, ";", &save);
    while (tok && nacts < 256) {
        Act *a = &acts[nacts]; char kind[8] = "", rest[600] = "";
        int ok = tok[0] == '@' ? sscanf(tok + 1, "%lf:%7[a-z]:%599[^\n]", &a->at, kind, rest) : sscanf(tok, "%u:%7[a-z]:%599[^\n]", &a->frame, kind, rest);
        if (tok[0] == '@') a->frame = 0xffffffffu;
        if (ok >= 2) {
            snprintf(a->kind, sizeof a->kind, "%s", kind);
            if (!strcmp(kind, "shot") || !strcmp(kind, "type") || !strcmp(kind, "peek")) snprintf(a->path, sizeof a->path, "%s", rest); else sscanf(rest, "%d,%d", &a->x, &a->y);
            nacts++;
        }
        tok = strtok_r(0, ";", &save);
    }
    free(s);
}
/* The game moves its own cursor by the distance of the OS cursor from the window centre each frame (then re-centres
 * it), so a scripted click is mouse motion: pin to the top-left corner, then move by the target offset in steps. */
static int mot_dx, mot_dy, mot_home;
/* Dungeon Siege II reads the cursor's absolute position (GetCursorPos): a scripted move puts it at the target and sends
 * WM_MOUSEMOVE there for a few frames */
static void motion_tick(void)
{
    if (mot_home <= 0) return;
    mot_home--;
    vcur_x = mot_dx; vcur_y = mot_dy; w32_set_cursor(mot_dx, mot_dy);
    post(0x200, 0, ((uint32_t)mot_dy << 16) | ((uint32_t)mot_dx & 0xffff));
}
/* "type" action: each character as a keyDown/keyUp NSEvent sent to the window; {esc} {enter} {bs} for keys */
static void type_text(NSString *text);
/* DS_CMDFILE=<path> (test mode): lines appended to the file while the game runs ("click:400,290", "type:Name",
 * "key:13", "shot:/tmp/a.png", ...) are carried out from the next frame on: for tests driven from outside.
 * "lag:100,20" sets the DP8_LAG simulator; "peek:7a05fc,a8,13" prints game memory: the dword at the first (hex) address, then each further offset added to the
 * previous value and read again (a pointer chain) */
static void cmdfile_poll(uint32_t fno)
{
    static const char *path; static off_t pos; static int init;
    if (!init) { init = 1; path = getenv("DS_CMDFILE"); }
    if (!path || fno % 15) return;
    FILE *f = fopen(path, "r"); if (!f) return;
    fseeko(f, pos, SEEK_SET); char line[700];
    while (nacts < 256 && fgets(line, sizeof line, f)) {
        size_t l = strlen(line); if (!l || line[l - 1] != '\n') break;       /* a line still being written */
        pos += (off_t)l; line[l - 1] = 0;
        Act *a = &acts[nacts]; char kind[8] = "", rest[600] = "";
        if (sscanf(line, "%7[a-z]:%599[^\n]", kind, rest) < 1 || !kind[0]) continue;
        memset(a, 0, sizeof *a); a->frame = fno + 1; snprintf(a->kind, sizeof a->kind, "%s", kind);
        if (!strcmp(kind, "shot") || !strcmp(kind, "type") || !strcmp(kind, "peek")) snprintf(a->path, sizeof a->path, "%s", rest); else sscanf(rest, "%d,%d", &a->x, &a->y);
        nacts++; fprintf(stderr, "DungeonSiegeNative: command %s\n", line);
    }
    fclose(f);
}
static const char *script_tick(uint32_t fno)       /* returns a screenshot path if one is due */
{
    const char *shot = 0;
    cmdfile_poll(fno);
    static double t0; double now = CACurrentMediaTime(); if (!t0) t0 = now;
    for (int k = 0; k < nacts; k++) if (acts[k].at > 0 && acts[k].frame == 0xffffffffu && now - t0 >= acts[k].at) acts[k].frame = fno;   /* timed steps start now */
    motion_tick();
    for (int k = 0; k < nacts; k++) {
        Act *a = &acts[k];
        if (!strcmp(a->kind, "click") || !strcmp(a->kind, "rclick")) {
            int r = a->kind[0] == 'r';
            if (fno == a->frame) { mot_home = 8; mot_dx = a->x; mot_dy = a->y; }
            if (fno == a->frame + 20) post(r ? 0x204 : 0x201, r ? 2 : 1, pos_lp());
            if (fno == a->frame + 24) post(r ? 0x205 : 0x202, 0, pos_lp());
        } else if (!strcmp(a->kind, "press")) {           /* press:x,y: the left button pressed and held (until a later click) */
            if (fno == a->frame) { mot_home = 8; mot_dx = a->x; mot_dy = a->y; }
            if (fno == a->frame + 20) post(0x201, 1, pos_lp());
        } else if (!strcmp(a->kind, "move") && fno == a->frame) { mot_home = 8; mot_dx = a->x; mot_dy = a->y; }
        else if (!strcmp(a->kind, "rel") && fno >= a->frame && fno < a->frame + 60) rel_motion(a->x / 60.0, a->y / 60.0);   /* x,y over 60 frames, like a trackpad swipe */
        else if (!strcmp(a->kind, "key")) {
            if (fno == a->frame) { w32_keys[a->x & 0xff] = 0x80; post(0x100, (uint32_t)a->x, 1); }
            if (fno == a->frame + 3) { w32_keys[a->x & 0xff] = 0; post(0x101, (uint32_t)a->x, 0xc0000001u); }
        } else if (!strcmp(a->kind, "hold")) {            /* hold:vk,frames: a key held down (the modifier of a chord) */
            if (fno == a->frame) { w32_keys[a->x & 0xff] = 0x80; post(0x100, (uint32_t)a->x, 1); }
            if (fno == a->frame + (uint32_t)a->y) { w32_keys[a->x & 0xff] = 0; post(0x101, (uint32_t)a->x, 0xc0000001u); }
        } else if (!strcmp(a->kind, "shot") && fno == a->frame) shot = a->path;
        else if (!strcmp(a->kind, "peek") && fno == a->frame) {
            extern uint8_t *G_MEM; char *e = a->path; uint32_t at = (uint32_t)strtoul(e, &e, 16), v = 0;
            for (;;) { if (at < 0x10000u) { v = 0; break; } memcpy(&v, G_MEM + at, 4); if (*e != ',') break; at = v + (uint32_t)strtoul(e + 1, &e, 16); }
            fprintf(stderr, "DungeonSiegeNative: peek %s = 0x%08x (byte 0x%02x)\n", a->path, v, v & 0xff);
        }
        else if (!strcmp(a->kind, "wheel") && fno >= a->frame && fno < a->frame + (uint32_t)abs(a->x)) post(0x20a, ((uint32_t)(int16_t)(a->x > 0 ? 120 : -120) << 16), pos_lp());   /* x notches */
        else if (!strcmp(a->kind, "type") && fno == a->frame) {   /* real NSEvents through the window (keyboard path) */
            NSString *text = @(a->path);
            dispatch_async(dispatch_get_main_queue(), ^{ type_text(text); });
        }
    }
    return shot;
}

// ---------------------------------------------------------------- input
typedef struct { uint8_t vk, scan, ext; } Key;
static Key keymap[128];
static void init_keymap(void)
{
    static const uint8_t t[][4] = {   // macOS keycode, VK, scan, extended
        {0x00,'A',0x1e,0},{0x01,'S',0x1f,0},{0x02,'D',0x20,0},{0x03,'F',0x21,0},{0x04,'H',0x23,0},{0x05,'G',0x22,0},{0x06,'Z',0x2c,0},
        {0x07,'X',0x2d,0},{0x08,'C',0x2e,0},{0x09,'V',0x2f,0},{0x0b,'B',0x30,0},{0x0c,'Q',0x10,0},{0x0d,'W',0x11,0},{0x0e,'E',0x12,0},
        {0x0f,'R',0x13,0},{0x10,'Y',0x15,0},{0x11,'T',0x14,0},{0x12,'1',0x02,0},{0x13,'2',0x03,0},{0x14,'3',0x04,0},{0x15,'4',0x05,0},
        {0x16,'6',0x07,0},{0x17,'5',0x06,0},{0x18,0xbb,0x0d,0},{0x19,'9',0x0a,0},{0x1a,'7',0x08,0},{0x1b,0xbd,0x0c,0},{0x1c,'8',0x09,0},
        {0x1d,'0',0x0b,0},{0x1e,0xdd,0x1b,0},{0x1f,'O',0x18,0},{0x20,'U',0x16,0},{0x21,0xdb,0x1a,0},{0x22,'I',0x17,0},{0x23,'P',0x19,0},
        {0x24,0x0d,0x1c,0},{0x25,'L',0x26,0},{0x26,'J',0x24,0},{0x27,0xde,0x28,0},{0x28,'K',0x25,0},{0x29,0xba,0x27,0},{0x2a,0xdc,0x2b,0},
        {0x2b,0xbc,0x33,0},{0x2c,0xbf,0x35,0},{0x2d,'N',0x31,0},{0x2e,'M',0x32,0},{0x2f,0xbe,0x34,0},{0x30,0x09,0x0f,0},{0x31,0x20,0x39,0},
        {0x32,0xc0,0x29,0},{0x33,0x08,0x0e,0},{0x35,0x1b,0x01,0},{0x38,0x10,0x2a,0},{0x39,0x14,0x3a,0},{0x3a,0x12,0x38,0},{0x3b,0x11,0x1d,0},
        {0x3c,0x10,0x36,0},{0x3d,0x12,0x38,1},{0x3e,0x11,0x1d,1},{0x7a,0x70,0x3b,0},{0x78,0x71,0x3c,0},{0x63,0x72,0x3d,0},{0x76,0x73,0x3e,0},
        {0x60,0x74,0x3f,0},{0x61,0x75,0x40,0},{0x62,0x76,0x41,0},{0x64,0x77,0x42,0},{0x65,0x78,0x43,0},{0x6d,0x79,0x44,0},{0x67,0x7a,0x57,0},
        {0x6f,0x7b,0x58,0},{0x73,0x24,0x47,1},{0x74,0x21,0x49,1},{0x75,0x2e,0x53,1},{0x77,0x23,0x4f,1},{0x79,0x22,0x51,1},{0x7b,0x25,0x4b,1},
        {0x7c,0x27,0x4d,1},{0x7d,0x28,0x50,1},{0x7e,0x26,0x48,1},{0x52,0x60,0x52,0},{0x53,0x61,0x4f,0},{0x54,0x62,0x50,0},{0x55,0x63,0x51,0},
        {0x56,0x64,0x4b,0},{0x57,0x65,0x4c,0},{0x58,0x66,0x4d,0},{0x59,0x67,0x47,0},{0x5b,0x68,0x48,0},{0x5c,0x69,0x49,0},{0x41,0x6e,0x53,0},
        {0x43,0x6a,0x37,0},{0x45,0x6b,0x4e,0},{0x4e,0x6d,0x4a,0},{0x4b,0x6f,0x35,1},{0x4c,0x0d,0x1c,1}};
    for (size_t k = 0; k < sizeof t / sizeof *t; k++) { keymap[t[k][0]].vk = t[k][1]; keymap[t[k][0]].scan = t[k][2]; keymap[t[k][0]].ext = t[k][3]; }
}
static uint32_t buttons;               // MK_ flags
static void post(uint32_t msg, uint32_t wp, uint32_t lp) { if (game_hwnd) w32_post(w32_window_tid(game_hwnd), game_hwnd, msg, wp, lp); }
static uint32_t mk(void)
{
    NSEventModifierFlags f = NSEvent.modifierFlags;
    return buttons | ((f & NSEventModifierFlagShift) ? 4 : 0) | ((f & NSEventModifierFlagControl) ? 8 : 0);
}
void w32_move_cursor(int dx, int dy, int w, int h, int *ox, int *oy); void w32_get_cursor(int *x, int *y);
static uint32_t pos_lp(void) { int x, y; w32_get_cursor(&x, &y); return ((uint32_t)y << 16) | ((uint32_t)x & 0xffff); }
static void key_event(uint16_t code, int down, int repeat)
{
    if (code >= 128 || !keymap[code].vk) return;
    Key k = keymap[code]; int alt = (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0;
    uint32_t prev = (w32_keys[k.vk] & 0x80) ? 1 : 0;
    w32_keys[k.vk] = down ? (uint8_t)(0x80 | ((w32_keys[k.vk] & 1) ^ (repeat ? 0 : 1))) : (uint8_t)(w32_keys[k.vk] & 1);
    if (k.vk == 0x10) w32_keys[code == 0x3c ? 0xa1 : 0xa0] = w32_keys[0x10];
    if (k.vk == 0x11) w32_keys[k.ext ? 0xa3 : 0xa2] = w32_keys[0x11];
    if (k.vk == 0x12) w32_keys[k.ext ? 0xa5 : 0xa4] = w32_keys[0x12];
    uint32_t lp = 1u | ((uint32_t)k.scan << 16) | ((uint32_t)k.ext << 24) | (prev << 30) | (down ? 0 : 0x80000000u);
    int sys = alt || k.vk == 0x12;
    post(down ? (sys ? 0x104u : 0x100u) : (sys ? 0x105u : 0x101u), k.vk, lp);
}

/* A borderless window cannot become the key window by default, so it would get no keyboard events (only the mouse,
 * which goes to the window under the pointer). */
@interface GameWindow : NSWindow @end
@implementation GameWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end
@interface GameView : NSView @end
@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return layer; }
- (void)moved:(NSEvent *)e
{
    /* Relative motion, applied to the game's cursor: the game reads it every frame, moves its own pointer by the
     * distance from the window centre and re-centres it (SetCursorPos). One scale for both axes: the picture is
     * shown aspect-correct (the 800x600 menus with bars at the sides). */
    uint32_t mw, mh; dsr_mode_size(&mw, &mh);
    double s = fmax(mw / self.bounds.size.width, mh / self.bounds.size.height);
    rel_motion(e.deltaX * s, e.deltaY * s);
}
- (void)mouseMoved:(NSEvent *)e { [self moved:e]; }
@end
static void rel_motion(double gx, double gy)
{
    static double fx, fy; fx += gx; fy += gy;
    int dx = (int)fx, dy = (int)fy; fx -= dx; fy -= dy;
    if (!dx && !dy) return;
    uint32_t mw, mh; dsr_mode_size(&mw, &mh);
    int x, y; w32_move_cursor(dx, dy, (int)mw, (int)mh, &x, &y); vcur_x = x; vcur_y = y;
    post(0x200, mk(), pos_lp());
}
@implementation GameView (Drag)
- (void)mouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)mouseDown:(NSEvent *)e { buttons |= 1; w32_keys[1] = 0x80; post(e.clickCount == 2 ? 0x203 : 0x201, mk(), pos_lp()); }
- (void)mouseUp:(NSEvent *)e { buttons &= ~1u; w32_keys[1] = 0; post(0x202, mk(), pos_lp()); }
- (void)rightMouseDown:(NSEvent *)e { buttons |= 2; w32_keys[2] = 0x80; post(e.clickCount == 2 ? 0x206 : 0x204, mk(), pos_lp()); }
- (void)rightMouseUp:(NSEvent *)e { buttons &= ~2u; w32_keys[2] = 0; post(0x205, mk(), pos_lp()); }
- (void)otherMouseDown:(NSEvent *)e { buttons |= 0x10; w32_keys[4] = 0x80; post(0x207, mk(), pos_lp()); }
- (void)otherMouseUp:(NSEvent *)e { buttons &= ~0x10u; w32_keys[4] = 0; post(0x208, mk(), pos_lp()); }
- (void)scrollWheel:(NSEvent *)e
{
    static double acc; acc += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
    while (acc >= 1 || acc <= -1) { int d = acc > 0 ? 1 : -1; acc -= d; post(0x20a, ((uint32_t)(int16_t)(120 * d) << 16) | mk(), pos_lp()); }
}
- (void)keyDown:(NSEvent *)e
{
    if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
    key_event(e.keyCode, 1, e.ARepeat);
    NSString *s = e.characters;
    for (NSUInteger k = 0; k < s.length; k++) {
        unichar ch = [s characterAtIndex:k]; uint8_t b; uint16_t w = ch;
        if (ch >= 0xf700) continue;                                   // function keys
        if (w32_wide_to_mb(1252, &w, 1, &b, 1) == 1) post(0x102, b, 1);
    }
}
- (void)keyUp:(NSEvent *)e { key_event(e.keyCode, 0, 0); }
- (void)flagsChanged:(NSEvent *)e
{
    static const struct { uint16_t code; NSEventModifierFlags flag; } mods[] = {
        {0x38, NSEventModifierFlagShift}, {0x3c, NSEventModifierFlagShift}, {0x3b, NSEventModifierFlagControl}, {0x3e, NSEventModifierFlagControl},
        {0x3a, NSEventModifierFlagOption}, {0x3d, NSEventModifierFlagOption}, {0x39, NSEventModifierFlagCapsLock}};
    for (size_t k = 0; k < sizeof mods / sizeof *mods; k++) if (mods[k].code == e.keyCode)
        key_event(e.keyCode, (e.modifierFlags & mods[k].flag) != 0, 0);
}
@end

// ---------------------------------------------------------------- window + capture
static void capture(int on)
{
    static int captured;
    if (test_mode) return;
    if (on == captured) return;
    captured = on;
    if (on) { [NSCursor hide]; CGAssociateMouseAndMouseCursorPosition(false); }
    else { CGAssociateMouseAndMouseCursorPosition(true); [NSCursor unhide]; }
}
static void window_created(uint32_t hwnd, int x, int y, int w, int h, uint32_t style)
{
    (void)x; (void)y; (void)w; (void)h; (void)style;
    if (game_hwnd) return;                                            // the game has one top-level window
    game_hwnd = hwnd;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSRect f = NSScreen.mainScreen.frame;
        NSWindowStyleMask sm = NSWindowStyleMaskBorderless;
        if (test_mode) { f = NSMakeRect(80, 80, 960, 620); sm = NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable; }
        win = [[GameWindow alloc] initWithContentRect:f styleMask:sm backing:NSBackingStoreBuffered defer:NO];
        win.releasedWhenClosed = NO;   /* ARC owns it (AppKit's default releases a closed window once more) */
        f.origin = NSZeroPoint;
        win.title = @"Dungeon Siege"; win.backgroundColor = NSColor.blackColor; win.acceptsMouseMovedEvents = YES;
        GameView *v = [[GameView alloc] initWithFrame:f]; v.wantsLayer = YES;
        win.contentView = v; [win makeFirstResponder:v];
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        [win setLevel:NSNormalWindowLevel];
    });
}
static void window_changed(uint32_t hwnd, int x, int y, int w, int h, int visible)
{
    (void)x; (void)y; (void)w; (void)h;
    if (hwnd != game_hwnd) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!win) return;
        if (visible && !win.visible) {
            if (test_mode) [win orderBack:nil];
            else {
                [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
                [win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; capture(1);
            }
        }
        else if (!visible && win.visible) { [win orderOut:nil]; capture(0); }
    });
}

/* The game's resolution lives in DungeonSiege2.ini (width/height at the top, before any [section]). With none set, use
 * this screen's size (in points); DS_RESOLUTION=WxH sets it explicitly. Test runs keep the game's own default unless
 * DS_RESOLUTION is given. */
static void ensure_resolution(const char *drive_c)
{
    char dir[1200], path[1300];
    snprintf(dir, sizeof dir, "%s/Users/player/Documents/My Games/Dungeon Siege 2", drive_c);
    snprintf(path, sizeof path, "%s/DungeonSiege2.ini", dir);
    if (getenv("DS_TEST") && !getenv("DS_RESOLUTION")) return;
    int w = w32_screen_w, h = w32_screen_h, force = 0;
    const char *r = getenv("DS_RESOLUTION");
    if (r && sscanf(r, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480) force = 1; else { w = w32_screen_w; h = w32_screen_h; }
    NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSISOLatin1StringEncoding error:nil] ?: @"";
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    if (lines.count && [lines.lastObject isEqualToString:@""]) [lines removeLastObject];
    int has_w = 0, has_h = 0, has_bpp = 0, top = 1;
    for (NSUInteger k = 0; k < lines.count; k++) {
        NSString *t = [lines[k] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([t hasPrefix:@"["]) top = 0;
        if (!top) continue;
        NSString *key = [[t componentsSeparatedByString:@"="].firstObject stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].lowercaseString;
        if ([key isEqualToString:@"width"]) { has_w = 1; if (force) lines[k] = [NSString stringWithFormat:@"width = %d", w]; }
        if ([key isEqualToString:@"height"]) { has_h = 1; if (force) lines[k] = [NSString stringWithFormat:@"height = %d", h]; }
        if ([key isEqualToString:@"bpp"]) has_bpp = 1;
    }
    if (has_w && has_h && !force) return;
    NSMutableArray<NSString *> *add = [NSMutableArray array];
    if (!has_w) [add addObject:[NSString stringWithFormat:@"width = %d", w]];
    if (!has_h) [add addObject:[NSString stringWithFormat:@"height = %d", h]];
    if (!has_bpp) [add addObject:@"bpp = 32"];
    [lines insertObjects:add atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, add.count)]];
    [[NSFileManager defaultManager] createDirectoryAtPath:@(dir) withIntermediateDirectories:YES attributes:nil error:nil];
    [[[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"] writeToFile:@(path) atomically:YES encoding:NSISOLatin1StringEncoding error:nil];
    fprintf(stderr, "DungeonSiegeNative: resolution %dx%d (%s)\n", w, h, path);
}

static void type_text(NSString *text)
{
    static const struct { unichar ch; uint16_t code; } letters[] = {{'a',0},{'s',1},{'d',2},{'f',3},{'h',4},{'g',5},{'z',6},{'x',7},{'c',8},{'v',9},
        {'b',11},{'q',12},{'w',13},{'e',14},{'r',15},{'y',16},{'t',17},{'1',18},{'2',19},{'3',20},{'4',21},{'6',22},{'5',23},{'9',25},{'7',26},
        {'8',28},{'0',29},{'o',31},{'u',32},{'i',34},{'p',35},{'l',37},{'j',38},{'k',40},{'n',45},{'m',46},{' ',49},
        {'.',47},{',',43},{'-',27},{'=',24},{'/',44},{';',41},{'\'',39}};
    void (^send)(NSString *, uint16_t) = ^(NSString *chars, uint16_t code) {
        for (int up = 0; up < 2; up++) {
            NSEvent *ev = [NSEvent keyEventWithType:up ? NSEventTypeKeyUp : NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                                       windowNumber:win.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
            [win sendEvent:ev];
        }
    };
    for (NSUInteger k = 0; k < text.length; k++) {
        if ([[text substringFromIndex:k] hasPrefix:@"{esc}"]) { send(@"\x1b", 0x35); k += 4; continue; }
        if ([[text substringFromIndex:k] hasPrefix:@"{enter}"]) { send(@"\r", 0x24); k += 6; continue; }
        if ([[text substringFromIndex:k] hasPrefix:@"{bs}"]) { send(@"\x7f", 0x33); k += 3; continue; }
        unichar ch = [text characterAtIndex:k], lo = (unichar)tolower(ch); uint16_t code = 0xffff;
        for (size_t j = 0; j < sizeof letters / sizeof *letters; j++) if (letters[j].ch == lo) code = letters[j].code;
        if (code != 0xffff) send([NSString stringWithCharacters:&ch length:1], code);
    }
}

@interface AppDelegate : NSObject <NSApplicationDelegate> @end
@implementation AppDelegate
- (void)applicationDidBecomeActive:(NSNotification *)n { (void)n; if (win.visible) capture(1); post(0x1c, 1, 0); }
- (void)applicationDidResignActive:(NSNotification *)n { (void)n; capture(0); post(0x1c, 0, 0); }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s { (void)s; return NSTerminateNow; }
@end

/* After a crash: say so, and offer the report (not in test mode). */
static void show_report_alert(NSString *title, NSString *path)
{
    NSAlert *al = [NSAlert new]; al.alertStyle = NSAlertStyleWarning; al.messageText = title;
    al.informativeText = [NSString stringWithFormat:@"A report was saved:\n%@\n\nIt says what the game was doing, so the problem can be found and fixed.", path];
    [al addButtonWithTitle:@"Show Report"]; [al addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([al runModal] == NSAlertFirstButtonReturn) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];
}
static void *game_thread(void *arg)
{
    (void)arg; char msg[256];
    uint32_t code = w32_run(msg, sizeof msg);
    fprintf(stderr, "DungeonSiegeNative: game ended: %s\n", msg);
    game_running = 0;
    const char *rp = !strncmp(msg, "fault", 5) ? w32_crash_last_path() : 0;
    NSString *path = rp ? @(rp) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        capture(0);
        if (path && !test_mode) {
            [win orderOut:nil]; [NSApp setPresentationOptions:NSApplicationPresentationDefault];
            show_report_alert(@"Dungeon Siege stopped unexpectedly.", path);
        }
        exit((int)code);
    });
    return 0;
}
/* A report from the last session that nobody has seen yet (a crash, or a hang the user had to force-quit). */
static void report_from_last_session(const char *data)
{
    NSString *dir = [NSString stringWithFormat:@"%s/CrashReports", data], *marker = [dir stringByAppendingPathComponent:@".last-shown"];
    NSString *seen = [NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSString *newest = nil;
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if (([f hasPrefix:@"crash-"] || ([f hasPrefix:@"hang-"] && ![f hasPrefix:@"hang-recovered-"])) && (!newest || [f compare:newest] == NSOrderedDescending)) newest = f;
    if (!newest || [newest compare:seen] != NSOrderedDescending) return;
    [newest writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
    show_report_alert([newest hasPrefix:@"hang-"] ? @"Last time, Dungeon Siege stopped responding." : @"Last time, Dungeon Siege stopped unexpectedly.",
                      [dir stringByAppendingPathComponent:newest]);
}
static void on_objc_exception(NSException *e)
{
    char r[400]; snprintf(r, sizeof r, "The macOS side of the program raised an Objective-C exception: %s: %s.", e.name.UTF8String, e.reason.UTF8String ?: "");
    w32_crash_report("crash", r, 0, 0);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *exe = 0, *game = 0, *data = 0;
        for (int k = 1; k + 1 < argc; k += 2) {
            if (!strcmp(argv[k], "--exe")) exe = argv[k + 1];
            else if (!strcmp(argv[k], "--game")) game = argv[k + 1];
            else if (!strcmp(argv[k], "--data")) data = argv[k + 1];
        }
        if (!exe || !game || !data) { fprintf(stderr, "usage: DungeonSiegeNative --exe <DungeonSiege2.exe> --game <game folder> --data <data folder>\n"); return 1; }
        NSString *bin = [[NSString stringWithUTF8String:argv[0]] stringByDeletingLastPathComponent];
        snprintf(dsr_snapshot_path, sizeof dsr_snapshot_path, "%s/dsr_snapshot.bin", bin.UTF8String);
        char drive_c[1024], overlay[1024];
        snprintf(drive_c, sizeof drive_c, "%s/drive_c", data); snprintf(overlay, sizeof overlay, "%s/game", data);
        mkdir(data, 0755); mkdir(drive_c, 0755); mkdir(overlay, 0755);
        NSRect sf = NSScreen.mainScreen.frame; w32_screen_w = (int)sf.size.width; w32_screen_h = (int)sf.size.height;
        w32_screen_scale = NSScreen.mainScreen.backingScaleFactor;
        init_keymap();
        {   /* the launch window (resolution, frame rate, updates) unless in test mode */
            int ds_launcher_run(const char *game_dir, const char *data_dir);
            if (!getenv("DS_TEST") || getenv("DS_LAUNCHER_SHOT")) {
                [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                if (!ds_launcher_run(game, data)) return 0;
            }
        }
        w32_crash_init(data, DS_BUILD); w32_crash_thread("macOS UI thread");
        { void w32_registry_install(const char *); w32_registry_install(data); }   /* values the game's installer would have written */
        NSSetUncaughtExceptionHandler(on_objc_exception);
        ensure_resolution(drive_c);
        test_mode = getenv("DS_TEST") && atoi(getenv("DS_TEST"));
        if (test_mode) parse_script();

        [NSApplication sharedApplication]; [NSApp setActivationPolicy:test_mode ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
        AppDelegate *del = [AppDelegate new]; NSApp.delegate = del;
        NSMenu *bar = [NSMenu new], *appm = [NSMenu new]; NSMenuItem *it = [NSMenuItem new]; it.submenu = appm; [bar addItem:it];
        [appm addItemWithTitle:@"Quit Dungeon Siege II" action:@selector(terminate:) keyEquivalent:@"q"]; NSApp.mainMenu = bar;

        layer = [CAMetalLayer layer]; layer.device = MTLCreateSystemDefaultDevice(); layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES; layer.opaque = YES; layer.contentsGravity = kCAGravityResizeAspect; layer.maximumDrawableCount = 3;
        layer.displaySyncEnabled = getenv("DSR_VSYNC") ? atoi(getenv("DSR_VSYNC")) != 0 : YES;
        layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

        void *m = mmap(0, DSR_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        H = m; ring = (uint8_t *)m + DSR_HEADER_SIZE; H->magic = DSR_SHM_MAGIC; H->version = 1;
        dsr_attach_consumer(H);
        w32_host_window_created = window_created; w32_host_window_changed = window_changed;
        if (w32_prepare(exe, game, drive_c, overlay)) { fprintf(stderr, "DungeonSiegeNative: cannot load %s\n", exe); return 1; }
        if (!test_mode) report_from_last_session(data);
        [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *tm) { (void)tm; w32_crash_heartbeat(2); }];   /* UI thread alive */
        if (test_mode && getenv("DS_CRASHTEST") && !strncmp(getenv("DS_CRASHTEST"), "uihang", 6))
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(getenv("DS_CRASHTEST") + 7) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ sleep(14); });

        pthread_attr_t a; pthread_t t;
        pthread_attr_init(&a); pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_create(&t, &a, render_thread, 0);
        pthread_attr_setstacksize(&a, 256u << 20);
        pthread_create(&t, &a, game_thread, 0);
        [NSApp run];
    }
    return 0;
}
