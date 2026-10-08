#ifndef DS_HOST_AUDIO_H
#define DS_HOST_AUDIO_H
#include <stdint.h>
typedef struct DsAudio DsAudio;
typedef void (*DsAudioRender)(void *user, void *samples, uint32_t frames);
DsAudio *ds_audio_open(int rate, int channels, int bits, int floating, DsAudioRender render, void *user);
void ds_audio_stop(DsAudio *audio);
void ds_audio_close(DsAudio *audio);
#endif
