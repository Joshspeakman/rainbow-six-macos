/* DirectShow's filter graph (CLSID_FilterGraph) for what Fallout NV does with it: play its MP3 music. One object with
 * the interfaces the game asks for (IGraphBuilder, IMediaControl, IMediaEventEx, IMediaSeeking, IBasicAudio,
 * IMediaPosition); RenderFile opens the file, minimp3 (third_party) decodes it as it plays through the host audio
 * output, and the end of the stream is reported as EC_COMPLETE through the graph's event handle. */
#include "w32.h"
#include "../host_audio.h"
#include <pthread.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

enum { I_GRAPH, I_CONTROL, I_EVENT, I_SEEKING, I_AUDIO, I_POSITION, NI };
#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define VFW_E_NOT_FOUND 0x80040216u
#define EC_COMPLETE 1u
typedef struct Graph {
    uint32_t g;                                        /* guest object: NI vtable pointers */
    int refs, state;                                   /* 0 stopped, 1 paused, 2 running */
    pthread_mutex_t m; mp3dec_ex_t dec; int open; DsAudio *out; int rate, channels;
    uint64_t pos;                                      /* samples (all channels) played */
    float gain; uint32_t event; int complete_pending, complete_sent; uint32_t notify_hwnd, notify_msg;
    uint8_t *data;                                     /* a stream pulled from the game's own source filter (Render) */
} Graph;
static uint32_t vt[NI];
static Graph *graphs[64]; static int ngraphs;
static Graph *of(Ctx *c, int iface)                    /* `this` -> the graph (each interface is one pointer into it) */
{
    uint32_t base = ARG(0) - 4u * (uint32_t)iface;
    for (int k = 0; k < ngraphs; k++) if (graphs[k] && graphs[k]->g == base) return graphs[k];
    return 0;
}
static int muted(void) { const char *e = getenv("W32_AUDIO_MUTE"); return e && strcmp(e, "0"); }

static void render(void *user, void *samples, uint32_t frames)    /* host audio thread */
{
    Graph *gr = user; int16_t *out = samples; size_t want = (size_t)frames * (size_t)gr->channels;
    pthread_mutex_lock(&gr->m);
    size_t got = 0;
    if (gr->open && gr->state == 2) {
        mp3d_sample_t buf[4096];
        while (got < want) {
            size_t n = want - got; if (n > 4096) n = 4096;
            size_t r = mp3dec_ex_read(&gr->dec, buf, n);
            for (size_t k = 0; k < r; k++) { float v = buf[k] * gr->gain; out[got + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
            got += r; gr->pos += r;
            if (r < n) { if (!gr->complete_sent) { gr->complete_pending = 1; gr->complete_sent = 1; if (gr->event) w32_event_signal(gr->event, 1); } break; }
        }
    }
    pthread_mutex_unlock(&gr->m);
    memset(out + got, 0, (want - got) * sizeof *out);
}
static void close_stream(Graph *gr)
{
    if (gr->out) { ds_audio_close(gr->out); gr->out = 0; }
    pthread_mutex_lock(&gr->m); if (gr->open) mp3dec_ex_close(&gr->dec); gr->open = 0; gr->pos = 0; free(gr->data); gr->data = 0; pthread_mutex_unlock(&gr->m);
}
static uint32_t start_stream(Graph *gr, int err, const char *what)
{
    if (!err) { gr->open = 1; gr->rate = gr->dec.info.hz; gr->channels = gr->dec.info.channels; gr->complete_pending = gr->complete_sent = 0; }
    if (getenv("W32_AUDIOLOG")) fprintf(stderr, "dshow: %s -> %s (%d Hz, %d channels)\n", what, err ? "unreadable" : "ok", gr->rate, gr->channels);
    if (err) return 0x80040218u;                       /* VFW_E_CANNOT_RENDER */
    if (!muted()) gr->out = ds_audio_open(gr->rate, gr->channels, 16, 0, render, gr);
    return S_OK;
}

/* ---- IUnknown, shared by every interface ---- */
static const uint32_t iid_first[NI][3] = {                     /* first dword of each IID this object answers */
    {0x56a868a9u, 0x56a8689fu, 0}, {0x56a868b1u, 0, 0}, {0x56a868c0u, 0x56a868b6u, 0}, {0x36b73880u, 0, 0}, {0x56a868b3u, 0, 0}, {0x56a868b2u, 0, 0}};
static void qi(Ctx *c, int iface)
{
    Graph *gr = of(c, iface); uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (!gr || !out) RET(E_NOINTERFACE, 3);
    for (int k = 0; k < NI; k++) for (int j = 0; j < 3 && iid_first[k][j]; j++) if (iid == iid_first[k][j]) { rt_w32(G_MEM, out, gr->g + 4u * (uint32_t)k); gr->refs++; RET(S_OK, 3); }
    if (iid == 0 /* IUnknown */) { rt_w32(G_MEM, out, gr->g); gr->refs++; RET(S_OK, 3); }
    rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
}
static void addref(Ctx *c, int iface) { Graph *gr = of(c, iface); RET(gr ? (uint32_t)++gr->refs : 0, 1); }
static void release(Ctx *c, int iface)
{
    Graph *gr = of(c, iface); if (!gr) RET(0, 1);
    int r = --gr->refs;
    if (r <= 0) { close_stream(gr); for (int k = 0; k < ngraphs; k++) if (graphs[k] == gr) graphs[k] = 0; free(gr); }
    RET((uint32_t)(r > 0 ? r : 0), 1);
}
#define IUNK(pfx, k) static void pfx##_qi(Ctx *c) { qi(c, k); } static void pfx##_add(Ctx *c) { addref(c, k); } static void pfx##_rel(Ctx *c) { release(c, k); }
#define DISPATCH(pfx) static void pfx##_tic(Ctx *c) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(S_OK, 2); } \
    static void pfx##_ti(Ctx *c) { RET(E_NOTIMPL, 4); } static void pfx##_ids(Ctx *c) { RET(E_NOTIMPL, 6); } static void pfx##_inv(Ctx *c) { RET(E_NOTIMPL, 9); }
IUNK(gb, I_GRAPH) IUNK(mc, I_CONTROL) IUNK(me, I_EVENT) IUNK(ms, I_SEEKING) IUNK(ba, I_AUDIO) IUNK(mp, I_POSITION)
DISPATCH(mc) DISPATCH(me) DISPATCH(ba) DISPATCH(mp)

/* ---- IGraphBuilder ---- */
static uint32_t render_file(Graph *gr, uint32_t wname)
{
    char a[1024]; int k = 0; for (; k < 1023 && rt_r16(G_MEM, wname + 2u * (uint32_t)k); k++) a[k] = (char)rt_r16(G_MEM, wname + 2u * (uint32_t)k); a[k] = 0;
    char host[2048]; if (w32_host_path(a, host, sizeof host, 0)) return VFW_E_NOT_FOUND;
    close_stream(gr);
    pthread_mutex_lock(&gr->m); int err = mp3dec_ex_open(&gr->dec, host, MP3D_SEEK_TO_SAMPLE); pthread_mutex_unlock(&gr->m);
    char what[1100]; snprintf(what, sizeof what, "RenderFile %s", a);
    return start_stream(gr, err, what);
}
/* Render(output pin): the game's own source filter. Its pin is read through IAsyncReader (Length, SyncRead) into memory */
static uint32_t render_pin(Ctx *c, Graph *gr, uint32_t pin)
{
    static const uint8_t iid_reader[16] = {0xaa, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70};
    uint32_t tmp = heap_alloc(w32_process_heap, 8, 64); memcpy(GP(tmp), iid_reader, 16);
    uint32_t a[5] = {pin, tmp, tmp + 16};
    uint32_t hr = w32_callback(c, rt_r32(G_MEM, rt_r32(G_MEM, pin)), 3, a), rdr = rt_r32(G_MEM, tmp + 16);
    if (hr || !rdr) { heap_free(w32_process_heap, tmp); return 0x80040218u; }
    uint32_t rvt = rt_r32(G_MEM, rdr);
    a[0] = rdr; a[1] = tmp + 24; a[2] = tmp + 32; w32_callback(c, rt_r32(G_MEM, rvt + 8 * 4), 3, a);           /* Length (IAsyncReader: 7 SyncRead, 8 Length) */
    uint32_t lhr = c->eax; uint64_t total = rt_r64(G_MEM, tmp + 24); uint8_t *data = 0; size_t got = 0;
    if (getenv("W32_AUDIOLOG")) fprintf(stderr, "dshow: pin %08x reader %08x (vtable %08x) Length -> %08x, total %llu, available %llu\n", pin, rdr, rvt, lhr,
                                        (unsigned long long)total, (unsigned long long)rt_r64(G_MEM, tmp + 32));
    if (total && total < (64u << 20)) {
        data = malloc((size_t)total); uint32_t chunk = 1u << 20, buf = heap_alloc(w32_process_heap, 0, chunk);
        while (got < total) {
            uint32_t n = total - got < chunk ? (uint32_t)(total - got) : chunk;
            uint32_t r[5] = {rdr, (uint32_t)got, (uint32_t)((uint64_t)got >> 32), n, buf};
            if (w32_callback(c, rt_r32(G_MEM, rvt + 7 * 4), 5, r) > 1) break;                                   /* SyncRead: S_OK or S_FALSE (short) */
            memcpy(data + got, GP(buf), n); got += n;
        }
        heap_free(w32_process_heap, buf);
    }
    a[0] = rdr; w32_callback(c, rt_r32(G_MEM, rvt + 2 * 4), 1, a);                                            /* Release */
    heap_free(w32_process_heap, tmp);
    close_stream(gr);
    pthread_mutex_lock(&gr->m); int err = !data || !got; gr->data = data;
    if (!err) err = mp3dec_ex_open_buf(&gr->dec, data, got, MP3D_SEEK_TO_SAMPLE);
    pthread_mutex_unlock(&gr->m);
    char what[64]; snprintf(what, sizeof what, "Render (source filter, %zu bytes)", got);
    return start_stream(gr, err, what);
}
static void gb_Render(Ctx *c) { Graph *gr = of(c, I_GRAPH); uint32_t pin = ARG(1); RET(gr && pin ? render_pin(c, gr, pin) : 0x80004003u, 2); }
static void gb_RenderFile(Ctx *c) { Graph *gr = of(c, I_GRAPH); RET(gr ? render_file(gr, ARG(1)) : E_NOTIMPL, 3); }
static void gb_ok1(Ctx *c) { RET(S_OK, 1); } static void gb_ok2(Ctx *c) { RET(S_OK, 2); } static void gb_ok3(Ctx *c) { RET(S_OK, 3); }
static void gb_ok4(Ctx *c) { RET(S_OK, 4); } static void gb_ni3(Ctx *c) { RET(E_NOTIMPL, 3); } static void gb_ni4(Ctx *c) { RET(E_NOTIMPL, 4); }
static void gb_ni2(Ctx *c) { RET(E_NOTIMPL, 2); }

/* ---- IMediaControl ---- */
static void set_state(Graph *gr, int st)
{
    pthread_mutex_lock(&gr->m);
    if (st == 0) { if (gr->open) mp3dec_ex_seek(&gr->dec, 0); gr->pos = 0; gr->complete_pending = gr->complete_sent = 0; }
    gr->state = st; pthread_mutex_unlock(&gr->m);
}
static void mc_Run(Ctx *c) { Graph *gr = of(c, I_CONTROL); if (gr) set_state(gr, 2); RET(S_OK, 1); }
static void mc_Pause(Ctx *c) { Graph *gr = of(c, I_CONTROL); if (gr) set_state(gr, 1); RET(S_OK, 1); }
static void mc_Stop(Ctx *c) { Graph *gr = of(c, I_CONTROL); if (gr) set_state(gr, 0); RET(S_OK, 1); }
static void mc_GetState(Ctx *c) { Graph *gr = of(c, I_CONTROL); if (ARG(2)) rt_w32(G_MEM, ARG(2), gr ? (uint32_t)gr->state : 0); RET(S_OK, 3); }
static void mc_RenderFile(Ctx *c) { Graph *gr = of(c, I_CONTROL); RET(gr ? render_file(gr, ARG(1)) : E_NOTIMPL, 2); }   /* BSTR: a wide string */
static void mc_ni2(Ctx *c) { RET(E_NOTIMPL, 2); } static void mc_ni3(Ctx *c) { RET(E_NOTIMPL, 3); }
static void mc_StopWhenReady(Ctx *c) { Graph *gr = of(c, I_CONTROL); if (gr) set_state(gr, 0); RET(S_OK, 1); }

/* ---- IMediaEventEx ---- */
static void me_GetEventHandle(Ctx *c)
{
    Graph *gr = of(c, I_EVENT); if (!gr) RET(E_NOTIMPL, 2);
    if (!gr->event) gr->event = w32_event_new(1, 0);
    rt_w32(G_MEM, ARG(1), gr->event); RET(S_OK, 2);
}
static void me_GetEvent(Ctx *c)                        /* (code *, param1 *, param2 *, timeout) */
{
    Graph *gr = of(c, I_EVENT); if (!gr) RET(E_NOTIMPL, 5);
    pthread_mutex_lock(&gr->m); int ev = gr->complete_pending; gr->complete_pending = 0;
    if (gr->event) w32_event_signal(gr->event, 0);
    pthread_mutex_unlock(&gr->m);
    if (!ev) RET(0x80004004u /* E_ABORT: none within the timeout */, 5);
    rt_w32(G_MEM, ARG(1), EC_COMPLETE); rt_w32(G_MEM, ARG(2), 0); rt_w32(G_MEM, ARG(3), 0); RET(S_OK, 5);
}
static void me_WaitForCompletion(Ctx *c) { Graph *gr = of(c, I_EVENT); int done = gr && gr->complete_sent; if (ARG(2)) rt_w32(G_MEM, ARG(2), done ? EC_COMPLETE : 0); RET(done ? S_OK : 0x80004004u, 3); }
static void me_ok2(Ctx *c) { RET(S_OK, 2); } static void me_ok4(Ctx *c) { RET(S_OK, 4); }
static void me_SetNotifyWindow(Ctx *c) { Graph *gr = of(c, I_EVENT); if (gr) { gr->notify_hwnd = ARG(1); gr->notify_msg = ARG(2); } RET(S_OK, 4); }
static void me_GetNotifyFlags(Ctx *c) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(S_OK, 2); }

/* ---- IMediaSeeking (100 ns units) ---- */
static uint64_t duration_100ns(Graph *gr) { return gr->open && gr->rate && gr->channels ? gr->dec.samples * 10000000ull / ((uint64_t)gr->rate * (uint64_t)gr->channels) : 0; }
static uint64_t position_100ns(Graph *gr) { return gr->rate && gr->channels ? gr->pos * 10000000ull / ((uint64_t)gr->rate * (uint64_t)gr->channels) : 0; }
static void seek_100ns(Graph *gr, uint64_t t)
{
    pthread_mutex_lock(&gr->m);
    if (gr->open) { uint64_t s = t * (uint64_t)gr->rate / 10000000ull * (uint64_t)gr->channels; mp3dec_ex_seek(&gr->dec, s); gr->pos = s; gr->complete_sent = 0; }
    pthread_mutex_unlock(&gr->m);
}
static void ms_GetCapabilities(Ctx *c) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0x1 | 0x2 | 0x8 | 0x40 | 0x80 | 0x100); RET(S_OK, 2); }   /* seek, get current/duration/stop position */
static void ms_CheckCapabilities(Ctx *c) { RET(S_OK, 2); }
static void ms_IsFormatSupported(Ctx *c) { RET(S_OK, 2); }
static void ms_QueryPreferredFormat(Ctx *c) { memset(GP(ARG(1)), 0, 16); rt_w32(G_MEM, ARG(1), 0x7b785574u); RET(S_OK, 2); }   /* TIME_FORMAT_MEDIA_TIME */
static void ms_GetTimeFormat(Ctx *c) { memset(GP(ARG(1)), 0, 16); rt_w32(G_MEM, ARG(1), 0x7b785574u); RET(S_OK, 2); }
static void ms_IsUsingTimeFormat(Ctx *c) { RET(S_OK, 2); }
static void ms_SetTimeFormat(Ctx *c) { RET(S_OK, 2); }
static void ms_GetDuration(Ctx *c) { Graph *gr = of(c, I_SEEKING); rt_w64(G_MEM, ARG(1), gr ? duration_100ns(gr) : 0); RET(S_OK, 2); }
static void ms_GetStopPosition(Ctx *c) { Graph *gr = of(c, I_SEEKING); rt_w64(G_MEM, ARG(1), gr ? duration_100ns(gr) : 0); RET(S_OK, 2); }
static void ms_GetCurrentPosition(Ctx *c) { Graph *gr = of(c, I_SEEKING); rt_w64(G_MEM, ARG(1), gr ? position_100ns(gr) : 0); RET(S_OK, 2); }
static void ms_ConvertTimeFormat(Ctx *c) { rt_w64(G_MEM, ARG(1), rt_r64(G_MEM, ARG(3))); RET(S_OK, 5); }
static void ms_SetPositions(Ctx *c)                    /* (current *, flags, stop *, flags) */
{
    Graph *gr = of(c, I_SEEKING); uint32_t cur = ARG(1), fl = ARG(2);
    if (gr && cur && (fl & 3) == 1) seek_100ns(gr, rt_r64(G_MEM, cur));            /* AM_SEEKING_AbsolutePositioning */
    RET(S_OK, 5);
}
static void ms_GetPositions(Ctx *c) { Graph *gr = of(c, I_SEEKING); if (ARG(1)) rt_w64(G_MEM, ARG(1), gr ? position_100ns(gr) : 0); if (ARG(2)) rt_w64(G_MEM, ARG(2), gr ? duration_100ns(gr) : 0); RET(S_OK, 3); }
static void ms_GetAvailable(Ctx *c) { Graph *gr = of(c, I_SEEKING); if (ARG(1)) rt_w64(G_MEM, ARG(1), 0); if (ARG(2)) rt_w64(G_MEM, ARG(2), gr ? duration_100ns(gr) : 0); RET(S_OK, 3); }
static void ms_SetRate(Ctx *c) { RET(S_OK, 3); }                                   /* double: two dwords */
static void ms_GetRate(Ctx *c) { double one = 1.0; memcpy(GP(ARG(1)), &one, 8); RET(S_OK, 2); }
static void ms_GetPreroll(Ctx *c) { rt_w64(G_MEM, ARG(1), 0); RET(S_OK, 2); }

/* ---- IBasicAudio: volume in hundredths of a decibel (-10000 silent .. 0 full) ---- */
static void ba_put_Volume(Ctx *c) { Graph *gr = of(c, I_AUDIO); int32_t v = (int32_t)ARG(1); if (gr) gr->gain = v <= -10000 ? 0.0f : powf(10.0f, (float)v / 2000.0f); RET(S_OK, 2); }
static void ba_get_Volume(Ctx *c) { Graph *gr = of(c, I_AUDIO); int32_t v = gr && gr->gain > 0 ? (int32_t)(2000.0f * log10f(gr->gain)) : -10000; rt_w32(G_MEM, ARG(1), (uint32_t)v); RET(S_OK, 2); }
static void ba_put_Balance(Ctx *c) { RET(S_OK, 2); }
static void ba_get_Balance(Ctx *c) { rt_w32(G_MEM, ARG(1), 0); RET(S_OK, 2); }

/* ---- IMediaPosition: seconds as doubles ---- */
static void mp_get_Duration(Ctx *c) { Graph *gr = of(c, I_POSITION); double d = gr ? duration_100ns(gr) / 1e7 : 0; memcpy(GP(ARG(1)), &d, 8); RET(S_OK, 2); }
static void mp_put_CurrentPosition(Ctx *c) { Graph *gr = of(c, I_POSITION); double t; uint32_t w[2] = {ARG(1), ARG(2)}; memcpy(&t, w, 8); if (gr && t >= 0) seek_100ns(gr, (uint64_t)(t * 1e7)); RET(S_OK, 3); }
static void mp_get_CurrentPosition(Ctx *c) { Graph *gr = of(c, I_POSITION); double t = gr ? position_100ns(gr) / 1e7 : 0; memcpy(GP(ARG(1)), &t, 8); RET(S_OK, 2); }
static void mp_get_d(Ctx *c) { double z = 0; memcpy(GP(ARG(1)), &z, 8); RET(S_OK, 2); }
static void mp_put_d(Ctx *c) { RET(S_OK, 3); }
static void mp_get_Rate(Ctx *c) { double one = 1.0; memcpy(GP(ARG(1)), &one, 8); RET(S_OK, 2); }
static void mp_can(Ctx *c) { rt_w32(G_MEM, ARG(1), 0xffffffffu); RET(S_OK, 2); }   /* OATRUE */

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "quartz!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
/* CLSID_FilterGraph {e436ebb3-524f-11ce-9f53-0020af0ba770}: called from CoCreateInstance; 0 for other classes */
uint32_t w32_dshow_create(uint32_t clsid_first_dword)
{
    if (clsid_first_dword != 0xe436ebb3u) return 0;
    if (!vt[0]) {
        static void (*const gb[])(Ctx *) = { gb_qi, gb_add, gb_rel, gb_ok3 /* AddFilter */, gb_ok2 /* RemoveFilter */, gb_ni2 /* EnumFilters */,
            gb_ni3 /* FindFilterByName */, gb_ni4 /* ConnectDirect */, gb_ni2 /* Reconnect */, gb_ni2 /* Disconnect */, gb_ok1 /* SetDefaultSyncSource */,
            gb_ni3 /* Connect */, gb_Render, gb_RenderFile, gb_ni4 /* AddSourceFilter */, gb_ok2 /* SetLogFile */, gb_ok1 /* Abort */, gb_ok1 /* ShouldOperationContinue */ };
        static void (*const mc[])(Ctx *) = { mc_qi, mc_add, mc_rel, mc_tic, mc_ti, mc_ids, mc_inv, mc_Run, mc_Pause, mc_Stop, mc_GetState,
            mc_RenderFile, mc_ni3 /* AddSourceFilter */, mc_ni2 /* get_FilterCollection */, mc_ni2 /* get_RegFilterCollection */, mc_StopWhenReady };
        static void (*const me[])(Ctx *) = { me_qi, me_add, me_rel, me_tic, me_ti, me_ids, me_inv, me_GetEventHandle, me_GetEvent, me_WaitForCompletion,
            me_ok2 /* CancelDefaultHandling */, me_ok2 /* RestoreDefaultHandling */, me_ok4 /* FreeEventParams */, me_SetNotifyWindow, me_ok2 /* SetNotifyFlags */, me_GetNotifyFlags };
        static void (*const ms[])(Ctx *) = { ms_qi, ms_add, ms_rel, ms_GetCapabilities, ms_CheckCapabilities, ms_IsFormatSupported, ms_QueryPreferredFormat,
            ms_GetTimeFormat, ms_IsUsingTimeFormat, ms_SetTimeFormat, ms_GetDuration, ms_GetStopPosition, ms_GetCurrentPosition, ms_ConvertTimeFormat,
            ms_SetPositions, ms_GetPositions, ms_GetAvailable, ms_SetRate, ms_GetRate, ms_GetPreroll };
        static void (*const ba[])(Ctx *) = { ba_qi, ba_add, ba_rel, ba_tic, ba_ti, ba_ids, ba_inv, ba_put_Volume, ba_get_Volume, ba_put_Balance, ba_get_Balance };
        static void (*const mp[])(Ctx *) = { mp_qi, mp_add, mp_rel, mp_tic, mp_ti, mp_ids, mp_inv, mp_get_Duration, mp_put_CurrentPosition, mp_get_CurrentPosition,
            mp_get_d /* get_StopTime */, mp_put_d /* put_StopTime */, mp_get_d /* get_PrerollTime */, mp_put_d /* put_PrerollTime */, mp_put_d /* put_Rate */,
            mp_get_Rate, mp_can /* CanSeekForward */, mp_can /* CanSeekBackward */ };
        _Static_assert(sizeof gb / sizeof *gb == 18 && sizeof mc / sizeof *mc == 16 && sizeof me / sizeof *me == 16 && sizeof ms / sizeof *ms == 20 &&
                       sizeof ba / sizeof *ba == 11 && sizeof mp / sizeof *mp == 18, "vtable sizes");
        vt[I_GRAPH] = vtable("IGraphBuilder", gb, 18); vt[I_CONTROL] = vtable("IMediaControl", mc, 16); vt[I_EVENT] = vtable("IMediaEventEx", me, 16);
        vt[I_SEEKING] = vtable("IMediaSeeking", ms, 20); vt[I_AUDIO] = vtable("IBasicAudio", ba, 11); vt[I_POSITION] = vtable("IMediaPosition", mp, 18);
    }
    Graph *gr = calloc(1, sizeof *gr); pthread_mutex_init(&gr->m, 0); gr->refs = 1; gr->gain = 1.0f;
    gr->g = heap_alloc(w32_process_heap, 8, 4u * NI);
    for (int k = 0; k < NI; k++) rt_w32(G_MEM, gr->g + 4u * (uint32_t)k, vt[k]);
    int slot = -1; for (int k = 0; k < ngraphs; k++) if (!graphs[k]) { slot = k; break; }
    if (slot < 0 && ngraphs < 64) slot = ngraphs++;
    if (slot < 0) { free(gr); return 0; }
    graphs[slot] = gr; return gr->g;
}
