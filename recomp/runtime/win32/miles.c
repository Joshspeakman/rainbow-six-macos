/* Miles Sound System 6.1c on Core Audio: 2D samples, 3D samples (distance attenuation + stereo panning, like Miles'
 * fast 2D-positional provider), streams (read through the game's file callbacks, decoded on a worker thread), timers.
 * Sound data (WAV PCM/ADPCM, MP3) is decoded with AudioToolbox from memory. The game's callbacks (timers, end of
 * sample) run on a guest service thread, as Miles calls them from its own thread on Windows.
 * Handles are small guest blocks; volumes are 0..127 (Miles 6). */
#include "w32.h"
#include "host_audio.h"
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#else
#include <sndfile.h>
#endif

#include <math.h>
#include <unistd.h>

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { V_2D = 1, V_3D, V_STREAM };
#define OUT_RATE 44100.0
static int alog = -1;
#define ALOG(...) do { if (alog < 0) alog = getenv("W32_AUDIOLOG") != 0; if (alog) fprintf(stderr, "audio: " __VA_ARGS__); } while (0)

typedef struct Pcm { float *data; uint32_t frames, cap; int ch; double rate; uint32_t src_bytes; volatile int done; int refs; } Pcm;
typedef struct Voice {
    int kind, used; uint32_t handle;
    Pcm *pcm; double pos; int status, loops, volume, rate_hz;
    uint32_t eos_cb;
    float x, y, z, min_d, max_d;
} Voice;
#define MAXV 512
static Voice voices[MAXV];
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;        /* voices + listener, shared with the render callback */
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static int master_vol = 127, started;
static float lx, ly, lz, lfx = 0, lfy = 0, lfz = 1, lux = 0, luy = 1, luz = 0;
static uint32_t listener_h, provider_h = 0x4d334450u;      /* "M3DP" */
static uint32_t file_open, file_close, file_seek, file_read;
/* Miles 6 defaults: DIG_RESAMPLING_TOLERANCE, DIG_MIXER_CHANNELS, DIG_DEFAULT_VOLUME, MDI_SERVICE_RATE, MDI_SEQUENCES,
 * MDI_DEFAULT_VOLUME, MDI_QUANT_ADVANCE, MDI_ALLOW_LOOP_CONTROL, MDI_DEFAULT_BEND_RANGE */
static uint32_t prefs[64] = {131, 64, 127, 120, 8, 127, 1, 0, 2};
static DsAudio * out_unit;

static uint32_t new_handle(int kind)
{
    for (int k = 0; k < MAXV; k++) if (!voices[k].used) {
        Voice *v = &voices[k]; memset(v, 0, sizeof *v);
        v->used = 1; v->kind = kind; v->status = SMP_DONE; v->volume = 127; v->loops = 1; v->min_d = 1; v->max_d = 100;
        v->handle = heap_alloc(w32_process_heap, 8, 64);
        rt_w32(G_MEM, v->handle, 0x4d494c53u); rt_w32(G_MEM, v->handle + 4, (uint32_t)k);   /* "MILS", index */
        return v->handle;
    }
    return 0;
}
static Voice *V(uint32_t h)
{
    if (!h || rt_r32(G_MEM, h) != 0x4d494c53u) return 0;
    uint32_t k = rt_r32(G_MEM, h + 4); return k < MAXV && voices[k].used && voices[k].handle == h ? &voices[k] : 0;
}
static void pcm_release(Pcm *p) { if (p && __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0) { while (!p->done) usleep(1000); free(p->data); free(p); } }

/* ---------------------------------------------------------------- decoding (AudioToolbox, from memory) */
typedef struct Mem { const uint8_t *p; uint32_t n; } Mem;
#ifdef __APPLE__
static OSStatus mem_read(void *u, SInt64 pos, UInt32 req, void *buf, UInt32 *got)
{ Mem *m = u; if (pos >= m->n) { *got = 0; return 0; } UInt32 n = (UInt32)(m->n - pos) < req ? (UInt32)(m->n - pos) : req; memcpy(buf, m->p + pos, n); *got = n; return 0; }
static SInt64 mem_size(void *u) { return ((Mem *)u)->n; }
#endif
static uint32_t data_size(const uint8_t *d, uint32_t limit)           /* size of a WAV image; MP3: limit */
{
    if (!memcmp(d, "RIFF", 4)) { uint32_t n; memcpy(&n, d + 4, 4); return n + 8; }
    return limit;
}
typedef struct DecJob { Pcm *p; uint8_t *bytes; uint32_t n; } DecJob;
#ifdef __APPLE__
static void decode_into(Pcm *p, const uint8_t *bytes, uint32_t n, int incremental)
{
    Mem m = {bytes, n}; AudioFileID af = 0; ExtAudioFileRef ef = 0;
    AudioFileTypeID hint = !memcmp(bytes, "RIFF", 4) ? kAudioFileWAVEType : kAudioFileMP3Type;
    if (AudioFileOpenWithCallbacks(&m, mem_read, 0, mem_size, 0, hint, &af) || ExtAudioFileWrapAudioFileID(af, false, &ef)) {
        ALOG("cannot decode %u bytes (%.4s)\n", n, bytes); if (af) AudioFileClose(af); p->done = 1; return;
    }
    AudioStreamBasicDescription src; UInt32 sz = sizeof src;
    ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileDataFormat, &sz, &src);
    int ch = src.mChannelsPerFrame > 1 ? 2 : 1;
    AudioStreamBasicDescription dst = {0};
    dst.mSampleRate = src.mSampleRate; dst.mFormatID = kAudioFormatLinearPCM; dst.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    dst.mChannelsPerFrame = (UInt32)ch; dst.mBitsPerChannel = 32; dst.mBytesPerFrame = dst.mBytesPerPacket = 4u * (UInt32)ch; dst.mFramesPerPacket = 1;
    ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat, sizeof dst, &dst);
    SInt64 total = 0; sz = sizeof total; ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileLengthFrames, &sz, &total);
    uint32_t cap = total > 0 ? (uint32_t)total + 4096 : 1u << 20;
    float *data = malloc((size_t)cap * (size_t)ch * 4);
    pthread_mutex_lock(&mix_lock); p->data = data; p->cap = cap; p->ch = ch; p->rate = src.mSampleRate; p->frames = 0; pthread_mutex_unlock(&mix_lock);
    for (;;) {
        if (p->frames + 4096 > p->cap) {                        /* grow (the render callback reads under mix_lock) */
            uint32_t nc = p->cap * 2; float *nd = malloc((size_t)nc * (size_t)ch * 4);
            memcpy(nd, p->data, (size_t)p->frames * (size_t)ch * 4);
            pthread_mutex_lock(&mix_lock); float *od = p->data; p->data = nd; p->cap = nc; pthread_mutex_unlock(&mix_lock); free(od);
        }
        AudioBufferList bl; bl.mNumberBuffers = 1; bl.mBuffers[0].mNumberChannels = (UInt32)ch;
        UInt32 frames = 4096; bl.mBuffers[0].mDataByteSize = frames * 4u * (UInt32)ch; bl.mBuffers[0].mData = p->data + (size_t)p->frames * (size_t)ch;
        if (ExtAudioFileRead(ef, &frames, &bl) || !frames) break;
        __atomic_store_n(&p->frames, p->frames + frames, __ATOMIC_RELEASE);
        (void)incremental;
    }
    ExtAudioFileDispose(ef); AudioFileClose(af);
    __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE);
}
#else
typedef struct { const uint8_t *p; sf_count_t size, at; } SndMemory;
static sf_count_t snd_size(void *u) { return ((SndMemory *)u)->size; }
static sf_count_t snd_seek(sf_count_t off, int whence, void *u) {
    SndMemory *m = u; sf_count_t at = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m->at : m->size) + off;
    if (at < 0 || at > m->size) return -1; return m->at = at;
}
static sf_count_t snd_read(void *out, sf_count_t n, void *u) {
    SndMemory *m = u; if (n > m->size - m->at) n = m->size - m->at;
    memcpy(out, m->p + m->at, (size_t)n); m->at += n; return n;
}
static sf_count_t snd_tell(void *u) { return ((SndMemory *)u)->at; }
static void decode_into(Pcm *p, const uint8_t *bytes, uint32_t n, int incremental) {
    (void)incremental; SndMemory m = {bytes, n, 0}; SF_INFO info = {0};
    SF_VIRTUAL_IO io = {snd_size, snd_seek, snd_read, NULL, snd_tell};
    SNDFILE *sf = sf_open_virtual(&io, SFM_READ, &info, &m);
    if (!sf || info.channels < 1 || info.channels > 2 || info.frames <= 0 || info.frames > UINT32_MAX) {
        if (sf) sf_close(sf); __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE); return;
    }
    float *samples = malloc((size_t)info.frames * info.channels * sizeof(float));
    if (!samples) { sf_close(sf); __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE); return; }
    sf_count_t frames = sf_readf_float(sf, samples, info.frames); sf_close(sf);
    pthread_mutex_lock(&mix_lock); p->data = samples; p->cap = info.frames; p->ch = info.channels; p->rate = info.samplerate;
    __atomic_store_n(&p->frames, (uint32_t)frames, __ATOMIC_RELEASE); pthread_mutex_unlock(&mix_lock);
    __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE);
}
#endif

static void *decode_thread(void *a) { DecJob *j = a; decode_into(j->p, j->bytes, j->n, 1); free(j->bytes); pcm_release(j->p); free(j); return 0; }
/* decoded sounds are cached (the game re-submits the same in-memory WAV for every play): key = address, size and a
 * hash of the data, so a reused buffer with new contents decodes again */
typedef struct CacheEnt { uint32_t addr, size; uint64_t hash; Pcm *p; uint64_t used; } CacheEnt;
static CacheEnt cache[256]; static uint64_t cache_tick;
static uint64_t hash_bytes(const uint8_t *d, uint32_t n)
{
    uint64_t h = 1469598103934665603ull; uint32_t step = n > 65536 ? n / 16384 : 1;
    for (uint32_t k = 0; k < n; k += step) { h ^= d[k]; h *= 1099511628211ull; }
    return h ^ n;
}
static Pcm *decode_now(uint32_t image, uint32_t limit)
{
    const uint8_t *d = (const uint8_t *)GP(image); uint32_t n = data_size(d, limit);
    uint64_t h = hash_bytes(d, n); int slot = 0;
    for (int k = 0; k < 256; k++) {
        if (cache[k].p && cache[k].addr == image && cache[k].size == n && cache[k].hash == h) {
            cache[k].used = ++cache_tick; __atomic_add_fetch(&cache[k].p->refs, 1, __ATOMIC_ACQ_REL); return cache[k].p;
        }
        if (cache[k].used < cache[slot].used) slot = k;
    }
    Pcm *p = calloc(1, sizeof *p); p->refs = 1; p->src_bytes = n;
    decode_into(p, d, n, 0);
    if (!p->frames) ALOG("empty sound (%.4s, %u bytes)\n", d, n);
    if (n < (8u << 20)) {                                     /* keep small/medium sounds */
        if (cache[slot].p) pcm_release(cache[slot].p);
        cache[slot].addr = image; cache[slot].size = n; cache[slot].hash = h; cache[slot].p = p; cache[slot].used = ++cache_tick;
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}

/* ---------------------------------------------------------------- mixing */
static float gain_of(int vol) { float g = (vol < 0 ? 0 : vol > 127 ? 127 : vol) / 127.0f; return g * g; }   /* ~perceptual */
static void render(void *u, void *buffer, uint32_t n)
{
    (void)u;
    float *out = buffer; memset(out, 0, (size_t)n * 8);
    static uint32_t ended[MAXV]; int nend = 0;
    pthread_mutex_lock(&mix_lock);
    float master = gain_of(master_vol);
    for (int k = 0; k < MAXV; k++) {
        Voice *v = &voices[k]; Pcm *p = v->pcm;
        if (!v->used || v->status != SMP_PLAYING || !p || !p->data) continue;
        float g = master * gain_of(v->volume), gl = g, gr = g;
        if (v->kind == V_3D) {                                   /* distance attenuation + pan relative to the listener */
            float dx = v->x - lx, dy = v->y - ly, dz = v->z - lz, d = sqrtf(dx * dx + dy * dy + dz * dz);
            float att = d <= v->min_d ? 1.0f : d >= v->max_d ? 0.0f : v->min_d / d;
            float rx = luy * lfz - luz * lfy, ry = luz * lfx - lux * lfz, rz = lux * lfy - luy * lfx;   /* right = up x face */
            float rl = sqrtf(rx * rx + ry * ry + rz * rz), pan = (d > 1e-3f && rl > 1e-6f) ? (dx * rx + dy * ry + dz * rz) / (d * rl) : 0;
            gl = g * att * sqrtf(0.5f * (1 - pan)) * 1.41421f; gr = g * att * sqrtf(0.5f * (1 + pan)) * 1.41421f;
        }
        double step = (v->rate_hz > 0 ? v->rate_hz : p->rate) / OUT_RATE;
        uint32_t frames = __atomic_load_n(&p->frames, __ATOMIC_ACQUIRE); int done = __atomic_load_n(&p->done, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; i++) {
            if (v->pos >= frames) {
                if (!done) break;                                 /* stream not decoded this far yet */
                if (v->loops != 1) { if (v->loops > 1) v->loops--; v->pos = 0; if (!frames) break; }
                else { v->status = SMP_DONE; if (v->eos_cb && nend < MAXV) ended[nend++] = v->handle; break; }
            }
            uint32_t a = (uint32_t)v->pos; float t = (float)(v->pos - a); uint32_t b = a + 1 < frames ? a + 1 : a;
            float l, r;
            if (p->ch == 2) { l = p->data[2 * a] + (p->data[2 * b] - p->data[2 * a]) * t; r = p->data[2 * a + 1] + (p->data[2 * b + 1] - p->data[2 * a + 1]) * t; }
            else { l = r = p->data[a] + (p->data[b] - p->data[a]) * t; }
            out[2 * i] += l * gl; out[2 * i + 1] += r * gr;
            v->pos += step;
        }
    }
    pthread_mutex_unlock(&mix_lock);
    for (uint32_t i = 0; i < 2 * n; i++) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    {   /* W32_AUDIO_MUTE=1 (test runs): mix as usual, report the level once per second, output silence */
        static int mute = -1; if (mute < 0) mute = getenv("W32_AUDIO_MUTE") != 0;
        static double acc; static float peak; static int voices_on;
        for (uint32_t i = 0; i < 2 * n; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
        acc += n / OUT_RATE;
        if (acc >= 1.0) { int on = 0; for (int k = 0; k < MAXV; k++) on += voices[k].used && voices[k].status == SMP_PLAYING; voices_on = on;
                          ALOG("output peak %.3f, %d voices playing\n", peak, voices_on); acc = 0; peak = 0; }
        if (mute) memset(out, 0, (size_t)n * 8);
    }
    if (nend) { extern void miles_queue_eos(const uint32_t *, int); miles_queue_eos(ended, nend); }

}
static void start_output(void)
{
    if (started) return; started = 1;
    out_unit = ds_audio_open(OUT_RATE, 2, 32, 1, render, NULL);
    if (!out_unit) fprintf(stderr, "audio: cannot start output\n");
}

/* ---------------------------------------------------------------- callbacks on a guest service thread */
typedef struct Timer { int used, running; uint32_t fn, user, period_us; double next; } Timer;
static Timer timers[16];
static uint32_t eos_q[1024]; static int eos_n;
static pthread_mutex_t svc_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t svc_cv = PTHREAD_COND_INITIALIZER;
static int svc_started;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void miles_queue_eos(const uint32_t *h, int n)
{
    pthread_mutex_lock(&svc_lock);
    for (int k = 0; k < n && eos_n < 1024; k++) eos_q[eos_n++] = h[k];
    pthread_cond_signal(&svc_cv); pthread_mutex_unlock(&svc_lock);
}
static void service(Ctx *c, void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&svc_lock);
        double t = now_s(), next = t + 0.05;
        for (int k = 0; k < 16; k++) if (timers[k].used && timers[k].running && timers[k].next < next) next = timers[k].next;
        if (!eos_n && next > t) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); double w = next - t;
            ts.tv_sec += (time_t)w; ts.tv_nsec += (long)((w - (time_t)w) * 1e9); if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&svc_cv, &svc_lock, &ts);
        }
        uint32_t q[1024]; int nq = eos_n; memcpy(q, eos_q, (size_t)nq * 4); eos_n = 0;
        uint32_t due_fn[16], due_user[16]; int nd = 0; t = now_s();
        for (int k = 0; k < 16; k++) if (timers[k].used && timers[k].running && timers[k].period_us && timers[k].next <= t) {
            due_fn[nd] = timers[k].fn; due_user[nd++] = timers[k].user;
            timers[k].next += timers[k].period_us * 1e-6; if (timers[k].next < t) timers[k].next = t + timers[k].period_us * 1e-6;
        }
        pthread_mutex_unlock(&svc_lock);
        for (int k = 0; k < nd; k++) w32_callback(c, due_fn[k], 1, &due_user[k]);
        for (int k = 0; k < nq; k++) {
            pthread_mutex_lock(&api_lock); Voice *v = V(q[k]); uint32_t cb = v ? v->eos_cb : 0; pthread_mutex_unlock(&api_lock);
            if (cb) w32_callback(c, cb, 1, &q[k]);
        }
    }
}
static void ensure_service(void) { if (!svc_started) { svc_started = 1; extern int w32_spawn_service(void (*)(Ctx *, void *), void *); w32_spawn_service(service, 0); } }

/* ---------------------------------------------------------------- API */
#define LOCK pthread_mutex_lock(&api_lock)
#define UNLOCK pthread_mutex_unlock(&api_lock)
static uint32_t str_in_guest(const char *s) { static uint32_t g[4]; static int k; uint32_t a = g[k & 3]; if (!a) a = g[k & 3] = heap_alloc(w32_process_heap, 8, 256); k++; snprintf((char *)GP(a), 256, "%s", s); return a; }
IMPL(mss32, AIL_startup) { RET(1, 0); }
IMPL(mss32, AIL_shutdown) { if (out_unit) ds_audio_stop(out_unit); RET(0, 0); }
IMPL(mss32, AIL_set_redist_directory) { RET(str_in_guest(""), 1); }
IMPL(mss32, AIL_last_error) { RET(str_in_guest(""), 0); }
IMPL(mss32, AIL_set_preference) { uint32_t n = ARG(0) & 63, o = prefs[n]; prefs[n] = ARG(1); ALOG("set_preference %u = %d\n", ARG(0), (int)ARG(1)); RET(o, 2); }
IMPL(mss32, AIL_get_preference) { RET(prefs[ARG(0) & 63], 1); }
IMPL(mss32, AIL_set_file_callbacks) { file_open = ARG(0); file_close = ARG(1); file_seek = ARG(2); file_read = ARG(3); RET(0, 4); }
IMPL(mss32, AIL_open_digital_driver)
{
    ALOG("open_digital_driver %u Hz, %d bits, %d ch\n", ARG(0), (int)ARG(1), (int)ARG(2));
    start_output(); ensure_service();
    static uint32_t drv; if (!drv) { drv = heap_alloc(w32_process_heap, 8, 256); }
    RET(drv, 4);
}
IMPL(mss32, AIL_set_digital_master_volume) { master_vol = (int)ARG(1); RET(0, 2); }

/* 2D samples */
IMPL(mss32, AIL_allocate_sample_handle) { LOCK; uint32_t h = new_handle(V_2D); UNLOCK; RET(h, 1); }
static void voice_free(Voice *v)
{
    pthread_mutex_lock(&mix_lock); Pcm *p = v->pcm; v->pcm = 0; v->used = 0; pthread_mutex_unlock(&mix_lock);
    pcm_release(p); heap_free(w32_process_heap, v->handle);
}
IMPL(mss32, AIL_release_sample_handle) { LOCK; Voice *v = V(ARG(0)); if (v) voice_free(v); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_init_sample)
{
    LOCK; Voice *v = V(ARG(0));
    if (v) { pthread_mutex_lock(&mix_lock); Pcm *p = v->pcm; v->pcm = 0; v->status = SMP_DONE; v->pos = 0; v->loops = 1; v->volume = 127; v->rate_hz = 0; pthread_mutex_unlock(&mix_lock); pcm_release(p); }
    UNLOCK; RET(0, 1);
}
static int set_file(Voice *v, uint32_t image, uint32_t limit)
{
    Pcm *p = decode_now(image, limit);
    pthread_mutex_lock(&mix_lock); Pcm *o = v->pcm; v->pcm = p; v->pos = 0; v->status = SMP_DONE; v->rate_hz = 0; pthread_mutex_unlock(&mix_lock);
    pcm_release(o);
    return p->frames > 0;
}
IMPL(mss32, AIL_set_sample_file)
{
    LOCK; Voice *v = V(ARG(0)); int ok = v && ARG(1) ? set_file(v, ARG(1), ARG(2) && ARG(2) != 0xffffffffu ? ARG(2) : 0x1000000u) : 0; UNLOCK;
    RET(ok, 3);
}
static void set_status(Voice *v, int st) { pthread_mutex_lock(&mix_lock); v->status = st; pthread_mutex_unlock(&mix_lock); }
IMPL(mss32, AIL_start_sample) { LOCK; Voice *v = V(ARG(0)); if (v) { pthread_mutex_lock(&mix_lock); v->pos = 0; v->status = SMP_PLAYING; pthread_mutex_unlock(&mix_lock); } UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_stop_sample) { LOCK; Voice *v = V(ARG(0)); if (v && v->status == SMP_PLAYING) set_status(v, SMP_STOPPED); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_resume_sample) { LOCK; Voice *v = V(ARG(0)); if (v && v->status == SMP_STOPPED) set_status(v, SMP_PLAYING); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_end_sample) { LOCK; Voice *v = V(ARG(0)); if (v) set_status(v, SMP_DONE); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_sample_status) { LOCK; Voice *v = V(ARG(0)); uint32_t s = v ? (uint32_t)v->status : SMP_FREE; UNLOCK; RET(s, 1); }
IMPL(mss32, AIL_set_sample_volume) { Voice *v = V(ARG(0)); if (v) v->volume = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_set_sample_playback_rate) { Voice *v = V(ARG(0)); if (v) v->rate_hz = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_sample_playback_rate) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)(v->rate_hz ? v->rate_hz : (v->pcm ? (int)v->pcm->rate : 22050)) : 0, 1); }
IMPL(mss32, AIL_set_sample_loop_count) { Voice *v = V(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
static uint32_t bytes_per_frame(Voice *v) { return v->pcm && v->pcm->frames ? (v->pcm->src_bytes + v->pcm->frames - 1) / v->pcm->frames : 1; }
IMPL(mss32, AIL_set_sample_position) { Voice *v = V(ARG(0)); if (v) v->pos = ARG(1) / (double)bytes_per_frame(v); RET(0, 2); }
IMPL(mss32, AIL_sample_position) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)(v->pos * bytes_per_frame(v)) : 0, 1); }
IMPL(mss32, AIL_sample_ms_position)
{
    Voice *v = V(ARG(0)); uint32_t tot = 0, cur = 0;
    if (v && v->pcm && v->pcm->rate > 0) { tot = (uint32_t)(v->pcm->frames * 1000.0 / v->pcm->rate); cur = (uint32_t)(v->pos * 1000.0 / v->pcm->rate); }
    if (ARG(1)) rt_w32(G_MEM, ARG(1), tot);
    if (ARG(2)) rt_w32(G_MEM, ARG(2), cur);
    RET(0, 3);
}
IMPL(mss32, AIL_register_EOS_callback) { Voice *v = V(ARG(0)); uint32_t o = v ? v->eos_cb : 0; if (v) v->eos_cb = ARG(1); RET(o, 2); }

/* 3D: one provider ("Miles Fast 2D Positional Audio"), one listener */
IMPL(mss32, AIL_enumerate_3D_providers)
{
    uint32_t next = ARG(0), dest = ARG(1), name = ARG(2);
    if (rt_r32(G_MEM, next) != 0) RET(0, 3);
    rt_w32(G_MEM, next, 1); rt_w32(G_MEM, dest, provider_h); rt_w32(G_MEM, name, str_in_guest("Miles Fast 2D Positional Audio"));
    RET(1, 3);
}
IMPL(mss32, AIL_open_3D_provider) { start_output(); ensure_service(); RET(0, 1); }   /* M3D_NOERR */
IMPL(mss32, AIL_close_3D_provider) { RET(0, 1); }
IMPL(mss32, AIL_set_3D_room_type) { RET(0, 2); }
IMPL(mss32, AIL_open_3D_listener) { if (!listener_h) listener_h = heap_alloc(w32_process_heap, 8, 64); RET(listener_h, 1); }
IMPL(mss32, AIL_close_3D_listener) { RET(0, 1); }
IMPL(mss32, AIL_allocate_3D_sample_handle) { LOCK; uint32_t h = new_handle(V_3D); UNLOCK; RET(h, 1); }
IMPL(mss32, AIL_release_3D_sample_handle) { LOCK; Voice *v = V(ARG(0)); if (v) voice_free(v); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_set_3D_sample_file) { LOCK; Voice *v = V(ARG(0)); int ok = v && ARG(1) ? set_file(v, ARG(1), 0x1000000u) : 0; UNLOCK; RET(ok, 2); }
IMPL(mss32, AIL_start_3D_sample) { LOCK; Voice *v = V(ARG(0)); if (v) { pthread_mutex_lock(&mix_lock); v->pos = 0; v->status = SMP_PLAYING; pthread_mutex_unlock(&mix_lock); } UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_stop_3D_sample) { LOCK; Voice *v = V(ARG(0)); if (v && v->status == SMP_PLAYING) set_status(v, SMP_STOPPED); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_resume_3D_sample) { LOCK; Voice *v = V(ARG(0)); if (v && v->status == SMP_STOPPED) set_status(v, SMP_PLAYING); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_end_3D_sample) { LOCK; Voice *v = V(ARG(0)); if (v) set_status(v, SMP_DONE); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_3D_sample_status) { LOCK; Voice *v = V(ARG(0)); uint32_t s = v ? (uint32_t)v->status : SMP_FREE; UNLOCK; RET(s, 1); }
IMPL(mss32, AIL_set_3D_sample_volume) { Voice *v = V(ARG(0)); if (v) v->volume = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_3D_sample_volume) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)v->volume : 0, 1); }
IMPL(mss32, AIL_set_3D_sample_playback_rate) { Voice *v = V(ARG(0)); if (v) v->rate_hz = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_3D_sample_playback_rate) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)(v->rate_hz ? v->rate_hz : (v->pcm ? (int)v->pcm->rate : 22050)) : 0, 1); }
IMPL(mss32, AIL_set_3D_sample_loop_count) { Voice *v = V(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_set_3D_sample_distances)
{
    Voice *v = V(ARG(0)); if (v) { float mx, mn; uint32_t a = ARG(1), b = ARG(2); memcpy(&mx, &a, 4); memcpy(&mn, &b, 4); v->max_d = mx; v->min_d = mn > 0 ? mn : 0.01f; }
    RET(0, 3);
}
IMPL(mss32, AIL_set_3D_sample_effects_level) { RET(0, 2); }
IMPL(mss32, AIL_set_3D_sample_offset) { Voice *v = V(ARG(0)); if (v) v->pos = ARG(1) / (double)bytes_per_frame(v); RET(0, 2); }
IMPL(mss32, AIL_3D_sample_offset) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)(v->pos * bytes_per_frame(v)) : 0, 1); }
IMPL(mss32, AIL_3D_sample_length) { Voice *v = V(ARG(0)); RET(v && v->pcm ? v->pcm->src_bytes : 0, 1); }
IMPL(mss32, AIL_register_3D_EOS_callback) { Voice *v = V(ARG(0)); uint32_t o = v ? v->eos_cb : 0; if (v) v->eos_cb = ARG(1); RET(o, 2); }
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
IMPL(mss32, AIL_set_3D_position)
{
    uint32_t o = ARG(0); float x = f32(ARG(1)), y = f32(ARG(2)), z = f32(ARG(3));
    pthread_mutex_lock(&mix_lock);
    if (o == listener_h) { lx = x; ly = y; lz = z; }
    else { Voice *v = V(o); if (v) { v->x = x; v->y = y; v->z = z; } }
    pthread_mutex_unlock(&mix_lock);
    RET(0, 4);
}
IMPL(mss32, AIL_set_3D_orientation)
{
    if (ARG(0) == listener_h) {
        pthread_mutex_lock(&mix_lock);
        lfx = f32(ARG(1)); lfy = f32(ARG(2)); lfz = f32(ARG(3)); lux = f32(ARG(4)); luy = f32(ARG(5)); luz = f32(ARG(6));
        pthread_mutex_unlock(&mix_lock);
    }
    RET(0, 7);
}

/* streams: read the whole file through the game's callbacks (from its archives), decode on a worker thread */
IMPL(mss32, AIL_open_stream)
{
    uint32_t name = ARG(1), scratch = heap_alloc(w32_process_heap, 8, 16), fh = 0;
    ALOG("open_stream %s\n", GS(name));
    if (!file_open) { heap_free(w32_process_heap, scratch); RET(0, 3); }
    uint32_t oa[2] = {name, scratch};
    if (!w32_callback(c, file_open, 2, oa)) { heap_free(w32_process_heap, scratch); ALOG("  cannot open\n"); RET(0, 3); }
    fh = rt_r32(G_MEM, scratch);
    uint32_t sa[3] = {fh, 0, 2}; uint32_t size = w32_callback(c, file_seek, 3, sa);
    sa[2] = 0; w32_callback(c, file_seek, 3, sa);
    uint32_t buf = size ? heap_alloc(w32_process_heap, 0, size) : 0, got = 0;
    if (buf) { uint32_t ra[3] = {fh, buf, size}; got = w32_callback(c, file_read, 3, ra); }
    w32_callback(c, file_close, 1, &fh); heap_free(w32_process_heap, scratch);
    if (!got) { if (buf) heap_free(w32_process_heap, buf); RET(0, 3); }
    LOCK; uint32_t h = new_handle(V_STREAM); Voice *v = V(h);
    Pcm *p = calloc(1, sizeof *p); p->refs = 2; p->src_bytes = got; v->pcm = p;
    DecJob *j = calloc(1, sizeof *j); j->p = p; j->n = got; j->bytes = malloc(got); memcpy(j->bytes, GP(buf), got);
    if (getenv("W32_MILES_SAVE")) {                              /* the stream's file, for reference decodes in tests */
        char path[1024]; const char *b = strrchr(GS(ARG(1)), '\\'); snprintf(path, sizeof path, "%s/%s", getenv("W32_MILES_SAVE"), b ? b + 1 : GS(ARG(1)));
        FILE *f = fopen(path, "wb"); if (f) { fwrite(j->bytes, 1, got, f); fclose(f); }
    }
    heap_free(w32_process_heap, buf);
    pthread_t t; pthread_create(&t, 0, decode_thread, j); pthread_detach(t);
    UNLOCK; RET(h, 3);
}
IMPL(mss32, AIL_close_stream) { LOCK; Voice *v = V(ARG(0)); if (v) voice_free(v); UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_start_stream) { LOCK; Voice *v = V(ARG(0)); if (v) { pthread_mutex_lock(&mix_lock); v->pos = 0; v->status = SMP_PLAYING; pthread_mutex_unlock(&mix_lock); } UNLOCK; RET(0, 1); }
IMPL(mss32, AIL_pause_stream)
{
    LOCK; Voice *v = V(ARG(0));
    if (v) { if (ARG(1) && v->status == SMP_PLAYING) set_status(v, SMP_STOPPED); else if (!ARG(1) && v->status == SMP_STOPPED) set_status(v, SMP_PLAYING); }
    UNLOCK; RET(0, 2);
}
IMPL(mss32, AIL_set_stream_volume) { Voice *v = V(ARG(0)); if (v) v->volume = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_stream_volume) { Voice *v = V(ARG(0)); RET(v ? (uint32_t)v->volume : 0, 1); }
IMPL(mss32, AIL_set_stream_loop_count) { Voice *v = V(ARG(0)); if (v) v->loops = (int)ARG(1); RET(0, 2); }
IMPL(mss32, AIL_stream_status) { LOCK; Voice *v = V(ARG(0)); uint32_t s = v ? (uint32_t)v->status : SMP_FREE; UNLOCK; RET(s, 1); }
IMPL(mss32, AIL_register_stream_callback) { Voice *v = V(ARG(0)); uint32_t o = v ? v->eos_cb : 0; if (v) v->eos_cb = ARG(1); RET(o, 2); }

/* AIL_WAV_info: AILSOUNDINFO {format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr} */
IMPL(mss32, AIL_WAV_info)
{
    uint32_t d = ARG(0), info = ARG(1); const uint8_t *p = (const uint8_t *)GP(d);
    if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) RET(0, 2);
    uint32_t riff; memcpy(&riff, p + 4, 4); uint32_t off = 12, fmt = 0, data = 0, dlen = 0;
    while (off + 8 <= riff + 8) {
        uint32_t len; memcpy(&len, p + off + 4, 4);
        if (!memcmp(p + off, "fmt ", 4)) fmt = off + 8;
        if (!memcmp(p + off, "data", 4)) { data = off + 8; dlen = len; break; }
        off += 8 + ((len + 1) & ~1u);
    }
    if (!fmt || !data) RET(0, 2);
    uint16_t tag, ch, align, bits; uint32_t rate; memcpy(&tag, p + fmt, 2); memcpy(&ch, p + fmt + 2, 2); memcpy(&rate, p + fmt + 4, 4); memcpy(&align, p + fmt + 12, 2); memcpy(&bits, p + fmt + 14, 2);
    uint32_t samples = tag == 1 && align ? dlen / align : 0;
    uint32_t v[9] = {tag, d + data, dlen, rate, bits, ch, samples, align, d + data};
    memcpy(GP(info), v, sizeof v);
    RET(1, 2);
}

/* timers */
IMPL(mss32, AIL_register_timer)
{
    ensure_service(); pthread_mutex_lock(&svc_lock); int k = 0; while (k < 16 && timers[k].used) k++;
    if (k < 16) { memset(&timers[k], 0, sizeof timers[k]); timers[k].used = 1; timers[k].fn = ARG(0); }
    pthread_mutex_unlock(&svc_lock); RET(k < 16 ? (uint32_t)k : 0xffffffffu, 1);
}
IMPL(mss32, AIL_set_timer_user) { uint32_t k = ARG(0), o = 0; if (k < 16) { o = timers[k].user; timers[k].user = ARG(1); } RET(o, 2); }
IMPL(mss32, AIL_set_timer_period) { uint32_t k = ARG(0); if (k < 16) { timers[k].period_us = ARG(1); timers[k].next = now_s() + ARG(1) * 1e-6; } RET(0, 2); }
IMPL(mss32, AIL_start_timer) { uint32_t k = ARG(0); if (k < 16) { pthread_mutex_lock(&svc_lock); timers[k].running = 1; timers[k].next = now_s() + timers[k].period_us * 1e-6; pthread_cond_signal(&svc_cv); pthread_mutex_unlock(&svc_lock); } RET(0, 1); }
IMPL(mss32, AIL_release_timer_handle) { uint32_t k = ARG(0); if (k < 16) { pthread_mutex_lock(&svc_lock); timers[k].used = 0; pthread_mutex_unlock(&svc_lock); } RET(0, 1); }
