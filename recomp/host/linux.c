/* Native Linux host: SDL window/input, OpenGL DSR command consumer, shared recompiled game. */
#include <SDL2/SDL.h>
#include <png.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include "../runtime/win32/w32.h"
#include "../runtime/win32/display_modes.h"
#include "../runtime/platform.h"
#include "../runtime/edition.h"
#include "../../src/dsr/dsr_shm.h"
#include "../../src/renderer/linux/renderer.h"
#include "frame_trace.h"
#include "frame_pacer.h"
#ifdef SIMS_EDITION
/* The Sims draws with Vulkan (runtime/win32/vulkan.c): the window is a Vulkan window and the game presents to it
 * itself; this program pumps input and gives the runtime the loader and the window's surface. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL2/SDL_vulkan.h>
#define HOST_WINDOW_API SDL_WINDOW_VULKAN
#else
#define HOST_WINDOW_API SDL_WINDOW_OPENGL
#endif
#ifndef DS_BUILD
#define DS_BUILD "dev"
#endif
extern int w32_prepare(const char *,const char *,const char *,const char *);
extern uint32_t w32_run(char *,size_t);
extern void w32_registry_install(const char *);
extern void dsr_attach_consumer(DsrShmHeader *);
extern void dsr_mode_size(uint32_t *,uint32_t *);
extern void w32_post(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern uint32_t w32_window_tid(uint32_t);
extern void w32_set_cursor(int,int),w32_get_cursor(int *,int *),w32_move_cursor(int,int,int,int,int *,int *);
extern uint8_t w32_keys[256];
extern void w32_di_key(int,int),w32_di_mouse_move(int,int),w32_di_mouse_wheel(int),w32_di_mouse_button(int,int),w32_di_release_all(void);
extern void w32_relative_cursor(int);
extern int w32_screen_w,w32_screen_h,w32_screen_hz; extern double w32_screen_scale;
extern void (*w32_host_window_created)(uint32_t,int,int,int,int,uint32_t);
extern void (*w32_host_window_changed)(uint32_t,int,int,int,int,int);
extern char dsr_snapshot_path[1024];
extern void w32_crash_init(const char *,const char *),w32_crash_thread(const char *),w32_crash_heartbeat(int);
static _Atomic uint32_t game_hwnd;
static _Atomic int game_running=1; static uint32_t game_code;
static SDL_Window *window; static int test_mode; static uint32_t buttons;
static double vcur_x,vcur_y;
static volatile sig_atomic_t interrupted;
static void stop_host(int signal) { (void)signal; interrupted=1; }
static void set_window_icon(const char *bin) {
 char path[4096]; snprintf(path,sizeof path,"%s/dungeon-siege-2.png",bin);
 png_image icon={0}; icon.version=PNG_IMAGE_VERSION;
 if(!png_image_begin_read_from_file(&icon,path)) { png_image_free(&icon); return; }
 icon.format=PNG_FORMAT_RGBA;
 if(!icon.width||!icon.height||icon.width>1024||icon.height>1024) { png_image_free(&icon); return; }
 SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,icon.width,icon.height,32,SDL_PIXELFORMAT_RGBA32);
 if(surface) {
  if(png_image_finish_read(&icon,NULL,surface->pixels,surface->pitch,NULL)) SDL_SetWindowIcon(window,surface);
  SDL_FreeSurface(surface);
 }
 png_image_free(&icon);
}
static void post(uint32_t msg,uint32_t wp,uint32_t lp) { uint32_t w=game_hwnd; if(w) w32_post(w32_window_tid(w),w,msg,wp,lp); }
static uint32_t mk(void) { return buttons | (w32_keys[16]&128?4:0) | (w32_keys[17]&128?8:0); }
static uint32_t pos_lp(void) { int x,y; w32_get_cursor(&x,&y); return ((uint32_t)y<<16)|((uint32_t)x&65535); }
static void rel_motion(double gx,double gy) {
 static double fx,fy; fx+=gx; fy+=gy; int dx=(int)fx,dy=(int)fy; fx-=dx; fy-=dy; if(!dx&&!dy) return;
 uint32_t w,h; dsr_mode_size(&w,&h); int x,y; w32_move_cursor(dx,dy,w,h,&x,&y); vcur_x=x; vcur_y=y; post(0x200,mk(),pos_lp());
}
static void window_created(uint32_t w,int x,int y,int width,int height,uint32_t style) { (void)x;(void)y;(void)width;(void)height;(void)style; if(!game_hwnd) game_hwnd=w; }
static void window_changed(uint32_t w,int x,int y,int width,int height,int visible) { (void)w;(void)x;(void)y;(void)width;(void)height;(void)visible; }
static void *game_thread(void *arg) { (void)arg; char msg[256]; game_code=w32_run(msg,sizeof msg); fprintf(stderr,"game: %s\n",msg); game_running=0; return NULL; }
static void mkdirs(const char *path) { char s[2048]; snprintf(s,sizeof s,"%s",path); for(char *p=s+1;*p;p++) if(*p=='/') { *p=0; mkdir(s,0755); *p='/'; } mkdir(s,0755); }
#ifdef FNV_EDITION
/* Fallout NV: the first run makes FalloutPrefs.ini from the game's High preset, as its launcher does, at the display's
 * size (DS_RESOLUTION to override); the engine itself creates Fallout.ini from Fallout_default.ini */
static void fnv_prefs(const char *drive,const char *game) {
 char dir[1400],path[1500],src[1500];
 snprintf(dir,sizeof dir,"%s/Users/player/Documents/My Games/FalloutNV",drive); mkdirs(dir);
 snprintf(path,sizeof path,"%s/FalloutPrefs.ini",dir);
 int w=w32_screen_w,h=w32_screen_h; const char *res=getenv("DS_RESOLUTION");
 if(res && !ds_parse_resolution(res,&w,&h)) { fprintf(stderr,"Invalid DS_RESOLUTION\n"); exit(1); }
 FILE *in=fopen(path,"r"); if(in) { fclose(in); if(!res) return; } else { snprintf(src,sizeof src,"%s/high.ini",game); in=fopen(src,"r"); if(!in) return; fclose(in); snprintf(path,sizeof path,"%s",src); }
 in=fopen(path,"r"); if(!in) return;
 char out_path[1500],temp[1600]; snprintf(out_path,sizeof out_path,"%s/FalloutPrefs.ini",dir); snprintf(temp,sizeof temp,"%s.tmp",out_path);
 FILE *out=fopen(temp,"w"); if(!out) { fclose(in); return; }
 char line[2048],key[80];
 while(fgets(line,sizeof line,in)) {
  key[0]=0; sscanf(line,"%79[^=]",key);
  if(!strcasecmp(key,"iSize W")) { fprintf(out,"iSize W=%d\r\n",w); continue; }
  if(!strcasecmp(key,"iSize H")) { fprintf(out,"iSize H=%d\r\n",h); continue; }
  if(!strcasecmp(key,"bFull Screen")) { fprintf(out,"bFull Screen=%d\r\n",test_mode?0:1); continue; }
  if(!strcasecmp(key,"sD3DDevice")) continue;
  fputs(line,out);
  /* the engine runs its launcher (to redetect the card) unless this names the adapter d3d9.c reports */
  if(!strncasecmp(line,"[Display]",9)) fprintf(out,"sD3DDevice=\"Native Linux (OpenGL)\"\r\niSize W=%d\r\niSize H=%d\r\nbFull Screen=%d\r\n",w,h,test_mode?0:1);
 }
 fclose(in); fclose(out); rename(temp,out_path);
}
#endif
#ifdef FNV_EDITION
#include <dirent.h>
/* Havok steps at most fMaxTime seconds per frame (Fallout_default.ini: 0.016, made for 60 fps). Above that physics
 * jitters and objects fly; the usual fix is fMaxTime = 1 / frame rate. Fallout.ini (the engine's copy of
 * Fallout_default.ini, made on first run; any case) gets the value for the frame cap (unlimited: the display's rate). */
static void fnv_havok(const char *drive,const char *game) {
 const char *cap=getenv("DSR_FPSCAP"); double fps=cap?atof(cap):60; if(fps<=0) fps=w32_screen_hz>0?w32_screen_hz:60;
 if(fps<60) fps=60;
 char dir[1400],path[1700]={0},src[1500];
 snprintf(dir,sizeof dir,"%s/Users/player/Documents/My Games/FalloutNV",drive); mkdirs(dir);
 DIR *d=opendir(dir); struct dirent *e;
 while(d && (e=readdir(d))) if(!strcasecmp(e->d_name,"Fallout.ini")) { snprintf(path,sizeof path,"%s/%s",dir,e->d_name); break; }
 if(d) closedir(d);
 FILE *in;
 if(path[0]) in=fopen(path,"r");
 else { snprintf(src,sizeof src,"%s/Fallout_default.ini",game); in=fopen(src,"r"); snprintf(path,sizeof path,"%s/FALLOUT.INI",dir); }
 if(!in) return;
 char temp[1800]; snprintf(temp,sizeof temp,"%s.tmp",path); FILE *out=fopen(temp,"w"); if(!out) { fclose(in); return; }
 char line[2048],key[80]; int havok=0,done=0;
 while(fgets(line,sizeof line,in)) {
  if(line[0]=='[') { if(havok && !done) { fprintf(out,"fMaxTime=%.6f\r\n",1.0/fps); done=1; } havok=!strncasecmp(line,"[HAVOK]",7); }
  key[0]=0; sscanf(line,"%79[^=]",key);
  if(havok && !strcasecmp(key,"fMaxTime")) { if(!done) fprintf(out,"fMaxTime=%.6f\r\n",1.0/fps); done=1; continue; }
  fputs(line,out);
 }
 if(!done && havok) fprintf(out,"fMaxTime=%.6f\r\n",1.0/fps);                 /* [HAVOK] was the last section */
 else if(!done) fprintf(out,"\r\n[HAVOK]\r\nfMaxTime=%.6f\r\n",1.0/fps);
 fclose(in); fclose(out); rename(temp,path);
 fprintf(stderr,"host: Havok step for %.0f fps (fMaxTime %.6f)\n",fps,1.0/fps);
}
#define ENSURE_PREFS(drive,game) do { fnv_prefs(drive,game); fnv_havok(drive,game); } while(0)
#elif defined(SIMS_EDITION)
#define ENSURE_PREFS(drive,game) do { (void)(drive); (void)(game); } while(0)
#else
#define ENSURE_PREFS(drive,game) ensure_resolution(drive)
#endif
static void ensure_resolution(const char *drive) {
 char dir[1400],path[1500];
 snprintf(dir,sizeof dir,"%s/Users/player/Documents/My Games/" DS2_DOCUMENTS,drive); mkdirs(dir);
 snprintf(path,sizeof path,"%s/" DS2_INI,dir);
 int w=w32_screen_w,h=w32_screen_h; const char *res=getenv("DS_RESOLUTION");
 if(res && !ds_parse_resolution(res,&w,&h)) { fprintf(stderr,"Invalid DS_RESOLUTION\n"); exit(1); }
 FILE *in=fopen(path,"r"); if(in&&!res) { fclose(in); return; }
 char temp[1600]; snprintf(temp,sizeof temp,"%s.tmp",path); FILE *out=fopen(temp,"w"); if(!out) { if(in) fclose(in); return; }
 fprintf(out,"width = %d\nheight = %d\nbpp = 32\n",w,h);
 if(in) { char line[2048],key[80]; int top=1; while(fgets(line,sizeof line,in)) {
  const char *p=line; while(isspace((unsigned char)*p)) p++; if(*p=='[') top=0;
  key[0]=0; sscanf(p,"%79[^= \t]",key);
  if(top&&(!strcasecmp(key,"width")||!strcasecmp(key,"height")||!strcasecmp(key,"bpp"))) continue;
  fputs(line,out);
 } fclose(in); } fclose(out); rename(temp,path);
}
/* Windows virtual keys and PC set-1 scan codes. SDL scancodes describe physical keys. */
typedef struct { int vk,scan,ext; } Key;
static Key keymap(SDL_Scancode sc) {
 static const int letters[26]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
 if(sc>=SDL_SCANCODE_A&&sc<=SDL_SCANCODE_Z) return (Key){'A'+sc-SDL_SCANCODE_A,letters[sc-SDL_SCANCODE_A],0};
 if(sc>=SDL_SCANCODE_1&&sc<=SDL_SCANCODE_9) return (Key){'1'+sc-SDL_SCANCODE_1,2+sc-SDL_SCANCODE_1,0};
 if(sc>=SDL_SCANCODE_F1&&sc<=SDL_SCANCODE_F10) return (Key){0x70+sc-SDL_SCANCODE_F1,0x3b+sc-SDL_SCANCODE_F1,0};
 switch(sc) {
 #define K(s,v,c,e) case SDL_SCANCODE_##s: return (Key){v,c,e}
 K(0,'0',0x0b,0); K(RETURN,13,0x1c,0); K(ESCAPE,27,1,0); K(BACKSPACE,8,0xe,0); K(TAB,9,0xf,0); K(SPACE,32,0x39,0);
 K(LSHIFT,16,0x2a,0); K(RSHIFT,16,0x36,0); K(LCTRL,17,0x1d,0); K(RCTRL,17,0x1d,1); K(LALT,18,0x38,0); K(RALT,18,0x38,1);
 K(LEFT,37,0x4b,1); K(UP,38,0x48,1); K(RIGHT,39,0x4d,1); K(DOWN,40,0x50,1); K(INSERT,45,0x52,1); K(DELETE,46,0x53,1);
 K(HOME,36,0x47,1); K(END,35,0x4f,1); K(PAGEUP,33,0x49,1); K(PAGEDOWN,34,0x51,1); K(F11,0x7a,0x57,0); K(F12,0x7b,0x58,0);
 K(MINUS,0xbd,0xc,0); K(EQUALS,0xbb,0xd,0); K(LEFTBRACKET,0xdb,0x1a,0); K(RIGHTBRACKET,0xdd,0x1b,0); K(BACKSLASH,0xdc,0x2b,0);
 K(SEMICOLON,0xba,0x27,0); K(APOSTROPHE,0xde,0x28,0); K(GRAVE,0xc0,0x29,0); K(COMMA,0xbc,0x33,0); K(PERIOD,0xbe,0x34,0); K(SLASH,0xbf,0x35,0);
 K(CAPSLOCK,0x14,0x3a,0); K(KP_ENTER,13,0x1c,1);
 #undef K
 default: return (Key){0,0,0};
 }
}
static void key_event(Key k,int down,int repeat) {
 if(!k.vk) return;
 if(!repeat&&k.scan) w32_di_key(k.scan|(k.ext?0x80:0),down);   /* DirectInput: DIK codes are set-1 scan codes, extended keys +0x80 */ int prev=(w32_keys[k.vk]&128)!=0;
 w32_keys[k.vk]=down?(128|((w32_keys[k.vk]&1)^(repeat?0:1))):(w32_keys[k.vk]&1);
 if(k.vk==16) w32_keys[k.scan==0x36?0xa1:0xa0]=w32_keys[16];
 if(k.vk==17) w32_keys[k.ext?0xa3:0xa2]=w32_keys[17];
 if(k.vk==18) w32_keys[k.ext?0xa5:0xa4]=w32_keys[18];
 uint32_t lp=1u|((uint32_t)k.scan<<16)|((uint32_t)k.ext<<24)|((uint32_t)prev<<30)|(down?0:0x80000000u);
 int sys=(w32_keys[18]&128)||k.vk==18; post(down?(sys?0x104:0x100):(sys?0x105:0x101),k.vk,lp);
 if(down&&(k.vk==8||k.vk==9||k.vk==13)) post(0x102,k.vk,lp);
}
static void type_text(const char *text) {
 for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
  Key k={toupper(*p),0,0}; key_event(k,1,0); if(*p>=32) post(0x102,*p,1); key_event(k,0,0);
 }
}

typedef struct { uint32_t frame; double at; char kind[8]; int x, y, phase; char path[512]; } Act;   // at > 0: seconds from start
static Act acts[256]; static int nacts;
          // virtual cursor in game (display mode) pixels
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
            if (!strcmp(kind, "shot") || !strcmp(kind, "type") || !strcmp(kind, "ditype") || !strcmp(kind, "peek")) snprintf(a->path, sizeof a->path, "%s", rest); else sscanf(rest, "%d,%d", &a->x, &a->y);
            nacts++;
        }
        tok = strtok_r(0, ";", &save);
    }
    free(s);
}
/* DS2 reads an absolute cursor; allow its input update to observe a scripted move before pressing. */
static int mot_dx, mot_dy, mot_home, mot_pending;
static void motion_tick(void)
{
    if (!mot_home) return;
    vcur_x = mot_dx; vcur_y = mot_dy; w32_set_cursor(mot_dx, mot_dy);
    post(0x200, 0, ((uint32_t)mot_dy << 16) | ((uint32_t)mot_dx & 0xffff));
    mot_home = mot_dx = mot_dy = mot_pending = 0;
}
/* "type" action follows the same Windows key/character event path as SDL input. */
static void type_text(const char *text);
/* DS_CMDFILE=<path> (test mode): lines appended to the file while the game runs ("click:400,290", "type:Name",
 * "key:13", "shot:/tmp/a.png", ...) are carried out from the next frame on: for tests driven from outside.
 * "peek:7a05fc,a8,13" prints game memory: the dword at the first (hex) address, then each further offset added to the
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
        if (!strcmp(kind, "shot") || !strcmp(kind, "type") || !strcmp(kind, "ditype") || !strcmp(kind, "peek")) snprintf(a->path, sizeof a->path, "%s", rest); else sscanf(rest, "%d,%d", &a->x, &a->y);
        nacts++; fprintf(stderr, "host: command %s\n", line);
    }
    fclose(f);
}
static const char *script_tick(uint32_t fno)       /* returns a screenshot path if one is due */
{
    const char *shot = 0;
    cmdfile_poll(fno);
    static double t0; double now = ds_clock_ns() / 1e9; if (!t0) t0 = now;
    for (int k = 0; k < nacts; k++) if (acts[k].at > 0 && acts[k].frame == 0xffffffffu && now - t0 >= acts[k].at) acts[k].frame = fno;   /* timed steps start now */
    motion_tick();
    for (int k = 0; k < nacts; k++) {
        Act *a = &acts[k];
        if (a->frame == 0xffffffffu) continue;   /* timed action is not due; offset arithmetic would wrap */
        if (!strcmp(a->kind, "click") || !strcmp(a->kind, "rclick")) {
            int r = a->kind[0] == 'r';
            if (fno == a->frame && !a->phase) { mot_home = 8; mot_dx = a->x; mot_dy = a->y; a->phase=1; }
            if (a->phase==1 && fno >= a->frame + 20 && !mot_home && !mot_dx && !mot_dy && !mot_pending) {
                w32_keys[r ? 2 : 1] = 0x80; post(r ? 0x204 : 0x201, r ? 2 : 1, pos_lp()); a->phase=2; a->frame=fno;
            }
            if (a->phase==2 && fno == a->frame + 4) { w32_keys[r ? 2 : 1] = 0; post(r ? 0x205 : 0x202, 0, pos_lp()); a->phase=3; }
        } else if (!strcmp(a->kind, "lag") && fno == a->frame) { void dp8_set_lag(int, int); dp8_set_lag(a->x, a->y); }
        else if (!strcmp(a->kind, "move") && fno == a->frame) { mot_home = 8; mot_dx = a->x; mot_dy = a->y; }
        else if (!strcmp(a->kind, "rel") && fno >= a->frame && fno < a->frame + 60) rel_motion(a->x / 60.0, a->y / 60.0);   /* x,y over 60 frames, like a trackpad swipe */
        else if (!strcmp(a->kind, "key")) {
            if (fno == a->frame) { w32_keys[a->x & 0xff] = 0x80; post(0x100, (uint32_t)a->x, 1); }
            if (fno == a->frame + 3) { w32_keys[a->x & 0xff] = 0; post(0x101, (uint32_t)a->x, 0xc0000001u); }
        } else if (!strcmp(a->kind, "hold")) {            /* hold:vk,frames: a key held down (the modifier of a chord) */
            if (fno == a->frame) { w32_keys[a->x & 0xff] = 0x80; post(0x100, (uint32_t)a->x, 1); }
            if (fno == a->frame + (uint32_t)a->y) { w32_keys[a->x & 0xff] = 0; post(0x101, (uint32_t)a->x, 0xc0000001u); }
        } else if (!strcmp(a->kind, "dik")) {             /* dik:code: a key by its DirectInput code (set-1 scan code, extended +0x80) for 15
                                                              frames, as a real key press: DirectInput and window messages */
            Key k = {0, 0, 0};
            for (int sc = 0; sc < SDL_NUM_SCANCODES && !k.vk; sc++) { Key t = keymap((SDL_Scancode)sc); if (t.vk && (t.scan | (t.ext ? 0x80 : 0)) == a->x) k = t; }
            if (fno == a->frame) { if (k.vk) key_event(k, 1, 0); else w32_di_key(a->x, 1); }
            if (fno == a->frame + 15) { if (k.vk) key_event(k, 0, 0); else w32_di_key(a->x, 0); }
        } else if (!strcmp(a->kind, "dimove") && fno >= a->frame && fno < a->frame + 10) w32_di_mouse_move(a->x / 10, a->y / 10);   /* mickeys over 10 frames */
        else if (!strcmp(a->kind, "mmove") && fno >= a->frame && fno < a->frame + 10) { rel_motion(a->x / 10.0, a->y / 10.0); w32_di_mouse_move(a->x / 10, a->y / 10); }   /* as SDL motion: cursor, WM_MOUSEMOVE and DirectInput */
        else if (!strcmp(a->kind, "diclick")) {           /* diclick: the left button through DirectInput */
            if (fno == a->frame) { w32_di_mouse_button(0, 1); buttons |= 1; w32_keys[1] = 0x80; post(0x201, mk(), pos_lp()); }
            if (fno == a->frame + 15) { w32_di_mouse_button(0, 0); buttons &= ~1u; w32_keys[1] = 0; post(0x202, mk(), pos_lp()); }   /* held long enough for a game frame to see it */
        } else if (!strcmp(a->kind, "ditype") && fno >= a->frame) {   /* ditype:text: typed through DirectInput, one key every 3 frames */
            static const char *row1 = "1234567890-=", *rowq = "qwertyuiop[]", *rowa = "asdfghjkl;'`", *rowz = "zxcvbnm,./";
            uint32_t i = (fno - a->frame) / 3, ph = (fno - a->frame) % 3; size_t n = strlen(a->path);
            if (i < n) {
                char ch = (char)tolower((unsigned char)a->path[i]); const char *q; int dik = 0, shift = 0;
                if (a->path[i] >= 'A' && a->path[i] <= 'Z') shift = 1;
                if (ch == '_') { ch = '-'; shift = 1; }
                if (ch == ' ') dik = 0x39; else if (ch == '\n') dik = 0x1c;
                else if ((q = strchr(row1, ch))) dik = 2 + (int)(q - row1);
                else if ((q = strchr(rowq, ch))) dik = 0x10 + (int)(q - rowq);
                else if ((q = strchr(rowa, ch))) dik = 0x1e + (int)(q - rowa);
                else if ((q = strchr(rowz, ch))) dik = 0x2c + (int)(q - rowz);
                if (ch == '`') dik = 0x29;
                Key k = {0, 0, 0};
                for (int sc = 0; sc < SDL_NUM_SCANCODES && !k.vk; sc++) { Key t = keymap((SDL_Scancode)sc); if (t.vk && (t.scan | (t.ext ? 0x80 : 0)) == dik) k = t; }
                if (dik && ph == 0) { if (shift) w32_di_key(0x2a, 1); if (k.vk) key_event(k, 1, 0); else w32_di_key(dik, 1); post(0x102, (uint32_t)(unsigned char)a->path[i], 1); }
                if (dik && ph == 1) { if (k.vk) key_event(k, 0, 0); else w32_di_key(dik, 0); if (shift) w32_di_key(0x2a, 0); }
            }
        } else if (!strcmp(a->kind, "shot") && fno == a->frame) shot = a->path;
        else if (!strcmp(a->kind, "peek") && fno == a->frame) {
            extern uint8_t *G_MEM; char *e = a->path; uint32_t at = (uint32_t)strtoul(e, &e, 16), v = 0;
            for (;;) { if (at < 0x10000u) { v = 0; break; } memcpy(&v, G_MEM + at, 4); if (*e != ',') break; at = v + (uint32_t)strtoul(e + 1, &e, 16); }
            fprintf(stderr, "host: peek %s = 0x%08x (byte 0x%02x)\n", a->path, v, v & 0xff);
        }
        else if (!strcmp(a->kind, "wheel") && fno >= a->frame && fno < a->frame + (uint32_t)abs(a->x)) post(0x20a, ((uint32_t)(int16_t)(a->x > 0 ? 120 : -120) << 16), pos_lp());   /* x notches */
        else if (!strcmp(a->kind, "type") && fno == a->frame) {   /* keyboard path */
            type_text(a->path);
        }
    }
    return shot;
}

#ifdef SIMS_EDITION
static volatile uint32_t sims_frames;
static PFN_vkGetInstanceProcAddr sims_vk_loader(void) { if(SDL_Vulkan_LoadLibrary(NULL)) { fprintf(stderr,"host: Vulkan: %s\n",SDL_GetError()); return NULL; } return (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr(); }
static unsigned sims_vk_extensions(const char **names,unsigned cap) { unsigned n=cap; return SDL_Vulkan_GetInstanceExtensions(window,&n,names)?n:0; }
static int sims_vk_surface(VkInstance i,VkSurfaceKHR *s) { if(SDL_Vulkan_CreateSurface(window,i,s)) return 0; fprintf(stderr,"host: surface: %s\n",SDL_GetError()); return -1; }
static void sims_vk_presented(void) { __atomic_add_fetch(&sims_frames,1,__ATOMIC_RELEASE); w32_crash_heartbeat(3); }
#endif
static int events(void) {
 SDL_Event e;
 while(SDL_PollEvent(&e)) switch(e.type) {
 case SDL_QUIT: post(0x10,0,0); return 0;
 case SDL_KEYDOWN: case SDL_KEYUP:
  if(e.key.keysym.sym==SDLK_F11) {
   if(e.type==SDL_KEYUP || e.key.repeat) break;
   SDL_SetWindowFullscreen(window,(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN_DESKTOP)?0:SDL_WINDOW_FULLSCREEN_DESKTOP); break;
  }
  if(e.key.keysym.sym==SDLK_F12) { if(e.type==SDL_KEYDOWN && !e.key.repeat) SDL_SetRelativeMouseMode(!SDL_GetRelativeMouseMode()); break; }
  key_event(keymap(e.key.keysym.scancode),e.type==SDL_KEYDOWN,e.key.repeat); break;
 case SDL_TEXTINPUT: for(const unsigned char *p=(unsigned char *)e.text.text;*p;p++) if(*p<128) post(0x102,*p,1); break;
 case SDL_MOUSEMOTION: {
#ifdef SIMS_EDITION
  { /* the system cursor, in the window's coordinates scaled to the game's screen (its window covers it) */
   int ww,hh; SDL_GetWindowSize(window,&ww,&hh); if(ww<1||hh<1) break;
   int x=(int)(e.motion.x*(double)w32_screen_w/ww), y=(int)(e.motion.y*(double)w32_screen_h/hh);
   vcur_x=x; vcur_y=y; w32_set_cursor(x,y); post(0x200,mk(),pos_lp()); break; }
#endif
  uint32_t w,h; dsr_mode_size(&w,&h); int ww,hh; SDL_GetWindowSize(window,&ww,&hh);
  double scale=fmax(w/(double)ww,h/(double)hh); rel_motion(e.motion.xrel*scale,e.motion.yrel*scale); w32_di_mouse_move(e.motion.xrel,e.motion.yrel); break;
 }
 case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
  int down=e.type==SDL_MOUSEBUTTONDOWN,r=e.button.button==SDL_BUTTON_RIGHT,m=e.button.button==SDL_BUTTON_MIDDLE;
  w32_di_mouse_button(r?1:m?2:e.button.button==SDL_BUTTON_X1?3:e.button.button==SDL_BUTTON_X2?4:0,down);
  unsigned b=r?2:m?16:1; if(down) buttons|=b; else buttons&=~b; w32_keys[r?2:m?4:1]=down?128:0;
  post(m?(down?0x207:0x208):r?(down?0x204:0x205):(down?0x201:0x202),mk(),pos_lp()); break;
 }
 case SDL_MOUSEWHEEL: w32_di_mouse_wheel(e.wheel.y*120); post(0x20a,((uint32_t)(int16_t)(e.wheel.y*120)<<16)|mk(),pos_lp()); break;
 case SDL_WINDOWEVENT:
  if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST) { SDL_SetRelativeMouseMode(SDL_FALSE); w32_relative_cursor(0); memset(w32_keys,0,sizeof w32_keys); buttons=0; w32_di_release_all(); post(0x1c,0,0); }
  if(e.window.event==SDL_WINDOWEVENT_FOCUS_GAINED) { if(!test_mode) SDL_SetRelativeMouseMode(SDL_TRUE); w32_relative_cursor(0);  /* DS2 reads the absolute virtual cursor. */ post(0x1c,1,0); }
  break;
 }
 return 1;
}
int main(int argc,char **argv) {
 const char *exe=NULL,*game=NULL,*data=NULL;
 for(int i=1;i<argc;i++) {
  if(i+1<argc&&!strcmp(argv[i],"--exe")) exe=argv[++i];
  else if(i+1<argc&&!strcmp(argv[i],"--game")) game=argv[++i];
  else if(i+1<argc&&!strcmp(argv[i],"--data")) data=argv[++i];
  else { fprintf(stderr,"Usage: %s --exe <verified game exe> --game <game folder> --data <writable data folder>\n",argv[0]); return 1; }
 }
 if(!exe||!game||!data) { fprintf(stderr,"--exe, --game and --data are required\n"); return 1; }
 test_mode=getenv("DS_TEST")&&atoi(getenv("DS_TEST"));
 signal(SIGINT,stop_host); signal(SIGTERM,stop_host);
 const char *title=getenv("DS_TEST_LAB") && !strcmp(getenv("DS_TEST_LAB"),"1")
     ? DS2_TITLE " — Test Lab" : DS2_TITLE " — Linux";
 SDL_SetHint(SDL_HINT_APP_NAME,title);
 SDL_SetHint("SDL_APP_ID","dungeon-siege-2-linux");
 SDL_SetHint("SDL_VIDEO_X11_WMCLASS","dungeon-siege-2-linux");
 SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS","dungeon-siege-2-linux");
 if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
 SDL_DisplayMode dm; if(!SDL_GetDesktopDisplayMode(0,&dm)) { w32_screen_w=dm.w; w32_screen_h=dm.h; if(dm.refresh_rate>0) w32_screen_hz=dm.refresh_rate; }
 /* Linux automatic mode follows the display, without the legacy 120 fps ceiling. */
 const char *fps=getenv("DSR_FPSCAP");
 if(!fps || !strcmp(fps,"auto") || !strcmp(fps,"display")) {
  int hz=w32_screen_hz;
#ifdef FNV_EDITION
  if(hz>60) hz=60;   /* Gamebryo's physics and animation misbehave above 60 fps (as on Windows without a frame limiter); DSR_FPSCAP=<n> overrides */
#endif
  char rate[32]; snprintf(rate,sizeof rate,"%d",hz); setenv("DSR_FPSCAP",rate,1);
 }
 w32_screen_scale=1;
 char drive[1200],overlay[1200],bin[2048]; ssize_t len=readlink("/proc/self/exe",bin,sizeof bin-1);
 if(len<=0) return 1; bin[len]=0; char *slash=strrchr(bin,'/'); if(slash) *slash=0;
 snprintf(dsr_snapshot_path,sizeof dsr_snapshot_path,"%s/dsr_snapshot.bin",bin);
 snprintf(drive,sizeof drive,"%s/drive_c",data); snprintf(overlay,sizeof overlay,"%s/game",data); mkdirs(drive); mkdirs(overlay); ENSURE_PREFS(drive,game);
 w32_crash_init(data,DS_BUILD); w32_crash_thread("Linux UI and renderer");
 /* Match the Mac host: publish the launcher's saved CD-key hash before the
  * game's multiplayer code looks up the retail installer's DoubleHash value. */
 w32_registry_install(data);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);   /* the game's shaders are GLSL 1.20 (MojoShader) */ SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
 SDL_GL_SetAttribute(SDL_GL_RED_SIZE,8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,8); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,8);
 SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,0); SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE,0);
 const char *mode=getenv("DS_WINDOW_MODE"); int fullscreen=!test_mode && (!mode || strcmp(mode,"windowed"));
 int ww=fullscreen?w32_screen_w:w32_screen_w*4/5, wh=fullscreen?w32_screen_h:w32_screen_h*4/5;
 if(test_mode) { ww=1024; wh=768; }
 window=SDL_CreateWindow(title,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,ww,wh,HOST_WINDOW_API|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI|(test_mode?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN)|(fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0));
 if(!window) { fprintf(stderr,"window: %s\n",SDL_GetError()); return 1; }
 set_window_icon(bin);
 #ifdef SIMS_EDITION
 SDL_StartTextInput(); w32_relative_cursor(0);                    /* the Sims' cursor is the system's */
 if(test_mode) { w32_screen_w=ww; w32_screen_h=wh; }               /* the game sizes its frame to the screen: the test window's */
 extern PFN_vkGetInstanceProcAddr (*w32_host_vk_loader)(void); extern unsigned (*w32_host_vk_extensions)(const char **, unsigned);
 extern int (*w32_host_vk_surface)(VkInstance, VkSurfaceKHR *); extern void (*w32_host_vk_presented)(void);
 w32_host_vk_loader=sims_vk_loader; w32_host_vk_extensions=sims_vk_extensions; w32_host_vk_surface=sims_vk_surface; w32_host_vk_presented=sims_vk_presented;
 #else
 SDL_GLContext gl=SDL_GL_CreateContext(window); if(!gl) { fprintf(stderr,"OpenGL: %s\n",SDL_GetError()); return 1; }
 SDL_GL_SetSwapInterval(test_mode?0:getenv("DSR_VSYNC")?atoi(getenv("DSR_VSYNC")):1); SDL_StartTextInput(); if(!test_mode) SDL_SetRelativeMouseMode(SDL_TRUE);
 w32_relative_cursor(0);  /* DS2 reads the absolute virtual cursor. */
 #endif
 SDL_GetWindowSize(window,&ww,&wh);
 fprintf(stderr,"host: %s, %s %dx%d, title '%s'\n",SDL_GetCurrentVideoDriver(),fullscreen?"fullscreen":"windowed",ww,wh,SDL_GetWindowTitle(window));
 fprintf(stderr,"host: display %d Hz, FPS cap %s, swap interval %d\n",w32_screen_hz,getenv("DSR_FPSCAP"),SDL_GL_GetSwapInterval());
 #ifndef SIMS_EDITION
 DSRRenderer *renderer=dsr_gl_create(bin); if(!renderer) return 1;
 #endif
 DsrShmHeader *h=mmap(NULL,DSR_SHM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(h==MAP_FAILED) { perror("command ring"); return 1; }
 h->magic=DSR_SHM_MAGIC; h->version=1; uint8_t *ring=(uint8_t *)h+DSR_HEADER_SIZE;
 #ifndef SIMS_EDITION
 dsr_gl_readback(renderer,(uint8_t *)h+DSR_READBACK_OFFSET,DSR_READBACK_SIZE,&h->readback_done); dsr_gl_queries(renderer,(volatile uint32_t *)((uint8_t *)h+DSR_QUERY_OFFSET));
 #endif
 dsr_attach_consumer(h);
 w32_host_window_created=window_created; w32_host_window_changed=window_changed;
 if(w32_prepare(exe,game,drive,overlay)) return 1;
 { void w32_lab_init(const char *); w32_lab_init(data); }
 if(test_mode) parse_script();
 __atomic_store_n(&h->host_ready,1,__ATOMIC_RELEASE);
 pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,256u<<20); pthread_t thread;
 if(pthread_create(&thread,&attr,game_thread,NULL)) return 1; pthread_attr_destroy(&attr);
 uint32_t read=0,frames=0; uint64_t start=ds_clock_ns(),last=start; int quit=0;
 DSFrameTrace trace={0}; ds_trace_init(&trace);
 DSFramePacer pacer={0}; ds_pacer_init(&pacer,atof(getenv("DSR_FPSCAP")),w32_screen_hz,SDL_GL_GetSwapInterval()!=0);
 double stop=getenv("DS_EXIT_AFTER")?atof(getenv("DS_EXIT_AFTER")):0;
#ifdef SIMS_EDITION
 (void)ring; (void)read; (void)last; (void)pacer; (void)trace;
 while(game_running&&!quit&&!interrupted) {                       /* the game presents; this thread is input */
  if(!events()) break; w32_crash_heartbeat(2);
  if(stop>0 && (ds_clock_ns()-start)/1e9>=stop) break;
  uint32_t f=__atomic_load_n(&sims_frames,__ATOMIC_ACQUIRE);
  while(frames!=f) { frames++; if(test_mode) { const char *shot=script_tick(frames); (void)shot; } }   /* every frame: steps wait for exact ones */
  SDL_WaitEventTimeout(NULL,2);
 }
#else
 while(game_running&&!quit&&!interrupted) {
  if(!events()) break; w32_crash_heartbeat(2);
  if(stop>0 && (ds_clock_ns()-start)/1e9>=stop) break;
  uint32_t write=__atomic_load_n(&h->write_pos,__ATOMIC_ACQUIRE); int count=0;
  while(read!=write && count++<1024) {
   uint32_t off=read%DSR_RING_SIZE; if(DSR_RING_SIZE-off<8) { read+=DSR_RING_SIZE-off; continue; }
   uint32_t op,size; memcpy(&op,ring+off,4); memcpy(&size,ring+off+4,4);
   if(size>DSR_RING_SIZE-8 || off+8ull+size>DSR_RING_SIZE) { fprintf(stderr,"invalid render command\n"); quit=1; break; }
   int present=op==DSR_WRAP?0:dsr_gl_exec(renderer,op,ring+off+8,size);
   uint64_t ready=0; if(present && size>=16) { uint32_t stamp[3]; memcpy(stamp,ring+off+12,12); if(stamp[2]) ready=((uint64_t)stamp[1]<<32|stamp[0])*1000; }
   read+=8+size; __atomic_store_n(&h->read_pos,read,__ATOMIC_RELEASE);
   if(present) {
    frames++; const char *shot=test_mode?script_tick(frames):NULL;
    if(shot && getenv("DS_SHOT_RAW")) dsr_gl_screenshot(renderer,shot);
    int w,hh; SDL_GL_GetDrawableSize(window,&w,&hh); dsr_gl_present(renderer,w,hh);
    if(shot && !getenv("DS_SHOT_RAW")) { if(!dsr_gl_display_screenshot(shot,w,hh)) fprintf(stderr,"frame %u saved to %s\n",frames,shot); }
    uint64_t deadline=ds_pacer_deadline(&pacer,ds_clock_ns());
    for(;;) {
     uint64_t now=ds_clock_ns(); if(now>=deadline) break;
     if(deadline-now>200000) {
      if(!events()) { quit=1; break; }
      uint64_t wake=deadline-150000; if(wake>now+500000) wake=now+500000;
      struct timespec ts={(time_t)(wake/1000000000),(long)(wake%1000000000)};
      clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL);
     }
    }
    uint64_t before_swap=ds_clock_ns(); SDL_GL_SwapWindow(window); ds_pacer_advance(&pacer);
    uint32_t trace_w,trace_h; dsr_gl_size(renderer,&trace_w,&trace_h);
    ds_trace_add(&trace,ready,before_swap,ds_clock_ns(),trace_w,trace_h);
    /* Poll fresh input before releasing the next simulation update. */
    if(!events()) quit=1;
    __atomic_store_n(&h->frames_done,frames,__ATOMIC_RELEASE); dsr_wake(&h->frames_done); w32_crash_heartbeat(3);
    uint64_t now=ds_clock_ns(); if(test_mode&&now-last>=1000000000) { uint32_t mw,mh; dsr_gl_size(renderer,&mw,&mh); fprintf(stderr,"frame %u (%ux%u), draws %ld, dropped %ld/%ld\n",frames,mw,mh,dsr_stat[0],dsr_stat[1],dsr_stat[2]); last=now; }
    break;
   }
  }
  if(read==write) {
   __atomic_store_n(&h->consumer_waiting,1,__ATOMIC_SEQ_CST);
   uint32_t expected=__atomic_load_n(&h->write_pos,__ATOMIC_ACQUIRE);
   if(read==expected) { dsr_gl_poll_queries(renderer); dsr_wait(&h->write_pos,expected,500000); }
   __atomic_store_n(&h->consumer_waiting,0,__ATOMIC_SEQ_CST);
  }
 }
#endif
 SDL_SetRelativeMouseMode(SDL_FALSE);
 ds_trace_save(&trace);
 /* Guest service threads share the renderer and address space until process exit. */
 fprintf(stderr,"host: %u frames presented\n",frames);
 return game_running?0:(int)game_code;
}
