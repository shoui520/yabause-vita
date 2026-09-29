/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Disc chunk read-ahead worker (VITA_DISC_PREFETCH, see disc_io.h). */
#include "disc_io.h"
#include <pthread.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif
#include "../core/threads.h"

enum { PF_IDLE, PF_BUSY, PF_READY };
static struct {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  int started, state;
  FILE *file;
  long base;
  size_t len;
  unsigned char data[VITA_DISC_CHUNK];
} pf = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *PrefetchMain(void *arg) {
  (void)arg;
  /* The sound worker spins on its core between VBlanks, so never share it;
   * the render core, at a priority above the render thread: this worker is
   * almost always blocked in the read and must run as soon as it completes. */
  YabThreadSetCurrentThreadAffinityMask(4);
#ifdef __vita__
  sceKernelChangeThreadPriority(0, 96);
#endif
  pthread_mutex_lock(&pf.lock);
  for (;;) {
    while (pf.state != PF_BUSY) pthread_cond_wait(&pf.cond, &pf.lock);
    FILE *const file = pf.file;
    const long base = pf.base;
    pthread_mutex_unlock(&pf.lock);
    size_t len = 0;
    const long saved = ftell(file);
    if (fseek(file, base, SEEK_SET) == 0) len = fread(pf.data, 1, VITA_DISC_CHUNK, file);
    if (saved >= 0) fseek(file, saved, SEEK_SET);
    clearerr(file);
    pthread_mutex_lock(&pf.lock);
    pf.len = len;
    pf.state = PF_READY;
    pthread_cond_broadcast(&pf.cond);
  }
  return NULL;
}

static void WaitNotBusy(void) {
  while (pf.state == PF_BUSY) pthread_cond_wait(&pf.cond, &pf.lock);
}

void VitaDiscPrefetchIdle(void) {
  pthread_mutex_lock(&pf.lock);
  WaitNotBusy();
  pthread_mutex_unlock(&pf.lock);
}

void VitaDiscPrefetchDrop(void) {
  pthread_mutex_lock(&pf.lock);
  WaitNotBusy();
  pf.state = PF_IDLE;
  pf.file = NULL;
  pthread_mutex_unlock(&pf.lock);
}

int VitaDiscPrefetchTake(FILE *file, long pos, size_t total, VitaDiscChunk *into) {
  pthread_mutex_lock(&pf.lock);
  if (pf.state == PF_BUSY && pf.file == file && pos >= pf.base && pos - pf.base < VITA_DISC_CHUNK) {
    VT_SCOPE(VT_DISC_IO);
    WaitNotBusy();
  }
  int ok = 0;
  if (pf.state == PF_READY && pf.file == file && pos >= pf.base) {
    const size_t off = (size_t)(pos - pf.base);
    /* Same coverage rule as a resident chunk: the whole read, or up to EOF. */
    if (off + total <= pf.len || (pf.len < VITA_DISC_CHUNK && off <= pf.len)) {
      into->file = file; into->base = pf.base; into->len = pf.len;
      memcpy(into->data, pf.data, pf.len);
      pf.state = PF_IDLE;
      ok = 1;
    }
  }
  pthread_mutex_unlock(&pf.lock);
  return ok;
}

void VitaDiscPrefetchStart(FILE *file, long base) {
  pthread_mutex_lock(&pf.lock);
  if (pf.state == PF_BUSY) { pthread_mutex_unlock(&pf.lock); return; }
  if (pf.state == PF_READY && pf.file == file && pf.base == base) { pthread_mutex_unlock(&pf.lock); return; }
  for (int i = 0; i < VITA_DISC_CHUNKS; ++i)       /* already resident */
    if (vita_disc_chunk[i].file == file && vita_disc_chunk[i].base == base && vita_disc_chunk[i].len) {
      pthread_mutex_unlock(&pf.lock);
      return;
    }
  if (!pf.started) {
    if (pthread_create(&pf.thread, NULL, PrefetchMain, NULL)) { pthread_mutex_unlock(&pf.lock); return; }
    pf.started = 1;
  }
  pf.file = file; pf.base = base; pf.state = PF_BUSY;
  pthread_cond_broadcast(&pf.cond);
  pthread_mutex_unlock(&pf.lock);
}
