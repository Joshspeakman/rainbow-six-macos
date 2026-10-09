/* Imports Rainbow Six uses that the other ports never needed. Voice capture (DirectSoundCapture) reports no device,
 * so the game's voice chat stays off as on a PC without a microphone; the rest are the small USER32/GDI32/KERNEL32
 * calls SMACKW32.DLL imports. */
#include "w32.h"

/* ---- dsound: capture (the voice-over-IP microphone) ---- */
IMPL(dsound, DirectSoundCaptureCreate) { RET(0x88780078u, 3); }               /* (guid, out, outer): DSERR_NODRIVER */
IMPL(dsound, DirectSoundCaptureEnumerateA) { RET(0, 2); }                     /* (callback, ctx): no capture devices */

/* ---- winmm: writing RIFF files (the game never needs it to play) ---- */
IMPL(winmm, mmioCreateChunk) { RET(0x10b, 3); }                               /* MMIOERR_CANNOTWRITE */
IMPL(winmm, mmioWrite) { RET(0xffffffffu, 3); }

/* ---- the calls SMACKW32.DLL imports ---- */
IMPL(gdi32, GetSystemPaletteUse) { RET(1, 1); }                               /* SYSPAL_STATIC */
IMPL(gdi32, PatBlt) { RET(1, 6); }
IMPL(kernel32, GetProfileIntA) { RET(ARG(2), 3); }                            /* (section, key, default) */
IMPL(kernel32, LoadModule) { RET(2, 2); }                                     /* ERROR_FILE_NOT_FOUND (< 32) */
IMPL(user32, IsChild) { RET(0, 2); }
IMPL(user32, LoadImageA)                                                      /* (hinst, name, type, cx, cy, flags): the window class's small icon */
{
    uint32_t id = ARG(1) & 0xffff, type = ARG(2);
    RET(type == 1 ? 0x31000 + id : type == 2 ? 0x30000 + id : 0, 6);          /* IMAGE_ICON / IMAGE_CURSOR as LoadIcon/LoadCursor; no bitmaps */
}

/* ---- vct32150.dll (Voxware voice codecs, for in-game voice chat): "the engine is there, no codec installed", which
 * is what the original reports under Wine too ("Could not find an appropriate Voxware CODEC"); the game then plays
 * without voice. Ordinal 100 is the engine's init (handle out, info: version at +0x18, codec count at +0x20). ---- */
IMPL(vct32150, ord100)
{
    uint32_t h = ARG(0), info = ARG(1);
    if (h) rt_w32(G_MEM, h, 1);
    if (info) { rt_w32(G_MEM, info + 0x18, 0x10005); rt_w16(G_MEM, info + 0x20, 0); }
    RET(0, 2);
}
IMPL(vct32150, ord101) { RET(0, 1); }                                         /* shut down */
IMPL(vct32150, ord200) { RET(1, 3); }                                         /* codec info by index: none */
IMPL(vct32150, ord1100) { RET(1, 1); }
IMPL(vct32150, ord1110) { RET(1, 1); }
IMPL(vct32150, ord1120) { RET(1, 1); }
IMPL(vct32150, ord2100) { RET(1, 1); }
IMPL(vct32150, ord2110) { RET(1, 1); }
IMPL(vct32150, ord2120) { RET(1, 1); }
