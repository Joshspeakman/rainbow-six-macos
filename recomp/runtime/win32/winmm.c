/* WINMM wave output on Core Audio (what the recompiled Miles Sound System mixes into), plus "no device" answers for
 * MIDI, mixer, aux, MCI and wave input. Buffers (WAVEHDR) play in submission order; finished buffers are marked
 * WHDR_DONE and reported through the client's chosen callback (function: on a guest service thread; event; window or
 * thread message). */
#include "w32.h"
#include "host_audio.h"

#include <unistd.h>
#include <math.h>

enum { WHDR_DONE = 1, WHDR_PREPARED = 2, WHDR_INQUEUE = 0x10, WOM_OPEN = 0x3bb, WOM_CLOSE = 0x3bc, WOM_DONE = 0x3bd };
enum { CB_NULL = 0, CB_WINDOW = 0x10000, CB_THREAD = 0x20000, CB_FUNCTION = 0x30000, CB_EVENT = 0x50000 };
typedef struct WaveOut {
    int used; uint32_t handle, cb, cbtype, inst; int ch, bits, rate;
    DsAudio * au; pthread_mutex_t lock;
    uint32_t q[256]; int qh, qn; uint32_t off;          /* queued WAVEHDRs (guest addresses), read offset in the head */
    float vol;
} WaveOut;
static WaveOut wos[8];
/* completion notifications are delivered by a guest service thread */
typedef struct Note { uint32_t wo, msg, hdr; } Note;
static Note notes[1024]; static int nnotes; static pthread_mutex_t nlock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t ncv = PTHREAD_COND_INITIALIZER;
static int svc;
void w32_post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t w32_window_tid(uint32_t hwnd);
int w32_event_set(uint32_t h);
static void deliver(Ctx *c, Note n)
{
    WaveOut *w = &wos[n.wo];
    switch (w->cbtype) {
    case CB_FUNCTION: { uint32_t a[5] = {w->handle, n.msg, w->inst, n.hdr, 0}; w32_callback(c, w->cb, 5, a); break; }
    case CB_EVENT: w32_event_set(w->cb); break;
    case CB_WINDOW: w32_post(w32_window_tid(w->cb), w->cb, n.msg, w->handle, n.hdr); break;
    case CB_THREAD: w32_post(w->cb, 0, n.msg, w->handle, n.hdr); break;
    }
}
static void service(Ctx *c, void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&nlock);
        while (!nnotes) pthread_cond_wait(&ncv, &nlock);
        Note q[1024]; int n = nnotes; memcpy(q, notes, sizeof(Note) * (size_t)n); nnotes = 0;
        pthread_mutex_unlock(&nlock);
        for (int k = 0; k < n; k++) deliver(c, q[k]);
    }
}
static void note(uint32_t wo, uint32_t msg, uint32_t hdr)
{
    if (wos[wo].cbtype == CB_NULL) return;
    pthread_mutex_lock(&nlock);
    if (nnotes < 1024) { notes[nnotes].wo = wo; notes[nnotes].msg = msg; notes[nnotes].hdr = hdr; nnotes++; }
    pthread_cond_signal(&ncv); pthread_mutex_unlock(&nlock);
}
/* every output path ends here: W32_AUDIOLOG prints the level once per second, W32_AUDIO_DUMP=<file.wav> records the
 * output (16-bit stereo); the caller then plays silence when W32_AUDIO_MUTE is set (test runs) */
void w32_audio_out(const float *lr, uint32_t frames, int rate, int source)
{
    static double acc[2]; static float peak[2]; static FILE *dump; static int dump_init, dump_rate;
    for (uint32_t i = 0; i < 2 * frames; i++) { float v = fabsf(lr[i]); if (v > peak[source]) peak[source] = v; }
    acc[source] += (double)frames / rate;
    if (acc[source] >= 1.0) {
        if (getenv("W32_AUDIOLOG")) fprintf(stderr, "audio: %s peak %.3f\n", source ? "DirectSound" : "waveOut", peak[source]);
        acc[source] = 0; peak[source] = 0;
    }
    if (!dump_init) { dump_init = 1; const char *p = getenv("W32_AUDIO_DUMP"); if (p && (dump = fopen(p, "wb"))) { uint8_t h[44] = {0}; fwrite(h, 1, 44, dump); dump_rate = rate; } }
    if (!dump || rate != dump_rate) return;
    int16_t s[2 * 1024];
    for (uint32_t at = 0; at < frames; at += 1024) {
        uint32_t n = frames - at < 1024 ? frames - at : 1024;
        for (uint32_t i = 0; i < 2 * n; i++) { float v = lr[2 * at + i] * 32768.0f; s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v)); }
        fwrite(s, 4, n, dump);
    }
    long n = ftell(dump) - 44; uint32_t v, br = (uint32_t)rate * 4; uint16_t h16; uint8_t h[44];
    memcpy(h, "RIFF", 4); v = (uint32_t)n + 36; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    h16 = 1; memcpy(h + 20, &h16, 2); h16 = 2; memcpy(h + 22, &h16, 2); v = (uint32_t)rate; memcpy(h + 24, &v, 4); memcpy(h + 28, &br, 4);
    h16 = 4; memcpy(h + 32, &h16, 2); h16 = 16; memcpy(h + 34, &h16, 2); memcpy(h + 36, "data", 4); v = (uint32_t)n; memcpy(h + 40, &v, 4);
    fseek(dump, 0, SEEK_SET); fwrite(h, 1, 44, dump); fseek(dump, 0, SEEK_END);
}
static void render(void *u, void *buffer, uint32_t frames)
{

    uint32_t k = (uint32_t)(uintptr_t)u; WaveOut *w = &wos[k];
    uint8_t *out = buffer; uint32_t want = frames * (uint32_t)(w->ch * w->bits / 8), got = 0;
    uint32_t done[64]; int nd = 0;
    pthread_mutex_lock(&w->lock);
    while (got < want && w->qn) {
        uint32_t h = w->q[w->qh], data = rt_r32(G_MEM, h), len = rt_r32(G_MEM, h + 4);
        uint32_t n = len - w->off < want - got ? len - w->off : want - got;
        memcpy(out + got, G_MEM + data + w->off, n); got += n; w->off += n;
        if (w->off >= len) {
            rt_w32(G_MEM, h + 16, (rt_r32(G_MEM, h + 16) & ~(uint32_t)WHDR_INQUEUE) | WHDR_DONE);
            w->qh = (w->qh + 1) % 256; w->qn--; w->off = 0;
            if (nd < 64) done[nd++] = h;
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (got < want) memset(out + got, w->bits == 8 ? 0x80 : 0, want - got);
    if (k == 0 && w->ch == 2 && w->bits == 16) {                /* the shared output tap (level log, dump) */
        float tmp[2 * 1024];
        for (uint32_t at = 0; at < frames; at += 1024) {
            uint32_t n = frames - at < 1024 ? frames - at : 1024;
            for (uint32_t i = 0; i < 2 * n; i++) tmp[i] = ((int16_t *)out)[2 * at + i] / 32768.0f;
            w32_audio_out(tmp, n, w->rate, 0);
        }
    }
    if (getenv("W32_AUDIO_MUTE")) memset(out, w->bits == 8 ? 0x80 : 0, want);
    for (int j = 0; j < nd; j++) note(k, WOM_DONE, done[j]);

}
IMPL(winmm, waveOutGetDevCapsA)
{
    uint32_t p = ARG(1), sz = ARG(2); uint8_t caps[52] = {0};
    caps[0] = 1; caps[2] = 1; caps[4] = 1; snprintf((char *)caps + 8, 32, "Native Audio Output");
    uint32_t fmts = 0xfff; memcpy(caps + 40, &fmts, 4); caps[44] = 2; uint32_t sup = 0x4 | 0x8; memcpy(caps + 48, &sup, 4);
    memcpy(GP(p), caps, sz < 52 ? sz : 52);
    RET(0, 3);
}
IMPL(winmm, waveOutOpen)
{
    uint32_t ph = ARG(0), fmt = ARG(2), cb = ARG(3), inst = ARG(4), fl = ARG(5);
    uint16_t tag = (uint16_t)rt_r16(G_MEM, fmt), ch = (uint16_t)rt_r16(G_MEM, fmt + 2), bits = (uint16_t)rt_r16(G_MEM, fmt + 14);
    uint32_t rate = rt_r32(G_MEM, fmt + 4);
    if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2) RET(32, 6);   /* WAVERR_BADFORMAT */
    if (fl & 1) RET(0, 6);                                                        /* WAVE_FORMAT_QUERY */
    int k = 0; while (k < 8 && wos[k].used) k++;
    if (k == 8) RET(4, 6);                                                        /* MMSYSERR_ALLOCATED */
    WaveOut *w = &wos[k]; memset(w, 0, sizeof *w);
    w->used = 1; w->handle = 0x00c00000u + 4 * (uint32_t)k; w->cb = cb; w->cbtype = fl & 0x70000; w->inst = inst;
    w->ch = ch; w->bits = bits; w->rate = (int)rate; pthread_mutex_init(&w->lock, NULL); w->vol = 1;
    w->au = ds_audio_open((int)rate, ch, bits, 0, render, (void *)(uintptr_t)k);
    if (!w->au) { pthread_mutex_destroy(&w->lock); w->used = 0; RET(2, 6); }
    if (!svc) { svc = 1; extern int w32_spawn_service(void (*)(Ctx *, void *), void *); w32_spawn_service(service, 0); }
    rt_w32(G_MEM, ph, w->handle);
    if (getenv("W32_AUDIOLOG")) fprintf(stderr, "audio: waveOutOpen %u Hz, %u bits, %u ch, callback type %x\n", rate, bits, ch, w->cbtype);
    note((uint32_t)k, WOM_OPEN, 0);
    RET(0, 6);
}
static WaveOut *WO(uint32_t h) { uint32_t k = (h - 0x00c00000u) / 4; return h >= 0x00c00000u && k < 8 && wos[k].used ? &wos[k] : 0; }
IMPL(winmm, waveOutPrepareHeader) { rt_w32(G_MEM, ARG(1) + 16, rt_r32(G_MEM, ARG(1) + 16) | WHDR_PREPARED); RET(0, 3); }
IMPL(winmm, waveOutUnprepareHeader)
{
    uint32_t h = ARG(1), f = rt_r32(G_MEM, h + 16);
    if (f & WHDR_INQUEUE) RET(33, 3);                                             /* WAVERR_STILLPLAYING */
    rt_w32(G_MEM, h + 16, f & ~(uint32_t)WHDR_PREPARED); RET(0, 3);
}
IMPL(winmm, waveOutWrite)
{
    WaveOut *w = WO(ARG(0)); uint32_t h = ARG(1); if (!w) RET(5, 3);
    pthread_mutex_lock(&w->lock);
    if (w->qn < 256) { rt_w32(G_MEM, h + 16, (rt_r32(G_MEM, h + 16) & ~(uint32_t)WHDR_DONE) | WHDR_INQUEUE); w->q[(w->qh + w->qn) % 256] = h; w->qn++; }
    pthread_mutex_unlock(&w->lock);
    RET(0, 3);
}
IMPL(winmm, waveOutReset)
{
    WaveOut *w = WO(ARG(0)); if (!w) RET(5, 1);
    uint32_t done[256]; int nd = 0;
    pthread_mutex_lock(&w->lock);
    while (w->qn) { uint32_t h = w->q[w->qh]; rt_w32(G_MEM, h + 16, (rt_r32(G_MEM, h + 16) & ~(uint32_t)WHDR_INQUEUE) | WHDR_DONE); done[nd++] = h; w->qh = (w->qh + 1) % 256; w->qn--; }
    w->off = 0;
    pthread_mutex_unlock(&w->lock);
    for (int j = 0; j < nd; j++) note((uint32_t)(w - wos), WOM_DONE, done[j]);
    RET(0, 1);
}
IMPL(winmm, waveOutClose)
{
    WaveOut *w = WO(ARG(0)); if (!w) RET(5, 1);
    ds_audio_close(w->au);
    pthread_mutex_destroy(&w->lock);
    note((uint32_t)(w - wos), WOM_CLOSE, 0); w->used = 0;
    RET(0, 1);
}
IMPL(winmm, waveOutGetID) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(0, 2); }
/* no MIDI, mixer, aux, MCI or recording devices */
IMPL(winmm, auxGetNumDevs) { RET(0, 0); }
IMPL(winmm, auxGetDevCapsA) { RET(2, 3); }
IMPL(winmm, auxGetVolume) { RET(2, 2); }
IMPL(winmm, auxSetVolume) { RET(2, 2); }
IMPL(winmm, mciSendCommandA) { RET(257, 4); }
IMPL(winmm, midiOutOpen) { RET(6, 5); }
IMPL(winmm, midiOutClose) { RET(5, 1); }
IMPL(winmm, midiOutLongMsg) { RET(5, 3); }
IMPL(winmm, midiOutShortMsg) { RET(5, 2); }
IMPL(winmm, midiOutPrepareHeader) { RET(5, 3); }
IMPL(winmm, midiOutUnprepareHeader) { RET(5, 3); }
IMPL(winmm, midiOutReset) { RET(5, 1); }
IMPL(winmm, mixerGetNumDevs) { RET(0, 0); }
IMPL(winmm, mixerOpen) { RET(2, 5); }
IMPL(winmm, mixerClose) { RET(5, 1); }
IMPL(winmm, mixerGetLineInfoA) { RET(5, 3); }
IMPL(winmm, mixerGetLineControlsA) { RET(5, 3); }
IMPL(winmm, mixerGetControlDetailsA) { RET(5, 3); }
IMPL(winmm, mixerSetControlDetails) { RET(5, 3); }
IMPL(winmm, waveInOpen) { RET(2, 6); }
IMPL(winmm, waveInClose) { RET(5, 1); }
IMPL(winmm, waveInAddBuffer) { RET(5, 3); }
IMPL(winmm, waveInPrepareHeader) { RET(5, 3); }
IMPL(winmm, waveInUnprepareHeader) { RET(5, 3); }
IMPL(winmm, waveInReset) { RET(5, 1); }
IMPL(winmm, waveInStart) { RET(5, 1); }
IMPL(winmm, timeEndPeriod) { RET(0, 1); }
IMPL(winmm, timeBeginPeriod) { RET(0, 1); }
