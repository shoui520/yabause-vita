/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_AUDIO_PCM_H
#define VITA_AUDIO_PCM_H
#include <stdint.h>
enum { VITA_AUDIO_FRAMES=512 };
typedef struct {
  int16_t samples[2][VITA_AUDIO_FRAMES*2];
  unsigned side,used;
} VitaAudioPcm;
/* submit returns when this buffer starts playing, not when it has finished.
 * The preceding buffer is then reusable, so alternate AFTER submission.
 * Only the sound producer owns side/used. Reset drops partial input but must
 * not reset side while a submitted buffer can still be playing. */
static inline void VitaAudioPcmPush(VitaAudioPcm *s,int32_t *left,int32_t *right,
    uint32_t n,void (*convert)(int32_t *,int32_t *,int16_t *,uint32_t),
    void (*submit)(const int16_t *)) {
  while(n) {
    unsigned take=n<VITA_AUDIO_FRAMES-s->used?n:VITA_AUDIO_FRAMES-s->used;
    convert(left,right,s->samples[s->side]+s->used*2,take);
    s->used+=take; left+=take; right+=take; n-=take;
    if(s->used==VITA_AUDIO_FRAMES) {
      submit(s->samples[s->side]);
      s->side^=1;
      s->used=0;
    }
  }
}
#endif
