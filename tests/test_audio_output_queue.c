/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/vita/audio_output_queue.h"
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  pthread_t worker;
  int started,release,producer_waited,fail;
  unsigned packets,drains;
  const int16_t *playing;
  int16_t saved[VITA_AUDIO_FRAMES*2];
} Device;
static void fill(int16_t *p,unsigned packet) {
  for(unsigned i=0;i<VITA_AUDIO_FRAMES*2;++i) p[i]=(packet*1024+i)%30000;
}
static int output(void *arg,const int16_t *pcm) {
  Device *d=arg;
  pthread_mutex_lock(&d->mutex);
  if(!d->started) {
    d->worker=pthread_self(); d->started=1;
    pthread_cond_broadcast(&d->condition);
    while(!d->release) pthread_cond_wait(&d->condition,&d->mutex);
  }
  if(d->playing) assert(!memcmp(d->playing,d->saved,sizeof(d->saved)));
  if(pcm && d->fail) {
    pthread_mutex_unlock(&d->mutex);
    return -77;
  }
  if(pcm) {
    assert(pcm!=d->playing);
    int16_t expected[VITA_AUDIO_FRAMES*2]; fill(expected,d->packets++);
    assert(!memcmp(pcm,expected,sizeof(expected)));
    memcpy(d->saved,pcm,sizeof(d->saved)); d->playing=pcm;
  } else { ++d->drains; d->playing=NULL; }
  pthread_mutex_unlock(&d->mutex);
  return 0;
}
static void hook(void *arg,int e) {
  Device *d=arg;
  if(e!=VAQ_WAIT_BEGIN) return;
  pthread_mutex_lock(&d->mutex);
  if(d->started && !pthread_equal(pthread_self(),d->worker)) {
    d->producer_waited=1; pthread_cond_broadcast(&d->condition);
  }
  pthread_mutex_unlock(&d->mutex);
}
static void *blocked_push(void *arg) {
  VitaAudioOutputQueue *q=arg;
  const Device *d=q->context;
  int16_t data[VITA_AUDIO_FRAMES*2]; fill(data,3);
  assert(VitaAudioQueuePush(q,data)==(d->fail?-77:0));
  memset(data,0,sizeof(data));
  return NULL;
}
static int fail_output(void *arg,const int16_t *pcm) { (void)arg; return pcm?-77:0; }
int main(void) {
  for(unsigned cycle=0;cycle<8;++cycle) {
    Device d={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER,
              .fail=cycle>=4};
    VitaAudioOutputQueue q;
    assert(VitaAudioQueueOpen(&q,&d,output,hook)==0);
    int16_t data[VITA_AUDIO_FRAMES*2]; fill(data,0);
    assert(VitaAudioQueuePush(&q,data)==0);
    pthread_mutex_lock(&d.mutex);
    while(!d.started) pthread_cond_wait(&d.condition,&d.mutex);
    pthread_mutex_unlock(&d.mutex);
    for(unsigned i=1;i<=2;++i) { fill(data,i); assert(VitaAudioQueuePush(&q,data)==0); }
    pthread_t producer; assert(pthread_create(&producer,NULL,blocked_push,&q)==0);
    pthread_mutex_lock(&d.mutex);
    while(!d.producer_waited) pthread_cond_wait(&d.condition,&d.mutex);
    d.release=1; pthread_cond_broadcast(&d.condition);
    pthread_mutex_unlock(&d.mutex);
    assert(pthread_join(producer,NULL)==0);
    if(d.fail) {
      /* Output fails only AFTER the fourth push blocks on the full queue.
       * The waiter must wake with that error; queued PCM must not be played. */
      assert(VitaAudioQueueDrain(&q)==-77);
      assert(VitaAudioQueueClose(&q)==-77);
      assert(q.submitted==3 && q.completed==1);
      assert(d.packets==0 && d.drains==1);
      pthread_cond_destroy(&d.condition); pthread_mutex_destroy(&d.mutex);
      continue;
    }
    for(unsigned i=4;i<1000;++i) {
      fill(data,i); assert(VitaAudioQueuePush(&q,data)==0);
      memset(data,0,sizeof(data)); /* Producer storage is immediately reusable. */
      if(i==500) assert(VitaAudioQueueDrain(&q)==0);
    }
    assert(VitaAudioQueueClose(&q)==0);
    assert(d.packets==1000 && d.drains==2 && q.submitted==q.completed);
    pthread_cond_destroy(&d.condition); pthread_mutex_destroy(&d.mutex);
  }
  VitaAudioOutputQueue q;
  int16_t pcm[VITA_AUDIO_FRAMES*2]={0};
  assert(VitaAudioQueueOpen(&q,NULL,fail_output,NULL)==0);
  (void)VitaAudioQueuePush(&q,pcm);
  assert(VitaAudioQueueDrain(&q)==-77);
  assert(VitaAudioQueuePush(&q,pcm)==-77);
  assert(VitaAudioQueueClose(&q)==-77);
  puts("audio queue: 4000 ordered packets, backpressure, retained buffers, drain/reopen and 4 blocked-producer failure wakeups passed");
}
