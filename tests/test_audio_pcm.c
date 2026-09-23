/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/vita/audio_pcm.h"
static const int16_t *playing;
static int16_t saved[VITA_AUDIO_FRAMES*2];
static unsigned outputs,expected;
static void check_playing(void) {
  if(playing) assert(!memcmp(playing,saved,sizeof(saved)));
}
static void convert(int32_t *l,int32_t *r,int16_t *out,uint32_t n) {
  check_playing();
  for(unsigned i=0;i<n;++i) { out[i*2]=l[i]; out[i*2+1]=r[i]; }
  check_playing(); /* No overwrite even during partial fill before next output. */
}
static void submit(const int16_t *samples) {
  check_playing();
  assert(samples!=playing);
  for(unsigned i=0;i<VITA_AUDIO_FRAMES;++i,++expected) {
    assert(samples[2*i]==(int16_t)(expected%30000));
    assert(samples[2*i+1]==(int16_t)-(int)(expected%30000));
  }
  playing=samples; memcpy(saved,samples,sizeof(saved)); ++outputs;
}
int main(void) {
  VitaAudioPcm pcm={0};
  int32_t l[2048],r[2048];
  unsigned produced=0;
  const unsigned chunks[]={0,1,511,512,735,1024,2048,17,493};
  for(unsigned repeat=0;repeat<50;++repeat) {
    for(unsigned c=0;c<sizeof(chunks)/sizeof(*chunks);++c) {
      unsigned n=chunks[c];
      for(unsigned i=0;i<n;++i) { l[i]=(produced+i)%30000; r[i]=-l[i]; }
      VitaAudioPcmPush(&pcm,l,r,n,convert,submit);
      produced+=n; check_playing();
      assert(expected==produced-produced%VITA_AUDIO_FRAMES);
    }
  }
  /* Reset must retain the safe write side, including a partial next block. */
  unsigned side=pcm.side;
  pcm.used=0;
  assert(pcm.side==side && pcm.samples[pcm.side]!=playing);
  check_playing();
  for(unsigned i=0;i<512;++i) { l[i]=(expected+i)%30000; r[i]=-l[i]; }
  VitaAudioPcmPush(&pcm,l,r,512,convert,submit);
  check_playing();
  printf("audio PCM: %u packets, exact order, in-flight ownership and partial fills passed\n",outputs);
}
