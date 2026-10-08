/* Audio output only. Miles/DirectSound mixing remains in the shared runtime. */
#include "host_audio.h"
#include <stdlib.h>
#include <stdio.h>
#ifdef __linux__
#include <SDL2/SDL.h>
struct DsAudio { SDL_AudioDeviceID device; DsAudioRender render; void *user; int frame_bytes; };
static void callback(void *u, Uint8 *data, int len) {
    DsAudio *a = u; a->render(a->user, data, (uint32_t)(len / a->frame_bytes));
}
DsAudio *ds_audio_open(int rate, int channels, int bits, int floating, DsAudioRender render, void *user) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return NULL; }
    DsAudio *a = calloc(1, sizeof *a); if (!a) return NULL;
    a->render = render; a->user = user; a->frame_bytes = channels * bits / 8;
    SDL_AudioSpec spec = {0}; spec.freq = rate; spec.channels = channels; spec.samples = 512;
    spec.format = floating ? AUDIO_F32SYS : bits == 16 ? AUDIO_S16SYS : AUDIO_U8;
    spec.callback = callback; spec.userdata = a;
    a->device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (!a->device) { fprintf(stderr, "audio: %s\n", SDL_GetError()); free(a); return NULL; }
    SDL_PauseAudioDevice(a->device, 0); return a;
}
void ds_audio_stop(DsAudio *a) { if (a) SDL_PauseAudioDevice(a->device, 1); }
void ds_audio_close(DsAudio *a) { if (a) { SDL_CloseAudioDevice(a->device); free(a); } }
#else
#include <AudioToolbox/AudioToolbox.h>
struct DsAudio { AudioComponentInstance unit; DsAudioRender render; void *user; };
static OSStatus callback(void *u, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)fl; (void)ts; (void)bus; DsAudio *a = u; a->render(a->user, io->mBuffers[0].mData, frames); return 0;
}
DsAudio *ds_audio_open(int rate, int channels, int bits, int floating, DsAudioRender render, void *user) {
    DsAudio *a = calloc(1, sizeof *a); if (!a) return NULL; a->render = render; a->user = user;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(0, &d);
    if (!comp || AudioComponentInstanceNew(comp, &a->unit)) { free(a); return NULL; }
    UInt32 bytes = channels * bits / 8;
    AudioStreamBasicDescription f = {rate, kAudioFormatLinearPCM, kAudioFormatFlagIsPacked |
        (floating ? kAudioFormatFlagIsFloat : bits == 16 ? kAudioFormatFlagIsSignedInteger : 0), bytes, 1, bytes, channels, bits, 0};
    AudioUnitSetProperty(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    UInt32 maxf = 4096; AudioUnitSetProperty(a->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, sizeof maxf);
    AURenderCallbackStruct cb = {callback, a};
    AudioUnitSetProperty(a->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(a->unit) || AudioOutputUnitStart(a->unit)) { ds_audio_close(a); return NULL; }
    return a;
}
void ds_audio_stop(DsAudio *a) { if (a) AudioOutputUnitStop(a->unit); }
void ds_audio_close(DsAudio *a) { if (a) { ds_audio_stop(a); AudioComponentInstanceDispose(a->unit); free(a); } }
#endif
