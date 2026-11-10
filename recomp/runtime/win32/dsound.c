/* DirectSound on the host audio output: what the recompiled Miles Sound System mixes into on Windows ("DirectSound - MSS
 * Mixer"), and what Fallout NV drives directly through DirectSound 8. Software buffers only: a primary buffer that just
 * gates output (and carries the 3D listener), and secondary buffers that are rings in guest memory, mixed by the render
 * callback (any PCM rate/format, volume in 1/100 dB, pan). 3D buffers are placed relative to the listener: inverse
 * distance rolloff between their minimum and maximum distance, and an equal-power pan from the listener's right axis.
 * Effects (I3DL2 reverb) are accepted and not applied. Play and write cursors advance with the device, as Miles'
 * fragment mixer expects.
 *
 * Each buffer's guest object holds three interface pointers, each followed by the buffer's index: +0 the buffer
 * (IDirectSoundBuffer8), +8 its IDirectSound3DBuffer (the primary buffer's: IDirectSound3DListener), +16 its effect. */
#include "w32.h"
#include "host_audio.h"
#include <math.h>
#include <stdatomic.h>

enum { DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_NOINTERFACE = 0x80004002u, DSERR_UNSUPPORTED = 0x80004001u,
       DSERR_GENERIC = 0x80004005u, DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4,
       DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2, OUT_RATE = 44100 };
typedef struct Buf {
    int used, primary, refs; uint32_t obj, mem, size, flags;           /* guest object, guest ring, bytes */
    int ch, bits, rate, align; double gain_l, gain_r; int32_t vol, pan; uint32_t freq;
    _Atomic uint32_t play; double frac; _Atomic int playing, looping;
    int is3d, mode; float pos[3], vel[3], mind, maxd; uint32_t cone[2]; float coneor[3]; int32_t conevol;
    FILE *dump; uint32_t dump_frames, serial;           /* W32_DSDUMP: the stream as played, before gain */
} Buf;
#define NBUF 512
static Buf bufs[NBUF];
static struct { float pos[3], vel[3], front[3], top[3], dist, rolloff, doppler; } lis = {{0}, {0}, {0, 0, 1}, {0, 1, 0}, 1, 1, 1}; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vt_ds, vt_buf, vt_3d, vt_lis, vt_fx, ds_obj; static int ds_refs;
static DsAudio * au; static _Atomic int primary_playing;
static _Atomic uint32_t slice_max = 512;                  /* largest render the device asked for: the write cursor's lead */

static Buf *B(uint32_t obj)                            /* any of a buffer's interface pointers -> the buffer */
{
    uint32_t k = rt_r32(G_MEM, obj + 4);
    return k < NBUF && bufs[k].used && (bufs[k].obj == obj || bufs[k].obj + 8 == obj || bufs[k].obj + 16 == obj) ? &bufs[k] : 0;
}
static float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
/* the 3D gains of a buffer: distance attenuation and pan relative to the listener */
static void gains3d(const Buf *b, double *gl, double *gr)
{
    *gl = b->gain_l; *gr = b->gain_r;
    if (!b->is3d || b->mode == 2) return;                                  /* DS3DMODE_DISABLE */
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = b->mode == 1 ? b->pos[k] : b->pos[k] - lis.pos[k];   /* HEADRELATIVE */
    double d = sqrt((double)r[0] * r[0] + (double)r[1] * r[1] + (double)r[2] * r[2]), mn = b->mind > 0 ? b->mind : 1, mx = b->maxd > mn ? b->maxd : mn;
    double dd = d < mn ? mn : d > mx ? mx : d, att = mn / (mn + lis.rolloff * (dd - mn));
    const float *f = lis.front, *t = lis.top;
    double rx = t[1] * f[2] - t[2] * f[1], ry = t[2] * f[0] - t[0] * f[2], rz = t[0] * f[1] - t[1] * f[0];      /* right = top x front */
    double rl = sqrt(rx * rx + ry * ry + rz * rz), side = d > 1e-6 && rl > 1e-6 ? (r[0] * rx + r[1] * ry + r[2] * rz) / (d * rl) : 0;
    if (side > 1) side = 1; if (side < -1) side = -1;
    double a = (side + 1) * 0.7853981633974483;                            /* equal power: left cos, right sin, 1/sqrt2 each in the centre */
    *gl *= att * cos(a) * 1.4142135623730951; *gr *= att * sin(a) * 1.4142135623730951;
    if (*gl > b->gain_l * att) *gl = b->gain_l * att; if (*gr > b->gain_r * att) *gr = b->gain_r * att;
}
static void set_gain(Buf *b)
{
    double v = b->vol <= -10000 ? 0 : pow(10.0, b->vol / 2000.0);                         /* 1/100 dB */
    double l = b->pan > 0 ? pow(10.0, -b->pan / 2000.0) : 1, r = b->pan < 0 ? pow(10.0, b->pan / 2000.0) : 1;
    b->gain_l = v * l; b->gain_r = v * r;
}
static inline float sample(Buf *b, uint32_t frame, int chn)
{
    uint32_t at = frame * b->align + (b->ch == 2 ? chn : 0) * (b->bits / 8);
    return b->bits == 16 ? (int16_t)rt_r16(G_MEM, b->mem + at) / 32768.0f : ((int)G_MEM[b->mem + at] - 128) / 128.0f;
}
void w32_audio_out(const float *lr, uint32_t frames, int rate, int source);         /* winmm.c: mute, level log, dump */
static void render(void *u, void *buffer, uint32_t frames)
{
    (void)u;
    float *out = buffer; memset(out, 0, frames * 8);
    if (frames > slice_max) slice_max = frames;
    for (int k = 0; k < NBUF; k++) {          /* secondaries play whether or not the primary was Play()ed: DirectSound runs it implicitly */
        Buf *b = &bufs[k]; if (!b->used || b->primary || !b->playing) continue;
        double gl, gr; gains3d(b, &gl, &gr);
        uint32_t nframes = b->size / b->align, pos = b->play / b->align; double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE, fr = b->frac;
        static const char *dumpdir; static int dd_init; if (!dd_init) { dd_init = 1; dumpdir = getenv("W32_DSDUMP"); }
        if (dumpdir && !b->dump) {
            static uint32_t serial; char path[1024]; b->serial = ++serial;
            snprintf(path, sizeof path, "%s/%05u-buf%03d-src%dHz-%dch-out%dHz.raw", dumpdir, b->serial, k, b->freq ? (int)b->freq : b->rate, b->ch, OUT_RATE);   /* samples at the output rate */
            b->dump = fopen(path, "wb");
        }
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t p0 = pos, p1 = pos + 1; float t = (float)fr;
            if (!b->looping && p1 >= nframes) p1 = p0;
            /* Wrap each interpolation endpoint once, shared by both channels. */
            if (p0 >= nframes) p0 %= nframes;
            if (p1 >= nframes) p1 %= nframes;
            float l = sample(b, p0, 0) * (1 - t) + sample(b, p1, 0) * t, r = sample(b, p0, 1) * (1 - t) + sample(b, p1, 1) * t;
            if (b->dump) { int16_t v[2] = {(int16_t)(l * 32767), (int16_t)(r * 32767)}; fwrite(v, 4, 1, b->dump); }
            out[2 * i] += (float)(l * gl); out[2 * i + 1] += (float)(r * gr);
            fr += step; while (fr >= 1) { fr -= 1; pos++; }
            if (pos >= nframes) { if (b->looping) pos -= nframes; else { b->playing = 0; pos = 0; break; } }
        }
        b->frac = fr; b->play = pos * b->align;
    }
    w32_audio_out(out, frames, OUT_RATE, 1);
    if (getenv("W32_AUDIO_MUTE")) memset(out, 0, frames * 8);

}
static int start_output(void)
{
    if (au) return 0;
    au = ds_audio_open(OUT_RATE, 2, 32, 1, render, NULL);
    if (!au) return -1;
    return 0;
}
static void set_format(Buf *b, uint32_t wfx)
{
    b->ch = rt_r16(G_MEM, wfx + 2); b->rate = (int)rt_r32(G_MEM, wfx + 4); b->bits = rt_r16(G_MEM, wfx + 14);
    b->align = b->ch * b->bits / 8; if (b->align <= 0) b->align = 4;
}
#define LOG(...) do { if (getenv("W32_DSLOG")) fprintf(stderr, "dsound: " __VA_ARGS__); } while (0)

/* ---- IDirectSoundBuffer ---- */
static void b_QueryInterface(Ctx *c)                   /* the buffer (8), its 3D interface, or the listener on the primary */
{
    Buf *b = B(ARG(0)); uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2), r = 0;
    if (!b || !out) RET(DSERR_INVALIDPARAM, 3);
    if (iid == 0 || iid == 0x279afa85u || iid == 0x6825a449u) r = b->obj;                       /* IUnknown, IDirectSoundBuffer(8) */
    else if (iid == 0x279afa86u && !b->primary) r = b->obj + 8;                                   /* IDirectSound3DBuffer */
    else if (iid == 0x279afa84u && b->primary) r = b->obj + 8;                                    /* IDirectSound3DListener */
    LOG("buffer QueryInterface %08x -> %s\n", iid, r ? "ok" : "no interface");
    rt_w32(G_MEM, out, r); if (r) b->refs++;
    RET(r ? DS_OK : DSERR_NOINTERFACE, 3);
}
static void b_AddRef(Ctx *c) { Buf *b = B(ARG(0)); RET(b ? ++b->refs : 0, 1); }
static void b_Release(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(0, 1);
    int r = --b->refs; if (r <= 0) { b->playing = 0; if (b->dump) { fclose(b->dump); b->dump = 0; } pthread_mutex_lock(&lock); b->used = 0; pthread_mutex_unlock(&lock); LOG("release buffer\n"); }
    RET(r > 0 ? r : 0, 1);
}
static void b_GetCaps(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1); if (!b) RET(DSERR_INVALIDPARAM, 2);
    rt_w32(G_MEM, p + 4, b->flags | 0x40000 /* DSBCAPS_LOCSOFTWARE */); rt_w32(G_MEM, p + 8, b->size); rt_w32(G_MEM, p + 12, 0); rt_w32(G_MEM, p + 16, 0);
    RET(DS_OK, 2);
}
static uint32_t write_ahead(Buf *b) { return (uint32_t)((uint64_t)(b->freq ? b->freq : (uint32_t)b->rate) * slice_max / OUT_RATE + 1) * b->align; }
static void b_GetCurrentPosition(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 3);
    uint32_t p = b->primary ? 0 : b->play, w = b->primary ? 0 : (p + write_ahead(b)) % b->size;
    if (ARG(1)) rt_w32(G_MEM, ARG(1), p); if (ARG(2)) rt_w32(G_MEM, ARG(2), w);
    RET(DS_OK, 3);
}
static void b_GetFormat(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1), cap = ARG(2); if (!b) RET(DSERR_INVALIDPARAM, 4);
    uint8_t w[18] = {0}; uint16_t v16; uint32_t v32;
    v16 = 1; memcpy(w, &v16, 2); v16 = (uint16_t)b->ch; memcpy(w + 2, &v16, 2); v32 = (uint32_t)b->rate; memcpy(w + 4, &v32, 4);
    v32 = (uint32_t)(b->rate * b->align); memcpy(w + 8, &v32, 4); v16 = (uint16_t)b->align; memcpy(w + 12, &v16, 2); v16 = (uint16_t)b->bits; memcpy(w + 14, &v16, 2);
    if (p) memcpy(GP(p), w, cap < 18 ? cap : 18);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), 18);
    RET(DS_OK, 4);
}
static void b_GetVolume(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->vol); RET(DS_OK, 2); }
static void b_GetPan(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->pan); RET(DS_OK, 2); }
static void b_GetFrequency(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), b->freq ? b->freq : (uint32_t)b->rate); RET(DS_OK, 2); }
static void b_GetStatus(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    int pl = b->primary ? primary_playing : b->playing;
    rt_w32(G_MEM, ARG(1), (pl ? DSBSTATUS_PLAYING : 0) | (pl && b->looping ? DSBSTATUS_LOOPING : 0)); RET(DS_OK, 2);
}
static void b_Initialize(Ctx *c) { RET(0x88780082u /* DSERR_ALREADYINITIALIZED */, 3); }
static void b_Lock(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), fl = ARG(7);
    if (!b || b->primary || !b->size) RET(DSERR_INVALIDPARAM, 8);
    if (fl & DSBLOCK_FROMWRITECURSOR) off = (b->play + write_ahead(b)) % b->size;
    if (fl & DSBLOCK_ENTIREBUFFER) n = b->size;
    if (n > b->size || off >= b->size) RET(DSERR_INVALIDPARAM, 8);
    uint32_t a = b->size - off < n ? b->size - off : n;
    rt_w32(G_MEM, p1, b->mem + off); rt_w32(G_MEM, l1, a);
    if (p2) rt_w32(G_MEM, p2, n > a ? b->mem : 0); if (l2) rt_w32(G_MEM, l2, n - a);
    RET(DS_OK, 8);
}
static void b_Play(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (start_output()) RET(DSERR_GENERIC, 4);
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    if (b->primary) primary_playing = 1; else b->playing = 1;
    LOG("play %s looping %d\n", b->primary ? "primary" : "secondary", b->looping);
    RET(DS_OK, 4);
}
static void b_SetCurrentPosition(Ctx *c) { Buf *b = B(ARG(0)); if (!b || b->primary) RET(DSERR_INVALIDPARAM, 2); b->play = ARG(1) % (b->size ? b->size : 1) / b->align * b->align; b->frac = 0; RET(DS_OK, 2); }
static void b_SetFormat(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    set_format(b, ARG(1)); LOG("format %d Hz %d bits %d ch (%s)\n", b->rate, b->bits, b->ch, b->primary ? "primary" : "secondary");
    RET(DS_OK, 2);
}
static void b_SetVolume(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->vol = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetPan(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->pan = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetFrequency(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->freq = ARG(1); RET(DS_OK, 2); }
static void b_Stop(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 1);
    if (b->primary) primary_playing = 0; else b->playing = 0;
    RET(DS_OK, 1);
}
static void b_Unlock(Ctx *c) { RET(DS_OK, 5); }
static void b_Restore(Ctx *c) { RET(DS_OK, 1); }

/* ---- IDirectSound ---- */
static void d_QueryInterface(Ctx *c)                   /* IDirectSound, IDirectSound8 */
{
    uint32_t iid = rt_r32(G_MEM, ARG(1));
    if (iid == 0 || iid == 0x279afa83u || iid == 0xc50a7e93u) { rt_w32(G_MEM, ARG(2), ds_obj); ds_refs++; RET(DS_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(DSERR_NOINTERFACE, 3);
}
static void d_VerifyCertification(Ctx *c) { rt_w32(G_MEM, ARG(1), 0 /* DS_CERTIFIED */); RET(DS_OK, 2); }
static void d_AddRef(Ctx *c) { RET(++ds_refs, 1); }
static void d_Release(Ctx *c) { int r = --ds_refs; RET(r < 0 ? 0 : r, 1); }
static uint32_t new_buf(Buf **out)
{
    pthread_mutex_lock(&lock);
    int k = 0; while (k < NBUF && bufs[k].used) k++;
    if (k == NBUF) { pthread_mutex_unlock(&lock); static int w; if (!w++) fprintf(stderr, "dsound: more than %d sound buffers\n", NBUF); return 0; }
    Buf *b = &bufs[k]; uint32_t keep = b->obj; memset(b, 0, sizeof *b); b->used = 1; b->refs = 1; b->mind = 1; b->maxd = 1e9f; set_gain(b);
    b->obj = keep ? keep : heap_alloc(w32_process_heap, 8, 24);           /* a slot's object is reused with it */
    for (uint32_t i = 0; i < 3; i++) rt_w32(G_MEM, b->obj + 8 * i + 4, (uint32_t)k);
    rt_w32(G_MEM, b->obj, vt_buf); rt_w32(G_MEM, b->obj + 8, vt_3d); rt_w32(G_MEM, b->obj + 16, vt_fx);
    pthread_mutex_unlock(&lock);
    *out = b; return b->obj;
}
static void d_CreateSoundBuffer(Ctx *c)
{
    uint32_t desc = ARG(1), out = ARG(2), fl = rt_r32(G_MEM, desc + 4), bytes = rt_r32(G_MEM, desc + 8), wfx = rt_r32(G_MEM, desc + 16);
    Buf *b; uint32_t obj = new_buf(&b); if (!obj) RET(DSERR_GENERIC, 4);
    b->flags = fl; b->primary = (fl & DSBCAPS_PRIMARYBUFFER) != 0; b->is3d = (fl & 0x10) != 0;   /* DSBCAPS_CTRL3D */
    if (b->primary) rt_w32(G_MEM, obj + 8, vt_lis);
    b->ch = 2; b->bits = 16; b->rate = 22050; b->align = 4;
    if (wfx) set_format(b, wfx);
    if (!b->primary) {
        if (!wfx || bytes < 4) { b->used = 0; RET(DSERR_INVALIDPARAM, 4); }
        b->size = bytes / b->align * b->align; b->mem = heap_alloc(w32_process_heap, 8, b->size);
        memset(GP(b->mem), b->bits == 8 ? 0x80 : 0, b->size);
    }
    LOG("CreateSoundBuffer flags %#x, %u bytes, %d Hz %d bits %d ch%s\n", fl, bytes, b->rate, b->bits, b->ch, b->primary ? " (primary)" : "");
    rt_w32(G_MEM, out, obj); RET(DS_OK, 4);
}
static void d_GetCaps(Ctx *c)
{
    uint32_t p = ARG(1); uint32_t sz = rt_r32(G_MEM, p); memset(GP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    rt_w32(G_MEM, p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800);  /* primary/secondary mono/stereo 8/16 bit, continuous rate, certified */
    rt_w32(G_MEM, p + 8, 100); rt_w32(G_MEM, p + 12, 200000); rt_w32(G_MEM, p + 16, 1);
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(Ctx *c)             /* (original, duplicate *): the same sound data, its own cursor and settings */
{
    Buf *o = B(ARG(1)); if (!o || o->primary) RET(DSERR_INVALIDPARAM, 3);
    Buf *b; uint32_t obj = new_buf(&b); if (!obj) RET(DSERR_GENERIC, 3);
    b->flags = o->flags; b->is3d = o->is3d; b->ch = o->ch; b->bits = o->bits; b->rate = o->rate; b->align = o->align; b->size = o->size; b->mem = o->mem;
    b->vol = o->vol; b->pan = o->pan; b->freq = o->freq; set_gain(b);
    rt_w32(G_MEM, ARG(2), obj); RET(DS_OK, 3);
}
static void d_SetCooperativeLevel(Ctx *c) { RET(DS_OK, 3); }
static void d_Compact(Ctx *c) { RET(DS_OK, 1); }
static void d_GetSpeakerConfig(Ctx *c) { rt_w32(G_MEM, ARG(1), 4 /* DSSPEAKER_STEREO */); RET(DS_OK, 2); }
static void d_SetSpeakerConfig(Ctx *c) { RET(DS_OK, 2); }
static void d_Initialize(Ctx *c) { RET(DS_OK, 2); }

/* ---- IDirectSoundBuffer8 additions ---- */
static void b_SetFX(Ctx *c)                             /* (count, DSEFFECTDESC *, results *): accepted, not applied */
{
    uint32_t n = ARG(1), res = ARG(3);
    for (uint32_t k = 0; res && k < n; k++) rt_w32(G_MEM, res + 4 * k, 2 /* DSFXR_LOCSOFTWARE */);
    RET(DS_OK, 4);
}
static void b_AcquireResources(Ctx *c) { uint32_t n = ARG(2), res = ARG(3); for (uint32_t k = 0; res && k < n; k++) rt_w32(G_MEM, res + 4 * k, 2); RET(DS_OK, 4); }
static void b_GetObjectInPath(Ctx *c)                   /* (object guid, index, interface iid, out): the effect's interface */
{
    Buf *b = B(ARG(0)); if (!b || !ARG(4)) RET(DSERR_INVALIDPARAM, 5);
    rt_w32(G_MEM, ARG(4), b->obj + 16); b->refs++; RET(DS_OK, 5);
}
/* ---- IDirectSound3DBuffer ---- */
static void t_GetAllParameters(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1); if (!b) RET(DSERR_INVALIDPARAM, 2);
    memcpy(GP(p + 4), b->pos, 12); memcpy(GP(p + 16), b->vel, 12); memcpy(GP(p + 28), b->cone, 8); memcpy(GP(p + 36), b->coneor, 12);
    rt_w32(G_MEM, p + 48, (uint32_t)b->conevol); memcpy(GP(p + 52), &b->mind, 4); memcpy(GP(p + 56), &b->maxd, 4); rt_w32(G_MEM, p + 60, (uint32_t)b->mode);
    RET(DS_OK, 2);
}
static void t_SetAllParameters(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1); if (!b) RET(DSERR_INVALIDPARAM, 3);
    memcpy(b->pos, GP(p + 4), 12); memcpy(b->vel, GP(p + 16), 12); memcpy(b->cone, GP(p + 28), 8); memcpy(b->coneor, GP(p + 36), 12);
    b->conevol = (int32_t)rt_r32(G_MEM, p + 48); memcpy(&b->mind, GP(p + 52), 4); memcpy(&b->maxd, GP(p + 56), 4); b->mode = (int)rt_r32(G_MEM, p + 60);
    RET(DS_OK, 3);
}
static void t_GetConeAngles(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 3); rt_w32(G_MEM, ARG(1), b->cone[0]); rt_w32(G_MEM, ARG(2), b->cone[1]); RET(DS_OK, 3); }
static void t_GetConeOrientation(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); memcpy(GP(ARG(1)), b->coneor, 12); RET(DS_OK, 2); }
static void t_GetConeOutsideVolume(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->conevol); RET(DS_OK, 2); }
static void t_GetMaxDistance(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); memcpy(GP(ARG(1)), &b->maxd, 4); RET(DS_OK, 2); }
static void t_GetMinDistance(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); memcpy(GP(ARG(1)), &b->mind, 4); RET(DS_OK, 2); }
static void t_GetMode(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->mode); RET(DS_OK, 2); }
static void t_GetPosition(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); memcpy(GP(ARG(1)), b->pos, 12); RET(DS_OK, 2); }
static void t_GetVelocity(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); memcpy(GP(ARG(1)), b->vel, 12); RET(DS_OK, 2); }
static void t_SetConeAngles(Ctx *c) { Buf *b = B(ARG(0)); if (b) { b->cone[0] = ARG(1); b->cone[1] = ARG(2); } RET(b ? DS_OK : DSERR_INVALIDPARAM, 4); }
static void t_SetConeOrientation(Ctx *c) { Buf *b = B(ARG(0)); if (b) { b->coneor[0] = F(ARG(1)); b->coneor[1] = F(ARG(2)); b->coneor[2] = F(ARG(3)); } RET(b ? DS_OK : DSERR_INVALIDPARAM, 5); }
static void t_SetConeOutsideVolume(Ctx *c) { Buf *b = B(ARG(0)); if (b) b->conevol = (int32_t)ARG(1); RET(b ? DS_OK : DSERR_INVALIDPARAM, 3); }
static void t_SetMaxDistance(Ctx *c) { Buf *b = B(ARG(0)); if (b) b->maxd = F(ARG(1)); RET(b ? DS_OK : DSERR_INVALIDPARAM, 3); }
static void t_SetMinDistance(Ctx *c) { Buf *b = B(ARG(0)); if (b) b->mind = F(ARG(1)); RET(b ? DS_OK : DSERR_INVALIDPARAM, 3); }
static void t_SetMode(Ctx *c) { Buf *b = B(ARG(0)); if (b) b->mode = (int)ARG(1); RET(b ? DS_OK : DSERR_INVALIDPARAM, 3); }
static void t_SetPosition(Ctx *c) { Buf *b = B(ARG(0)); if (b) { b->pos[0] = F(ARG(1)); b->pos[1] = F(ARG(2)); b->pos[2] = F(ARG(3)); } RET(b ? DS_OK : DSERR_INVALIDPARAM, 5); }
static void t_SetVelocity(Ctx *c) { Buf *b = B(ARG(0)); if (b) { b->vel[0] = F(ARG(1)); b->vel[1] = F(ARG(2)); b->vel[2] = F(ARG(3)); } RET(b ? DS_OK : DSERR_INVALIDPARAM, 5); }
/* ---- IDirectSound3DListener (one, on the primary buffer); deferred settings apply at once ---- */
static void l_GetAllParameters(Ctx *c)
{
    uint32_t p = ARG(1);
    memcpy(GP(p + 4), lis.pos, 12); memcpy(GP(p + 16), lis.vel, 12); memcpy(GP(p + 28), lis.front, 12); memcpy(GP(p + 40), lis.top, 12);
    memcpy(GP(p + 52), &lis.dist, 4); memcpy(GP(p + 56), &lis.rolloff, 4); memcpy(GP(p + 60), &lis.doppler, 4); RET(DS_OK, 2);
}
static void l_SetAllParameters(Ctx *c)
{
    uint32_t p = ARG(1);
    memcpy(lis.pos, GP(p + 4), 12); memcpy(lis.vel, GP(p + 16), 12); memcpy(lis.front, GP(p + 28), 12); memcpy(lis.top, GP(p + 40), 12);
    memcpy(&lis.dist, GP(p + 52), 4); memcpy(&lis.rolloff, GP(p + 56), 4); memcpy(&lis.doppler, GP(p + 60), 4); RET(DS_OK, 3);
}
static void l_GetDistanceFactor(Ctx *c) { memcpy(GP(ARG(1)), &lis.dist, 4); RET(DS_OK, 2); }
static void l_GetDopplerFactor(Ctx *c) { memcpy(GP(ARG(1)), &lis.doppler, 4); RET(DS_OK, 2); }
static void l_GetOrientation(Ctx *c) { memcpy(GP(ARG(1)), lis.front, 12); memcpy(GP(ARG(2)), lis.top, 12); RET(DS_OK, 3); }
static void l_GetPosition(Ctx *c) { memcpy(GP(ARG(1)), lis.pos, 12); RET(DS_OK, 2); }
static void l_GetRolloffFactor(Ctx *c) { memcpy(GP(ARG(1)), &lis.rolloff, 4); RET(DS_OK, 2); }
static void l_GetVelocity(Ctx *c) { memcpy(GP(ARG(1)), lis.vel, 12); RET(DS_OK, 2); }
static void l_SetDistanceFactor(Ctx *c) { lis.dist = F(ARG(1)); RET(DS_OK, 3); }
static void l_SetDopplerFactor(Ctx *c) { lis.doppler = F(ARG(1)); RET(DS_OK, 3); }
static void l_SetOrientation(Ctx *c) { for (int k = 0; k < 3; k++) { lis.front[k] = F(ARG(1 + k)); lis.top[k] = F(ARG(4 + k)); } RET(DS_OK, 8); }
static void l_SetPosition(Ctx *c) { for (int k = 0; k < 3; k++) lis.pos[k] = F(ARG(1 + k)); RET(DS_OK, 5); }
static void l_SetRolloffFactor(Ctx *c) { lis.rolloff = F(ARG(1)); RET(DS_OK, 3); }
static void l_SetVelocity(Ctx *c) { for (int k = 0; k < 3; k++) lis.vel[k] = F(ARG(1 + k)); RET(DS_OK, 5); }
static void l_CommitDeferredSettings(Ctx *c) { RET(DS_OK, 1); }
/* ---- IDirectSoundFXI3DL2Reverb: parameters kept, no reverb rendered ---- */
static uint8_t reverb_params[48];
static void x_SetAllParameters(Ctx *c) { memcpy(reverb_params, GP(ARG(1)), 48); RET(DS_OK, 2); }
static void x_GetAllParameters(Ctx *c) { memcpy(GP(ARG(1)), reverb_params, 48); RET(DS_OK, 2); }
static void x_SetPreset(Ctx *c) { RET(DS_OK, 2); }
static void x_GetPreset(Ctx *c) { rt_w32(G_MEM, ARG(1), 0); RET(DS_OK, 2); }
static void x_SetQuality(Ctx *c) { RET(DS_OK, 2); }
static void x_GetQuality(Ctx *c) { rt_w32(G_MEM, ARG(1), 0); RET(DS_OK, 2); }

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "dsound!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
IMPL(dsound, DirectSoundCreate)
{
    if (!vt_ds) {
        static void (*const ds[])(Ctx *) = { d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps, d_DuplicateSoundBuffer,
            d_SetCooperativeLevel, d_Compact, d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize, d_VerifyCertification };
        static void (*const bf[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume,
            b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
            b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore, b_SetFX, b_AcquireResources, b_GetObjectInPath };
        static void (*const td[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, t_GetAllParameters, t_GetConeAngles, t_GetConeOrientation,
            t_GetConeOutsideVolume, t_GetMaxDistance, t_GetMinDistance, t_GetMode, t_GetPosition, t_GetVelocity, t_SetAllParameters,
            t_SetConeAngles, t_SetConeOrientation, t_SetConeOutsideVolume, t_SetMaxDistance, t_SetMinDistance, t_SetMode, t_SetPosition, t_SetVelocity };
        static void (*const ls[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, l_GetAllParameters, l_GetDistanceFactor, l_GetDopplerFactor,
            l_GetOrientation, l_GetPosition, l_GetRolloffFactor, l_GetVelocity, l_SetAllParameters, l_SetDistanceFactor, l_SetDopplerFactor,
            l_SetOrientation, l_SetPosition, l_SetRolloffFactor, l_SetVelocity, l_CommitDeferredSettings };
        static void (*const fx[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, x_SetAllParameters, x_GetAllParameters, x_SetPreset, x_GetPreset,
            x_SetQuality, x_GetQuality };
        _Static_assert(sizeof ds / sizeof *ds == 12 && sizeof bf / sizeof *bf == 24 && sizeof td / sizeof *td == 21 && sizeof ls / sizeof *ls == 18 &&
                       sizeof fx / sizeof *fx == 9, "vtable sizes");
        vt_ds = vtable("IDirectSound8", ds, 12); vt_buf = vtable("IDirectSoundBuffer8", bf, 24); vt_3d = vtable("IDirectSound3DBuffer", td, 21);
        vt_lis = vtable("IDirectSound3DListener", ls, 18); vt_fx = vtable("IDirectSoundFXI3DL2Reverb", fx, 9);
        ds_obj = heap_alloc(w32_process_heap, 8, 8); rt_w32(G_MEM, ds_obj, vt_ds);
    }
    { const char *m = getenv("W32_DSOUND"); if (m && !strcmp(m, "0")) RET(DSERR_GENERIC, 3); }   /* W32_DSOUND=0: Miles uses waveOut */
    ds_refs++; rt_w32(G_MEM, ARG(1), ds_obj); LOG("DirectSoundCreate\n");
    RET(DS_OK, 3);
}
IMPL(dsound, DirectSoundEnumerateA)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    uint32_t s = heap_alloc(w32_process_heap, 8, 64); strcpy((char *)GP(s), "Primary Sound Driver"); strcpy((char *)GP(s + 32), "");
    uint32_t a[4] = {0, s, s + 32, ctx}; w32_callback(c, cb, 4, a);
    RET(DS_OK, 2);
}
