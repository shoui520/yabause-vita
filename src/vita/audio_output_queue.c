/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio_output_queue.h"
#include <errno.h>
#include <string.h>
static void event(VitaAudioOutputQueue *q,int e) { if(q->event) q->event(q->context,e); }
static void wait_on(VitaAudioOutputQueue *q,pthread_cond_t *cond) {
  event(q,VAQ_WAIT_BEGIN);
  int rc=pthread_cond_wait(cond,&q->mutex);
  event(q,VAQ_WAIT_END);
  if(rc && !q->error) q->error=-rc;
}
static void *worker(void *arg) {
  VitaAudioOutputQueue *q=arg;
  unsigned side=0;
  event(q,VAQ_START);
  pthread_mutex_lock(&q->mutex);
  for(;;) {
    while(!q->count && !q->stopping && !q->error) wait_on(q,&q->ready);
    if(q->error || (!q->count && q->stopping)) break;
    int drain=q->slots[q->read].drain;
    if(!drain) memcpy(q->playback[side],q->slots[q->read].pcm,sizeof(q->playback[side]));
    q->read=(q->read+1)%VITA_AUDIO_QUEUE_CAPACITY; --q->count;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->mutex);
    /* Keep previous playback storage intact until the next output starts. */
    int rc=q->output(q->context,drain?NULL:q->playback[side]);
    if(!drain) side^=1;
    pthread_mutex_lock(&q->mutex);
    if(rc<0) q->error=rc;
    ++q->completed;
    pthread_cond_broadcast(&q->changed);
  }
  pthread_cond_broadcast(&q->changed);
  pthread_mutex_unlock(&q->mutex);
  int rc=q->output(q->context,NULL);
  pthread_mutex_lock(&q->mutex);
  if(rc<0 && !q->error) q->error=rc;
  pthread_cond_broadcast(&q->changed);
  pthread_mutex_unlock(&q->mutex);
  event(q,VAQ_STOP);
  return NULL;
}
int VitaAudioQueueOpen(VitaAudioOutputQueue *q,void *ctx,
    int (*output)(void *,const int16_t *),void (*hook)(void *,int)) {
  memset(q,0,sizeof(*q)); q->context=ctx; q->output=output; q->event=hook;
  int rc=pthread_mutex_init(&q->mutex,NULL); if(rc) return -rc;
  rc=pthread_cond_init(&q->ready,NULL); if(rc) goto mutex_fail;
  rc=pthread_cond_init(&q->changed,NULL); if(rc) goto ready_fail;
  rc=pthread_create(&q->thread,NULL,worker,q); if(!rc) return 0;
  pthread_cond_destroy(&q->changed);
ready_fail: pthread_cond_destroy(&q->ready);
mutex_fail: pthread_mutex_destroy(&q->mutex); return -rc;
}
static int enqueue(VitaAudioOutputQueue *q,const int16_t *pcm,int drain) {
  pthread_mutex_lock(&q->mutex);
  while(q->count==VITA_AUDIO_QUEUE_CAPACITY && !q->error && !q->stopping)
    wait_on(q,&q->changed);
  if(q->error || q->stopping) {
    int rc=q->error?q->error:-ECANCELED;
    pthread_mutex_unlock(&q->mutex); return rc;
  }
  q->slots[q->write].drain=drain;
  if(!drain) memcpy(q->slots[q->write].pcm,pcm,sizeof(q->slots[q->write].pcm));
  q->write=(q->write+1)%VITA_AUDIO_QUEUE_CAPACITY; ++q->count;
  unsigned long long ticket=++q->submitted;
  pthread_cond_signal(&q->ready);
  if(drain) while(q->completed<ticket && !q->error) wait_on(q,&q->changed);
  int rc=q->error;
  pthread_mutex_unlock(&q->mutex);
  return rc;
}
int VitaAudioQueuePush(VitaAudioOutputQueue *q,const int16_t *pcm) { return enqueue(q,pcm,0); }
int VitaAudioQueueDrain(VitaAudioOutputQueue *q) { return enqueue(q,NULL,1); }
int VitaAudioQueueClose(VitaAudioOutputQueue *q) {
  pthread_mutex_lock(&q->mutex); q->stopping=1;
  pthread_cond_broadcast(&q->ready); pthread_cond_broadcast(&q->changed);
  pthread_mutex_unlock(&q->mutex);
  int rc=pthread_join(q->thread,NULL); if(rc) return -rc;
  rc=q->error;
  pthread_cond_destroy(&q->changed); pthread_cond_destroy(&q->ready);
  pthread_mutex_destroy(&q->mutex);
  return rc;
}
