/* SPDX-License-Identifier: GPL-2.0-or-later
 * Asynchronous append-only log writer. Producers copy formatted text into a
 * bounded byte ring and return; one writer thread performs the file writes and
 * flushes. A full ring blocks the producer (text is never dropped). Drain waits
 * until every byte enqueued before the call has been written and flushed.
 */
#ifndef VITA_LOG_WRITER_H
#define VITA_LOG_WRITER_H
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { VITA_LOG_RING = 256 * 1024 };

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t readable, writable, flushed;
  char data[VITA_LOG_RING];
  size_t head, used;          /* producer offset and queued byte count */
  unsigned long long queued, written; /* monotonic byte totals */
  FILE *file;
  int stop, running;
  pthread_t thread;
  void (*setup)(void); /* optional: runs first on the writer thread */
} VitaLogWriter;

static void *VitaLogWriterMain(void *opaque) {
  VitaLogWriter *w = (VitaLogWriter *)opaque;
  if (w->setup) w->setup();
  pthread_mutex_lock(&w->lock);
  for (;;) {
    while (!w->used && !w->stop) pthread_cond_wait(&w->readable, &w->lock);
    if (!w->used && w->stop) break;
    /* Write the oldest contiguous span outside the lock. */
    const size_t tail = (w->head + VITA_LOG_RING - w->used) % VITA_LOG_RING;
    const size_t span = w->used < VITA_LOG_RING - tail ? w->used : VITA_LOG_RING - tail;
    const char *chunk = w->data + tail;
    pthread_mutex_unlock(&w->lock);
    fwrite(chunk, 1, span, w->file);
    fflush(w->file);
    pthread_mutex_lock(&w->lock);
    w->used -= span;
    w->written += span;
    pthread_cond_broadcast(&w->writable);
    pthread_cond_broadcast(&w->flushed);
  }
  pthread_mutex_unlock(&w->lock);
  return NULL;
}

/* Returns 0 on success. The file stays owned by the caller. */
static int VitaLogWriterStart(VitaLogWriter *w, FILE *file, void (*setup)(void)) {
  memset(w, 0, sizeof(*w));
  w->file = file;
  w->setup = setup;
  if (pthread_mutex_init(&w->lock, NULL) || pthread_cond_init(&w->readable, NULL) ||
      pthread_cond_init(&w->writable, NULL) || pthread_cond_init(&w->flushed, NULL))
    return -1;
  if (pthread_create(&w->thread, NULL, VitaLogWriterMain, w)) return -1;
  w->running = 1;
  return 0;
}

static void VitaLogWriterAppend(VitaLogWriter *w, const char *text, size_t length) {
  pthread_mutex_lock(&w->lock);
  while (length) {
    while (w->used == VITA_LOG_RING) pthread_cond_wait(&w->writable, &w->lock);
    size_t room = VITA_LOG_RING - w->used;
    size_t contiguous = VITA_LOG_RING - w->head;
    size_t n = length < room ? length : room;
    if (n > contiguous) n = contiguous;
    memcpy(w->data + w->head, text, n);
    w->head = (w->head + n) % VITA_LOG_RING;
    w->used += n; w->queued += n;
    text += n; length -= n;
    pthread_cond_signal(&w->readable);
  }
  pthread_mutex_unlock(&w->lock);
}

/* Wait until everything queued before this call is written and flushed. */
static void VitaLogWriterDrain(VitaLogWriter *w) {
  pthread_mutex_lock(&w->lock);
  const unsigned long long target = w->queued;
  while (w->written < target) pthread_cond_wait(&w->flushed, &w->lock);
  pthread_mutex_unlock(&w->lock);
}

/* Drains, then joins the writer. Safe to call once after VitaLogWriterStart. */
static void VitaLogWriterStop(VitaLogWriter *w) {
  if (!w->running) return;
  pthread_mutex_lock(&w->lock);
  w->stop = 1;
  pthread_cond_signal(&w->readable);
  pthread_mutex_unlock(&w->lock);
  pthread_join(w->thread, NULL);
  w->running = 0;
}
#endif
