/* MCI CD audio (mciSendCommandA on the "cdaudio" device), played from the game folder's MUSIC/TrackNN.ogg as GOG's
 * winmm.dll shim does on Windows. Rainbow Six opens the device, sets the TMSF time format, asks for the number of
 * tracks and plays one track at a time with MCI_NOTIFY (from N to N+1), stopping or closing the device itself; the
 * notification (MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL) goes to the callback window when a track ends.
 * Ogg Vorbis is decoded with stb_vorbis (third_party, public domain) on the audio thread. W32_CD_LOG=1 logs. */
#include "w32.h"
#include "host_audio.h"
#include <math.h>
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "../../third_party/stb_vorbis/stb_vorbis.c"

enum { MCI_OPEN = 0x803, MCI_CLOSE = 0x804, MCI_PLAY = 0x806, MCI_STOP = 0x808, MCI_PAUSE = 0x809, MCI_SET = 0x80d,
       MCI_STATUS = 0x814, MCI_RESUME = 0x855, MCI_NOTIFY = 1, MCI_FROM = 4, MCI_TO = 8, MM_MCINOTIFY = 0x3b9,
       MCIERR_DEVICE_OPEN = 263, MCIERR_INVALID_DEVICE_ID = 257, MCIERR_UNRECOGNIZED_COMMAND = 261, MCIERR_OUTOFRANGE = 282,
       CD_DEVICE = 1, OUT_RATE = 44100 };

static pthread_mutex_t cd_lock = PTHREAD_MUTEX_INITIALIZER;
static DsAudio *cd_out;
static stb_vorbis *cd_ogg; static int cd_ch, cd_rate;
static int cd_open, cd_playing, cd_track, cd_last, cd_format = 10, cd_ntracks = -1;
static uint32_t cd_notify_hwnd;
static double cd_frac; static float cd_prev[2], cd_next[2];
static int cd_log(void) { static int v = -1; if (v < 0) v = getenv("W32_CD_LOG") != 0; return v; }

static int track_path(int n, char *out, size_t cap)
{
    char win[260]; snprintf(win, sizeof win, "C:\\GOG Games\\Dungeon Siege\\MUSIC\\Track%02d.ogg", n);
    if (w32_host_path(win, out, cap, 0)) return -1;
    FILE *f = fopen(out, "rb"); if (!f) return -1; fclose(f); return 0;
}
static int count_tracks(void)                         /* the highest TrackNN.ogg (track 1 is the data track) */
{
    if (cd_ntracks >= 0) return cd_ntracks;
    char p[2048]; int n = 0;
    for (int k = 1; k < 100; k++) if (!track_path(k, p, sizeof p)) n = k;
    return cd_ntracks = n;
}
static void notify_done_locked(void)                  /* the track played to its end */
{
    uint32_t h = cd_notify_hwnd; cd_notify_hwnd = 0;
    if (h) { uint32_t w32_window_tid(uint32_t); void w32_post(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
             w32_post(w32_window_tid(h), h, MM_MCINOTIFY, 1 /* MCI_NOTIFY_SUCCESSFUL */, CD_DEVICE); }
}
static int start_track_locked(int n)
{
    if (cd_ogg) { stb_vorbis_close(cd_ogg); cd_ogg = 0; }
    char p[2048]; if (track_path(n, p, sizeof p)) return -1;
    int err = 0; cd_ogg = stb_vorbis_open_filename(p, &err, NULL);
    if (!cd_ogg) { fprintf(stderr, "cdaudio: %s: vorbis error %d\n", p, err); return -1; }
    stb_vorbis_info i = stb_vorbis_get_info(cd_ogg); cd_ch = i.channels; cd_rate = (int)i.sample_rate;
    cd_track = n; cd_frac = 0; cd_prev[0] = cd_prev[1] = cd_next[0] = cd_next[1] = 0;
    if (cd_log()) fprintf(stderr, "cdaudio: track %d (%s, %d Hz, %d ch)\n", n, p, cd_rate, cd_ch);
    return 0;
}
static int pull(float *lr)                            /* one source frame; 0 at the end of the range */
{
    for (;;) {
        if (!cd_ogg) return 0;
        float buf[2]; int got = cd_ch == 1 ? stb_vorbis_get_samples_float_interleaved(cd_ogg, 1, buf, 1)
                                           : stb_vorbis_get_samples_float_interleaved(cd_ogg, 2, buf, 2);
        if (got > 0) { lr[0] = buf[0]; lr[1] = cd_ch == 1 ? buf[0] : buf[1]; return 1; }
        if (cd_track < cd_last && !start_track_locked(cd_track + 1)) continue;   /* a range of several tracks */
        stb_vorbis_close(cd_ogg); cd_ogg = 0; return 0;
    }
}
static void render(void *u, void *buffer, uint32_t frames)
{
    (void)u; float *out = buffer; memset(out, 0, frames * 8);
    pthread_mutex_lock(&cd_lock);
    if (cd_playing && cd_ogg) {
        double step = (double)cd_rate / OUT_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            while (cd_frac >= 1.0) { cd_prev[0] = cd_next[0]; cd_prev[1] = cd_next[1]; if (!pull(cd_next)) { cd_playing = 0; break; } cd_frac -= 1.0; }
            if (!cd_playing) break;
            float t = (float)cd_frac;
            out[2 * i] = cd_prev[0] + (cd_next[0] - cd_prev[0]) * t; out[2 * i + 1] = cd_prev[1] + (cd_next[1] - cd_prev[1]) * t;
            cd_frac += step;
        }
        if (!cd_playing) notify_done_locked();
    }
    pthread_mutex_unlock(&cd_lock);
    if (getenv("W32_AUDIO_MUTE")) memset(out, 0, frames * 8);
}
static int track_of(uint32_t v) { return cd_format == 10 ? (int)(v & 0xff) : 0; }   /* TMSF: the track in the low byte */

IMPL(winmm, mciSendCommandA)                          /* (device, message, flags, parameters) */
{
    uint32_t dev = ARG(0), msg = ARG(1), fl = ARG(2), p = ARG(3), r = 0;
    pthread_mutex_lock(&cd_lock);
    switch (msg) {
    case MCI_OPEN: {                                  /* MCI_OPEN_PARMS {callback, device id out, type, element, alias} */
        const char *type = (fl & 0x2000) && !(fl & 0x1000) && rt_r32(G_MEM, p + 8) ? GS(rt_r32(G_MEM, p + 8)) : "cdaudio";
        if (strcasecmp(type, "cdaudio") && !((fl & 0x1000) && (rt_r32(G_MEM, p + 8) & 0xffff) == 516)) { r = 275; break; }   /* MCIERR_INVALID_DEVICE_NAME */
        if (count_tracks() < 2) { r = 275; break; }
        cd_open = 1; rt_w32(G_MEM, p + 4, CD_DEVICE); break;
    }
    case MCI_CLOSE: cd_playing = 0; cd_notify_hwnd = 0; if (cd_ogg) { stb_vorbis_close(cd_ogg); cd_ogg = 0; } cd_open = 0; break;
    case MCI_SET: if (fl & 0x400) cd_format = (int)rt_r32(G_MEM, p + 4); break;   /* MCI_SET_TIME_FORMAT */
    case MCI_STATUS: {                                /* MCI_STATUS_PARMS {callback, return, item, track} */
        uint32_t item = rt_r32(G_MEM, p + 8), v = 0;
        switch (item) {
        case 3: v = (uint32_t)count_tracks(); break;                         /* NUMBER_OF_TRACKS */
        case 4: v = cd_playing ? 526 : 525; break;                          /* MODE: playing / stopped */
        case 5: v = 1; break;                                               /* MEDIA_PRESENT */
        case 2: v = cd_format == 10 ? (uint32_t)cd_track : 0; break;         /* POSITION */
        case 0x4001: v = rt_r32(G_MEM, p + 12) == 1 ? 1089 : 1088; break;   /* CDA type of track: data / audio */
        default: v = 0; break;
        }
        rt_w32(G_MEM, p + 4, v); break;
    }
    case MCI_PLAY: {                                  /* MCI_PLAY_PARMS {callback, from, to} */
        if (!cd_open || dev != CD_DEVICE) { r = MCIERR_INVALID_DEVICE_ID; break; }
        int from = (fl & MCI_FROM) ? track_of(rt_r32(G_MEM, p + 4)) : cd_track ? cd_track : 2;
        int to = (fl & MCI_TO) ? track_of(rt_r32(G_MEM, p + 8)) - 1 : count_tracks();   /* TMSF "to" N+1:00:00 ends at track N */
        if (to < from) to = from;
        if (cd_notify_hwnd && (fl & MCI_NOTIFY)) {    /* the previous play is superseded */
            uint32_t h = cd_notify_hwnd; cd_notify_hwnd = 0; uint32_t w32_window_tid(uint32_t); void w32_post(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
            w32_post(w32_window_tid(h), h, MM_MCINOTIFY, 2 /* MCI_NOTIFY_SUPERSEDED */, CD_DEVICE);
        }
        if (start_track_locked(from)) { r = MCIERR_OUTOFRANGE; break; }
        cd_last = to; cd_playing = 1; cd_notify_hwnd = (fl & MCI_NOTIFY) ? rt_r32(G_MEM, p) : 0;
        if (!cd_out) cd_out = ds_audio_open(OUT_RATE, 2, 32, 1, render, NULL);
        break;
    }
    case MCI_STOP: case MCI_PAUSE:
        if (cd_notify_hwnd) { uint32_t h = cd_notify_hwnd; cd_notify_hwnd = 0; uint32_t w32_window_tid(uint32_t); void w32_post(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
                              w32_post(w32_window_tid(h), h, MM_MCINOTIFY, 4 /* MCI_NOTIFY_ABORTED */, CD_DEVICE); }
        cd_playing = 0; break;
    case MCI_RESUME: if (cd_ogg) cd_playing = 1; break;
    default: r = MCIERR_UNRECOGNIZED_COMMAND; break;
    }
    pthread_mutex_unlock(&cd_lock);
    if (cd_log()) fprintf(stderr, "cdaudio: mciSendCommand(%x, %x, %x, %x) -> %u\n", dev, msg, fl, p, r);
    RET(r, 4);
}
