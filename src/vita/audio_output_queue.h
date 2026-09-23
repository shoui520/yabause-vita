/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_AUDIO_OUTPUT_QUEUE_H
#define VITA_AUDIO_OUTPUT_QUEUE_H
#include <pthread.h>
#include "audio_pcm.h"
enum { VITA_AUDIO_QUEUE_CAPACITY=2 };
enum { VAQ_START,VAQ_STOP,VAQ_WAIT_BEGIN,VAQ_WAIT_END };
typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t ready,changed;
  struct { int16_t pcm[VITA_AUDIO_FRAMES*2]; int drain; } slots[VITA_AUDIO_QUEUE_CAPACITY];
  int16_t playback[2][VITA_AUDIO_FRAMES*2];
  unsigned read,write,count;
  unsigned long long submitted,completed;
  int stopping,error;
  void *context;
  int (*output)(void *,const int16_t *);
  void (*event)(void *,int);
} VitaAudioOutputQueue;
/* Lifecycle and producer calls are serialized by the frontend. Output and
 * drain callbacks belong only to the worker until Close joins it. */
int VitaAudioQueueOpen(VitaAudioOutputQueue *,void *,int (*)(void *,const int16_t *),void (*)(void *,int));
int VitaAudioQueuePush(VitaAudioOutputQueue *,const int16_t *);
int VitaAudioQueueDrain(VitaAudioOutputQueue *);
int VitaAudioQueueClose(VitaAudioOutputQueue *);
#endif
