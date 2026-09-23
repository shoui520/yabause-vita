/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "threads.h"
#include "telemetry.h"
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <errno.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include "cpu_affinity.h"

_Static_assert(SCE_KERNEL_CPU_MASK_USER_0 == (1 << 16) &&
               SCE_KERNEL_CPU_MASK_USER_1 == (2 << 16) &&
               SCE_KERNEL_CPU_MASK_USER_2 == (4 << 16), "Vita CPU mask encoding");

extern void YuiMsg(const char *, ...);
static void thread_failure(const char *operation, int error) {
  YuiMsg("thread_failure operation=%s error=%d", operation, error);
  /* Preserve a diagnosable dump instead of silently spinning or losing a wake. */
  __builtin_trap();
}

typedef struct { pthread_t thread; sem_t wake; int active; } Thread;
static Thread threads[YAB_NUM_THREADS];
static _Thread_local int current = -1;
typedef struct { unsigned id; void *(*fn)(void *); void *arg; } Start;
static void *entry(void *p) {
  Start s = *(Start *)p; free(p); current = s.id;
  YabThreadSetCurrentThreadAffinityMask(0);
  char role[32]; snprintf(role, sizeof(role), "worker_%u", s.id);
  if (s.id == YAB_THREAD_SCSP) snprintf(role, sizeof(role), "sound");
  if (s.id == YAB_THREAD_VDP) snprintf(role, sizeof(role), "vdp");
  if (s.id == YAB_THREAD_VIDSOFT_LAYER_RBG0) snprintf(role, sizeof(role), "rotation");
  VitaTelemetryThread(role);
  void *result = s.fn(s.arg);
  VitaTelemetryReport();
  return result;
}
int YabThreadStart(unsigned id, void *(*fn)(void *), void *arg) {
  if (id >= YAB_NUM_THREADS || threads[id].active) return -1;
  Start *s = malloc(sizeof(*s)); if (!s) return -1;
  *s = (Start){id, fn, arg};
  if (sem_init(&threads[id].wake, 0, 0)) { free(s); return -1; }
  /* The child may sleep or wake immediately, before pthread_create returns.
   * Lifecycle start/join is owned by the emulation thread. */
  threads[id].active = 1;
  if (pthread_create(&threads[id].thread, NULL, entry, s)) {
    threads[id].active = 0; sem_destroy(&threads[id].wake); free(s); return -1;
  }
  return 0;
}
void YabThreadWait(unsigned id) {
  if (id >= YAB_NUM_THREADS || !threads[id].active) return;
  VitaTelemetryEnter(VT_THREAD_JOIN);
  pthread_join(threads[id].thread, NULL);
  VitaTelemetryLeave(VT_THREAD_JOIN);
  sem_destroy(&threads[id].wake);
  threads[id].active = 0;
}
void YabThreadYield(void) { YabThreadUSleep(1); }
void YabThreadUSleep(unsigned us) {
  VitaTelemetryEnter(VT_THREAD_SLEEP);
  sceKernelDelayThread(us ? us : 1);
  VitaTelemetryLeave(VT_THREAD_SLEEP);
  VitaTelemetryReport();
}
void YabThreadSleep(void) {
  if (current < 0) {
    thread_failure("sleep-outside-worker", EINVAL);
    return;
  }
  int result;
  VitaTelemetryEnter(VT_THREAD_SLEEP);
  do { result = sem_wait(&threads[current].wake); } while (result && errno == EINTR);
  VitaTelemetryLeave(VT_THREAD_SLEEP);
  VitaTelemetryReport();
  if (result) thread_failure("sem_wait", errno);
}
void YabThreadRemoteSleep(unsigned id) { if ((int)id == current) YabThreadSleep(); }
void YabThreadWake(unsigned id) {
  if (id < YAB_NUM_THREADS && threads[id].active && sem_post(&threads[id].wake))
    thread_failure("sem_post", errno);
}
YabMutex *YabThreadCreateMutex(void) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  if (m && pthread_mutex_init(m, NULL)) { free(m); m = NULL; }
  return (YabMutex *)m;
}
void YabThreadFreeMutex(YabMutex *m) { if(m) { pthread_mutex_destroy((pthread_mutex_t *)m); free(m); } }
void YabThreadLock(YabMutex *m) {
  if(m) {
    VitaTelemetryEnter(VT_MUTEX_WAIT);
    pthread_mutex_lock((pthread_mutex_t *)m);
    VitaTelemetryLeave(VT_MUTEX_WAIT);
  }
}
void YabThreadUnLock(YabMutex *m) { if(m) pthread_mutex_unlock((pthread_mutex_t *)m); }
void YabThreadSetCurrentThreadAffinityMask(int mask) {
  static _Thread_local int last_mask = -1, last_result = -1;
  mask = VitaAffinityForRole(current, mask);
  // Legacy render loops request affinity every iteration. Once confirmed,
  // avoid a kernel call for the same fixed policy on every iteration.
  if (last_mask == mask && last_result == 0) return;
  const int native = VitaAffinityEncode(mask);
  const int result = native < 0 ? -EINVAL : sceKernelChangeThreadCpuAffinityMask(0, native);
  // Affinity is a scheduling policy, not guest semantics: report a failure
  // and retain the OS policy instead of aborting emulation or claiming success.
  if (last_mask != mask || last_result != result) {
    const int observed = sceKernelGetThreadCpuAffinityMask(0);
    YuiMsg("thread_affinity role=%d logical=%x native=%x result=%08x observed=%08x",
           current, mask, native, result, observed);
    last_mask = mask;
    // Success can return the previous mask, not just zero.
    last_result = result < 0 ? result :
      (native != 0 && observed != native ? -EINVAL : 0);
  }
}
int YabThreadGetCurrentThreadAffinityMask(void) {
  return VitaAffinityDecode(sceKernelGetThreadCpuAffinityMask(0));
}

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t readable, writable;
  int capacity, size, head, tail;
  int data[];
} Queue;
YabEventQueue *YabThreadCreateQueue(int n) {
  if(n <= 0 || n > 65536) return NULL;
  Queue *q = calloc(1, sizeof(*q) + n * sizeof(int)); if (!q) return NULL;
  q->capacity = n;
  if (pthread_mutex_init(&q->lock, NULL)) { free(q); return NULL; }
  if (pthread_cond_init(&q->readable, NULL)) { pthread_mutex_destroy(&q->lock); free(q); return NULL; }
  if (pthread_cond_init(&q->writable, NULL)) {
    pthread_cond_destroy(&q->readable); pthread_mutex_destroy(&q->lock); free(q); return NULL;
  }
  return (YabEventQueue *)q;
}
void YabThreadDestoryQueue(YabEventQueue *p) {
  Queue *q=(Queue *)p; if(!q) return;
  pthread_cond_destroy(&q->readable); pthread_cond_destroy(&q->writable);
  pthread_mutex_destroy(&q->lock); free(q);
}
void YabAddEventQueue(YabEventQueue *p,int value) {
  Queue *q=(Queue *)p;
  VitaTelemetryEnter(VT_MUTEX_WAIT);
  pthread_mutex_lock(&q->lock);
  VitaTelemetryLeave(VT_MUTEX_WAIT);
  while(q->size == q->capacity) {
    VitaTelemetryEnter(VT_QUEUE_WAIT);
    pthread_cond_wait(&q->writable,&q->lock);
    VitaTelemetryLeave(VT_QUEUE_WAIT);
  }
  q->data[q->tail]=value; q->tail=(q->tail+1)%q->capacity; ++q->size;
  pthread_cond_signal(&q->readable); pthread_mutex_unlock(&q->lock);
}
int YabWaitEventQueue(YabEventQueue *p) {
  Queue *q=(Queue *)p;
  VitaTelemetryEnter(VT_MUTEX_WAIT);
  pthread_mutex_lock(&q->lock);
  VitaTelemetryLeave(VT_MUTEX_WAIT);
  while(!q->size) {
    VitaTelemetryEnter(VT_QUEUE_WAIT);
    pthread_cond_wait(&q->readable,&q->lock);
    VitaTelemetryLeave(VT_QUEUE_WAIT);
  }
  int value=q->data[q->head]; q->head=(q->head+1)%q->capacity; --q->size;
  pthread_cond_signal(&q->writable); pthread_mutex_unlock(&q->lock);
  // Report only after releasing the queue lock, never from inside accounting.
  VitaTelemetryReport();
  return value;
}
int YaGetQueueSize(YabEventQueue *p) {
  Queue *q=(Queue *)p;
  { VT_SCOPE(VT_MUTEX_WAIT); pthread_mutex_lock(&q->lock); }
  int n=q->size;
  pthread_mutex_unlock(&q->lock); return n;
}
int YabClearEventQueue(YabEventQueue *p) {
  Queue *q=(Queue *)p;
  { VT_SCOPE(VT_MUTEX_WAIT); pthread_mutex_lock(&q->lock); }
  q->size=q->head=q->tail=0; pthread_cond_broadcast(&q->writable);
  pthread_mutex_unlock(&q->lock); return 0;
}
