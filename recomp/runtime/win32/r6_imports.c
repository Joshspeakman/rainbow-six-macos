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
