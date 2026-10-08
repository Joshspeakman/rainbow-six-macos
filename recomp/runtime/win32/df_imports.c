/* Imports Delta Force and Delta Force 2 use that the other ports never needed: the 16-bit-era file calls (_lopen ...),
 * Watcom's console and memory calls, MIDI and joystick queries (answered "no device"), and the Novalogic DLLs that are
 * not recompiled: Netcomm.dll (NovaWorld voice chat) and iBrowse.dll (NovaWorld's built-in web pages) report failure,
 * so the game runs without them. DirectPlay (DPLAYX) is only probed by the games; their LAN play is Netsock.dll. */
#include "w32.h"
#include <time.h>

/* calls another stdcall import with its own arguments on a scratch frame below the caller's */
static uint32_t call_imp(Ctx *c, void (*fn)(Ctx *), int n, const uint32_t *args)
{
    uint32_t esp = c->esp; c->esp -= 4u * (uint32_t)(n + 1) + 64;
    rt_w32(G_MEM, c->esp, 0);
    for (int k = 0; k < n; k++) rt_w32(G_MEM, c->esp + 4 + 4u * (uint32_t)k, args[k]);
    fn(c); c->esp = esp; return c->eax;
}
void imp_kernel32_CreateFileA(Ctx *), imp_kernel32_ReadFile(Ctx *), imp_kernel32_WriteFile(Ctx *),
     imp_kernel32_SetFilePointer(Ctx *), imp_kernel32_CloseHandle(Ctx *), imp_kernel32_SetEndOfFile(Ctx *);
static uint32_t scratch(void) { static uint32_t p; if (!p) p = heap_alloc(w32_process_heap, 8, 16); return p; }

/* ---- kernel32: OpenFile-era handles (HFILE = the file handle, HFILE_ERROR = -1) ---- */
IMPL(kernel32, _lopen)                                             /* (path, OF_READ 0 / OF_WRITE 1 / OF_READWRITE 2) */
{
    uint32_t m = ARG(1) & 3, access = m == 0 ? 0x80000000u : m == 1 ? 0x40000000u : 0xc0000000u;
    uint32_t a[7] = {ARG(0), access, 3, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
    uint32_t h = call_imp(c, imp_kernel32_CreateFileA, 7, a); RET(h, 2);
}
IMPL(kernel32, _lcreat)                                            /* (path, attributes): create or truncate */
{
    uint32_t a[7] = {ARG(0), 0xc0000000u, 3, 0, 2 /* CREATE_ALWAYS */, ARG(1) ? ARG(1) : 0x80, 0};
    uint32_t h = call_imp(c, imp_kernel32_CreateFileA, 7, a); RET(h, 2);
}
static uint32_t lrw(Ctx *c, void (*fn)(Ctx *), uint32_t h, uint32_t buf, uint32_t n)
{
    uint32_t done = scratch(); rt_w32(G_MEM, done, 0);
    uint32_t a[5] = {h, buf, n, done, 0};
    return call_imp(c, fn, 5, a) ? rt_r32(G_MEM, done) : 0xffffffffu;
}
IMPL(kernel32, _lread) { uint32_t r = lrw(c, imp_kernel32_ReadFile, ARG(0), ARG(1), ARG(2)); RET(r, 3); }
IMPL(kernel32, _hread) { uint32_t r = lrw(c, imp_kernel32_ReadFile, ARG(0), ARG(1), ARG(2)); RET(r, 3); }
IMPL(kernel32, _lwrite)
{
    if (!ARG(2)) { uint32_t a[1] = {ARG(0)}; call_imp(c, imp_kernel32_SetEndOfFile, 1, a); RET(0, 3); }   /* 0 bytes: truncate */
    uint32_t r = lrw(c, imp_kernel32_WriteFile, ARG(0), ARG(1), ARG(2)); RET(r, 3);
}
IMPL(kernel32, _hwrite) { uint32_t r = lrw(c, imp_kernel32_WriteFile, ARG(0), ARG(1), ARG(2)); RET(r, 3); }
IMPL(kernel32, _llseek) { uint32_t a[4] = {ARG(0), ARG(1), 0, ARG(2)}; uint32_t r = call_imp(c, imp_kernel32_SetFilePointer, 4, a); RET(r, 3); }

/* ---- kernel32: memory ---- */
IMPL(kernel32, GlobalReAlloc)                                      /* (mem, size, flags) */
{
    if (ARG(2) & 0x80) RET(ARG(0), 3);                             /* GMEM_MODIFY: attributes only */
    RET(heap_realloc(w32_process_heap, (ARG(2) & 0x40) ? 8 : 0, ARG(0), ARG(1) ? ARG(1) : 1), 3);
}
IMPL(kernel32, GlobalSize) { RET(ARG(0) ? heap_size(w32_process_heap, ARG(0)) : 0, 1); }
IMPL(kernel32, LocalLock) { RET(ARG(0), 1); }
IMPL(kernel32, LocalUnlock) { RET(0, 1); }
IMPL(kernel32, LocalReAlloc)
{
    if (ARG(2) & 0x80) RET(ARG(0), 3);                             /* LMEM_MODIFY */
    RET(heap_realloc(w32_process_heap, (ARG(2) & 0x40) ? 8 : 0, ARG(0), ARG(1) ? ARG(1) : 1), 3);
}
IMPL(kernel32, MulDiv)                                             /* (a, b, c): a * b / c rounded, -1 on c == 0 */
{
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) RET(0xffffffffu, 3);
    int64_t p = a * b, q = (p >= 0) == (d > 0) ? (p + (d > 0 ? d : -d) / 2) / d : (p - (d > 0 ? d : -d) / 2) / d;
    RET((uint32_t)(int32_t)q, 3);
}
IMPL(kernel32, GetExitCodeProcess) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(1, 2); }

/* ---- kernel32: DOS dates (the game's archive and save file times) ---- */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2; const int64_t era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
IMPL(kernel32, DosDateTimeToFileTime)                              /* (date, time, FILETIME *) */
{
    uint32_t dd = ARG(0) & 0xffff, tt = ARG(1) & 0xffff;
    int64_t days = days_from_civil(1980 + (int)(dd >> 9), (dd >> 5) & 15, dd & 31);
    int64_t secs = days * 86400 + (int64_t)(tt >> 11) * 3600 + ((tt >> 5) & 63) * 60 + (tt & 31) * 2;
    rt_w64(G_MEM, ARG(2), (uint64_t)(secs + 11644473600ll) * 10000000ull); RET(1, 3);
}
IMPL(kernel32, FileTimeToDosDateTime)                              /* (FILETIME *, WORD *date, WORD *time) */
{
    time_t t = (time_t)(rt_r64(G_MEM, ARG(0)) / 10000000ull - 11644473600ull); struct tm tm; gmtime_r(&t, &tm);
    if (tm.tm_year < 80) RET(0, 3);
    if (ARG(1)) rt_w16(G_MEM, ARG(1), (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    if (ARG(2)) rt_w16(G_MEM, ARG(2), (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    RET(1, 3);
}
IMPL(kernel32, LocalFileTimeToFileTime)
{
    time_t now = time(0); struct tm lt; localtime_r(&now, &lt);
    rt_w64(G_MEM, ARG(1), rt_r64(G_MEM, ARG(0)) - (uint64_t)((int64_t)lt.tm_gmtoff * 10000000ll)); RET(1, 2);
}

/* ---- kernel32: Watcom's console support (a GUI program: no console) ---- */
IMPL(kernel32, SetConsoleMode) { w32_set_last_error(c, 6); RET(0, 2); }
IMPL(kernel32, ReadConsoleInputA) { if (ARG(3)) rt_w32(G_MEM, ARG(3), 0); w32_set_last_error(c, 6); RET(0, 4); }

/* ---- user32 ---- */
IMPL(user32, CharUpperBuffA)                                       /* (str, len) -> len */
{
    uint8_t *p = GP(ARG(0)); for (uint32_t k = 0; k < ARG(1); k++) if (p[k] >= 'a' && p[k] <= 'z') p[k] -= 32;
    RET(ARG(1), 2);
}
IMPL(user32, GetKeyboardState) { extern uint8_t w32_keys[256]; memcpy(GP(ARG(0)), w32_keys, 256); RET(1, 1); }
IMPL(user32, SetKeyboardState) { RET(1, 1); }
IMPL(user32, EnumWindows) { RET(1, 2); }                           /* (callback, lparam): no other windows */
IMPL(user32, DialogBoxParamA)                                      /* (instance, template, owner, proc, param) */
{
    fprintf(stderr, "w32: DialogBoxParamA(template %#x) not shown\n", ARG(1)); RET(0xffffffffu, 5);
}
IMPL(user32, SendDlgItemMessageA) { RET(0, 5); }

/* ---- gdi32: windowed mode. Both games can run in a window, drawing each frame with StretchDIBits (Delta Force 2) or
 * SetDIBitsToDevice (Delta Force); the picture becomes the frame on screen, as DirectDraw's primary does. ---- */
extern uint32_t w32_logpal[256];                                        /* sims_imports.c: CreatePalette */
IMPL(gdi32, SetPaletteEntries)                                          /* (palette, first, count, entries) */
{
    uint32_t st = ARG(1), n = ARG(2);
    for (uint32_t k = 0; k < n && st + k < 256; k++) w32_logpal[st + k] = rt_r32(G_MEM, ARG(3) + 4 * k);
    RET(n, 4);
}
IMPL(gdi32, SetSystemPaletteUse) { RET(1, 2); }                         /* SYSPAL_STATIC */
uint32_t *dsr_frame_begin(uint32_t w, uint32_t h); void dsr_frame_end(Ctx *c); void dsr_frame_palette(const uint32_t *bgra);
/* a DIB's rectangle, scaled to dw x dh, as the frame on screen. usage 0: RGBQUAD colour table, 1: indices into the
 * logical palette */
static void dib_show(Ctx *c, int32_t dw, int32_t dh, int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t bits, uint32_t bmi, uint32_t usage)
{
    int32_t w = (int32_t)rt_r32(G_MEM, bmi + 4), h = (int32_t)rt_r32(G_MEM, bmi + 8), ah = h < 0 ? -h : h;
    uint32_t hsz = rt_r32(G_MEM, bmi), bpp = rt_r16(G_MEM, bmi + 14), comp = rt_r32(G_MEM, bmi + 16), used = rt_r32(G_MEM, bmi + 32);
    if (w <= 0 || !ah || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || dw > 4096 || dh > 4096 || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return;
    uint32_t pitch = ((uint32_t)w * bpp / 8 + 3) & ~3u, lut[256] = {0};
    uint32_t rm = 0x7c00, gm = 0x3e0, bm = 0x1f;
    if (bpp == 16 && comp == 3) { rm = rt_r32(G_MEM, bmi + hsz); gm = rt_r32(G_MEM, bmi + hsz + 4); bm = rt_r32(G_MEM, bmi + hsz + 8); }
    if (bpp == 8) {
        uint32_t n = used && used <= 256 ? used : 256;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t e;
            if (usage == 1) { uint32_t i = rt_r16(G_MEM, bmi + hsz + 2 * k) & 255, p = w32_logpal[i]; e = (p & 255) << 16 | (p & 0xff00) | (p >> 16 & 255); }
            else e = rt_r32(G_MEM, bmi + hsz + 4 * k) & 0xffffff;          /* RGBQUAD: b, g, r */
            lut[k] = 0xff000000u | e;
        }
        dsr_frame_palette(lut);
    }
    uint32_t *out = dsr_frame_begin((uint32_t)dw, (uint32_t)dh);
    for (int32_t y = 0; y < dh; y++) {
        int32_t yy = sy + (int32_t)((int64_t)y * sh / dh); if (yy >= ah) yy = ah - 1;
        const uint8_t *row = G_MEM + bits + (uint32_t)(h < 0 ? yy : ah - 1 - yy) * pitch;
        for (int32_t x = 0; x < dw; x++) {
            int32_t xx = sx + (int32_t)((int64_t)x * sw / dw); if (xx >= w) xx = w - 1;
            uint32_t v;
            if (bpp == 8) v = lut[row[xx]];
            else if (bpp == 16) { uint32_t p = (uint32_t)(row[2 * xx] | row[2 * xx + 1] << 8);
                v = 0xff000000u | (uint32_t)((p & rm) * 255 / rm) << 16 | (uint32_t)((p & gm) * 255 / gm) << 8 | (uint32_t)((p & bm) * 255 / bm); }
            else if (bpp == 24) v = 0xff000000u | (uint32_t)row[3 * xx + 2] << 16 | (uint32_t)row[3 * xx + 1] << 8 | row[3 * xx];
            else { memcpy(&v, row + 4 * xx, 4); v |= 0xff000000u; }
            out[(uint32_t)y * (uint32_t)dw + (uint32_t)x] = v;
        }
    }
    dsr_frame_end(c);
}
IMPL(gdi32, StretchDIBits)          /* (dc, x, y, w, h, src x, y, w, h, bits, BITMAPINFO, usage, rop) -> lines */
{
    dib_show(c, (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6), (int32_t)ARG(7), (int32_t)ARG(8), ARG(9), ARG(10), ARG(11));
    RET(ARG(8), 13);
}
IMPL(gdi32, SetDIBitsToDevice)      /* (dc, x, y, w, h, src x, y, start scan, lines, bits, BITMAPINFO, usage) -> lines */
{
    dib_show(c, (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6), (int32_t)ARG(3), (int32_t)ARG(4), ARG(9), ARG(10), ARG(11));
    RET(ARG(8), 12);
}

/* ---- winmm: no MIDI devices, no joystick (yet) ---- */
IMPL(winmm, midiOutGetNumDevs) { RET(0, 0); }
IMPL(winmm, midiOutGetDevCapsA) { RET(2, 3); }                     /* MMSYSERR_BADDEVICEID */
IMPL(winmm, midiStreamOpen) { RET(6, 6); }                         /* MMSYSERR_NODRIVER */
IMPL(winmm, midiStreamClose) { RET(5, 1); }                        /* MMSYSERR_INVALHANDLE */
IMPL(winmm, midiStreamOut) { RET(5, 3); }
IMPL(winmm, midiStreamPause) { RET(5, 1); }
IMPL(winmm, midiStreamRestart) { RET(5, 1); }
IMPL(winmm, midiStreamStop) { RET(5, 1); }
IMPL(winmm, midiStreamPosition) { RET(5, 3); }
IMPL(winmm, midiStreamProperty) { RET(5, 3); }
IMPL(winmm, joyGetDevCapsA) { RET(167, 3); }                       /* JOYERR_UNPLUGGED */
IMPL(winmm, joyGetPosEx) { RET(167, 2); }
IMPL(winmm, mixerGetDevCapsA) { RET(2, 3); }
IMPL(winmm, mmioSeek) { RET(0xffffffffu, 3); }

/* ---- DirectPlay: DirectPlayCreate (1) and DirectPlayEnumerateA (4) are probed, not used for play ---- */
IMPL(dplayx, ord1) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(0x80004005u, 3); }   /* (guid, out, outer): DPERR_GENERIC */
IMPL(dplayx, ord4) { RET(0, 2); }                                  /* (callback, context): no service providers */

/* ---- wsock32: gethostbyname (52 in net_ole.c), inet_ntoa (11) ... 51 is gethostbyaddr ---- */
IMPL(wsock32, ord51) { w32_set_last_error(c, 11004); RET(0, 3); }  /* WSANO_DATA */

/* ---- Netcomm.dll (Delta Force 2): NovaWorld voice chat, not available ---- */
IMPL(netcomm, netcomm_ServerInit2) { RETC(0); }
IMPL(netcomm, netcomm_ClientInit2) { RETC(0); }
IMPL(netcomm, netcomm_PlaybackInit) { RETC(0); }
IMPL(netcomm, netcomm_Shutdown) { RETC(0); }
IMPL(netcomm, netcomm_PlaybackShutdown) { RETC(0); }
IMPL(netcomm, netcomm_AddPlayer) { RETC(0); }
IMPL(netcomm, netcomm_RemovePlayer) { RETC(0); }
IMPL(netcomm, netcomm_Break) { RETC(0); }
IMPL(netcomm, netcomm_EnableFullDuplex) { RETC(0); }
IMPL(netcomm, netcomm_EnableVoxRecording) { RETC(0); }
IMPL(netcomm, netcomm_MutePlayers) { RETC(0); }
IMPL(netcomm, netcomm_UnMutePlayers) { RETC(0); }
IMPL(netcomm, netcomm_PlayerSendAudio) { RETC(0); }
IMPL(netcomm, netcomm_VoxPlayerSendAudio) { RETC(0); }
IMPL(netcomm, netcomm_RegisterClient) { RETC(0); }
IMPL(netcomm, netcomm_RegisterServer) { RETC(0); }
IMPL(netcomm, netcomm_RegisterMessageCallback) { RETC(0); }
IMPL(netcomm, netcomm_RegisterVuCallback) { RETC(0); }
IMPL(netcomm, netcomm_SelectMic) { RETC(0); }
IMPL(netcomm, netcomm_SetAudioGain) { RETC(0); }
IMPL(netcomm, netcomm_SetNovaworldPort) { RETC(0); }
IMPL(netcomm, netcomm_SetPlaybackVolume) { RETC(0); }
IMPL(netcomm, netcomm_SetVoxThreshold) { RETC(0); }
IMPL(netcomm, netcomm_StartMeteredRecording) { RETC(0); }
IMPL(netcomm, netcomm_StopAudio) { RETC(0); }
IMPL(netcomm, netcomm_VoxStopAudio) { RETC(0); }

/* ---- iBrowse.dll (Delta Force 2): NovaWorld's in-game web pages, not available ---- */
IMPL(ibrowse, ibrowse_Init) { RETC(0); }
IMPL(ibrowse, ibrowse_SetCallback) { RETC(0); }
IMPL(ibrowse, ibrowse_OpenURL) { RETC(0); }
IMPL(ibrowse, ibrowse_Render) { RETC(0); }
IMPL(ibrowse, ibrowse_Invalidate) { RETC(0); }
IMPL(ibrowse, ibrowse_GetTitle) { RETC(0); }
IMPL(ibrowse, ibrowse_GetMetaValue) { RETC(0); }
IMPL(ibrowse, ibrowse_GetCookieValue) { RETC(0); }
IMPL(ibrowse, ibrowse_OnChar) { RETC(0); }
IMPL(ibrowse, ibrowse_OnKeyDown) { RETC(0); }
IMPL(ibrowse, ibrowse_OnKeyUp) { RETC(0); }
IMPL(ibrowse, ibrowse_OnLMouseDown) { RETC(0); }
IMPL(ibrowse, ibrowse_OnLMouseUp) { RETC(0); }
IMPL(ibrowse, ibrowse_OnRMouseDown) { RETC(0); }
IMPL(ibrowse, ibrowse_OnRMouseUp) { RETC(0); }
IMPL(ibrowse, ibrowse_OnMouseMove) { RETC(0); }

/* ---- msacm32 (Delta Force 2): compressed WAVE data decoded to PCM — IMA ADPCM (0x11) and Microsoft ADPCM (2) ---- */
typedef struct AcmStream { uint16_t tag, ch, align, spb, bits_out; int16_t coef[32][2]; int ncoef; } AcmStream;
static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767 };
static const int ima_idx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
/* one block: samples per channel = spb; out interleaved 16-bit */
static uint32_t ima_block(const AcmStream *s, const uint8_t *in, int16_t *out)
{
    int pred[2], idx[2], ch = s->ch;
    for (int c = 0; c < ch; c++) { pred[c] = (int16_t)(in[4 * c] | in[4 * c + 1] << 8); idx[c] = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2]; out[c] = (int16_t)pred[c]; }
    const uint8_t *p = in + 4 * ch; uint32_t n = 1;
    while (n < s->spb) {                                    /* 8 samples per channel per 4-byte group, channels interleaved by group */
        for (int c = 0; c < ch; c++) for (int k = 0; k < 8 && n + (uint32_t)k < s->spb; k++) {
            int nib = (p[4 * c + k / 2] >> ((k & 1) * 4)) & 15, step = ima_step[idx[c]], d = step >> 3;
            if (nib & 1) d += step >> 2; if (nib & 2) d += step >> 1; if (nib & 4) d += step; if (nib & 8) d = -d;
            pred[c] = clamp16(pred[c] + d); idx[c] += ima_idx[nib]; if (idx[c] < 0) idx[c] = 0; if (idx[c] > 88) idx[c] = 88;
            out[(n + (uint32_t)k) * (uint32_t)ch + (uint32_t)c] = (int16_t)pred[c];
        }
        p += 4 * ch; n += 8;
    }
    return s->spb;
}
static const int ms_adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static uint32_t ms_block(const AcmStream *s, const uint8_t *in, int16_t *out)
{
    int ch = s->ch, pred[2], delta[2], s1[2], s2[2]; const uint8_t *p = in;
    for (int c = 0; c < ch; c++) pred[c] = p[c] < s->ncoef ? p[c] : 0; p += ch;
    for (int c = 0; c < ch; c++) { delta[c] = (int16_t)(p[0] | p[1] << 8); p += 2; }
    for (int c = 0; c < ch; c++) { s1[c] = (int16_t)(p[0] | p[1] << 8); p += 2; }
    for (int c = 0; c < ch; c++) { s2[c] = (int16_t)(p[0] | p[1] << 8); p += 2; }
    for (int c = 0; c < ch; c++) { out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    uint32_t total = (uint32_t)(s->spb - 2) * (uint32_t)ch;
    for (uint32_t k = 0; k < total; k++) {
        int c = (int)(k % (uint32_t)ch), nib = (p[k / 2] >> ((k & 1) ? 0 : 4)) & 15, sn = nib & 8 ? nib - 16 : nib;
        int v = (s1[c] * s->coef[pred[c]][0] + s2[c] * s->coef[pred[c]][1]) / 256 + sn * delta[c];
        int16_t o = clamp16(v); s2[c] = s1[c]; s1[c] = o; out[2 * ch + k] = o;
        delta[c] = ms_adapt[nib] * delta[c] / 256; if (delta[c] < 16) delta[c] = 16;
    }
    return s->spb;
}
static AcmStream *acm_of(uint32_t h) { return h ? *(AcmStream **)GP(h) : 0; }
IMPL(msacm32, acmStreamOpen)       /* (has *, driver, src fmt, dst fmt, filter, callback, instance, flags) */
{
    uint32_t src = ARG(2), dst = ARG(3);
    AcmStream *s = calloc(1, sizeof *s);
    s->tag = rt_r16(G_MEM, src); s->ch = rt_r16(G_MEM, src + 2); s->align = rt_r16(G_MEM, src + 12);
    s->bits_out = rt_r16(G_MEM, dst + 14);
    uint16_t cb = s->tag == 1 ? 0 : rt_r16(G_MEM, src + 16);
    if (cb >= 2) s->spb = rt_r16(G_MEM, src + 18);
    if (s->tag == 2 && cb >= 4) { s->ncoef = rt_r16(G_MEM, src + 20); if (s->ncoef > 32) s->ncoef = 32;
        for (int k = 0; k < s->ncoef; k++) { s->coef[k][0] = (int16_t)rt_r16(G_MEM, src + 22 + 4 * k); s->coef[k][1] = (int16_t)rt_r16(G_MEM, src + 24 + 4 * k); } }
    if (s->tag == 0x11 && !s->spb) s->spb = (uint16_t)((s->align - 4 * s->ch) * 8 / (4 * s->ch) + 1);
    if (s->tag == 2 && !s->spb) s->spb = (uint16_t)((s->align - 7 * s->ch) * 8 / (4 * s->ch) + 2);
    fprintf(stderr, "w32: acmStreamOpen format %#x, %u ch, block %u (%u samples) -> PCM %u-bit %u ch\n", s->tag, s->ch, s->align, s->spb,
            s->bits_out, rt_r16(G_MEM, dst + 2));
    if ((s->tag != 0x11 && s->tag != 2) || s->ch < 1 || s->ch > 2 || s->bits_out != 16 || !s->align || s->spb < 2) { free(s); RET(0x200 /* ACMERR_NOTPOSSIBLE */, 8); }
    if (ARG(7) & 1) { free(s); RET(0, 8); }             /* ACM_STREAMOPENF_QUERY */
    uint32_t h = heap_alloc(w32_process_heap, 8, 8); memcpy(GP(h), &s, sizeof s);
    rt_w32(G_MEM, ARG(0), h); RET(0, 8);
}
IMPL(msacm32, acmStreamClose) { AcmStream *s = acm_of(ARG(0)); free(s); if (ARG(0)) heap_free(w32_process_heap, ARG(0)); RET(0, 2); }
IMPL(msacm32, acmStreamSize)       /* (has, input bytes, out *, flags: 0 source -> dest size, 1 dest -> source) */
{
    AcmStream *s = acm_of(ARG(0)); if (!s) RET(5, 4);
    uint32_t n = ARG(1), per = (uint32_t)s->spb * s->ch * 2;
    rt_w32(G_MEM, ARG(2), (ARG(3) & 0xf) == 1 ? n / per * s->align : n / s->align * per); RET(0, 4);
}
IMPL(msacm32, acmStreamPrepareHeader) { rt_w32(G_MEM, ARG(1) + 4, rt_r32(G_MEM, ARG(1) + 4) | 0x20000); RET(0, 3); }   /* ACMSTREAMHEADER_STATUSF_PREPARED */
IMPL(msacm32, acmStreamUnprepareHeader) { rt_w32(G_MEM, ARG(1) + 4, rt_r32(G_MEM, ARG(1) + 4) & ~0x20000u); RET(0, 3); }
IMPL(msacm32, acmStreamConvert)    /* (has, ACMSTREAMHEADER *, flags): whole blocks of source into PCM */
{
    AcmStream *s = acm_of(ARG(0)); uint32_t h = ARG(1); if (!s) RET(5, 3);
    uint32_t src = rt_r32(G_MEM, h + 12), slen = rt_r32(G_MEM, h + 16), dst = rt_r32(G_MEM, h + 28), dlen = rt_r32(G_MEM, h + 32);
    uint32_t per = (uint32_t)s->spb * s->ch * 2, used = 0, out = 0;
    while (used + s->align <= slen && out + per <= dlen) {
        (s->tag == 0x11 ? ima_block : ms_block)(s, G_MEM + src + used, (int16_t *)GP(dst + out));
        used += s->align; out += per;
    }
    rt_w32(G_MEM, h + 20, used); rt_w32(G_MEM, h + 36, out);
    rt_w32(G_MEM, h + 4, rt_r32(G_MEM, h + 4) | 0x10000);   /* ACMSTREAMHEADER_STATUSF_DONE */
    RET(0, 3);
}

/* ---- W32_SMCLOG=1: code bytes the program rewrote, checked at every presented frame (each changed byte reported once,
 * with the value) — to compare against the fields tools/lift.py reads at run time (work/<game>/smc.json) ---- */
uint8_t *w32_code_copy[8]; uint32_t w32_code_va[8], w32_code_n[8]; int w32_code_k;
void w32_smc_check(void)
{
    static uint8_t *seen[8];
    for (int k = 0; k < w32_code_k; k++) {
        if (!seen[k]) seen[k] = calloc(w32_code_n[k], 1);
        const uint8_t *now = G_MEM + w32_code_va[k];
        for (uint32_t o = 0; o < w32_code_n[k]; o++)
            if (now[o] != w32_code_copy[k][o] && !seen[k][o]) { seen[k][o] = 1; fprintf(stderr, "smc: %08x %02x -> %02x\n", w32_code_va[k] + o, w32_code_copy[k][o], now[o]); }
    }
}
