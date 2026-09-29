/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_DISC_IO_H
#define VITA_DISC_IO_H
#include <stdio.h>
#include <string.h>
#include "telemetry.h"
/* Stdio latency includes buffering and preemption; it is not physical-device
 * busy time. Keep codec execution outside these scopes. Preserve return values. */
#ifdef VITA_DISC_READ_AHEAD
/* Sector reads are a SEEK_SET (or SEEK_CUR) followed by one small fread on a
 * read-only image. Each one costs a real seek + read on the memory card;
 * instead keep the logical position here and serve reads from a few 64 KiB
 * chunks (one per recently used file: data and CDDA tracks alternate). A
 * read returns the same bytes and item count fread would, including short
 * reads at end of file. Anything else falls back to stdio. */
enum { VITA_DISC_CHUNK = 64 * 1024, VITA_DISC_CHUNKS = 4 };
typedef struct {
  FILE *file;
  long base;
  size_t len;
  unsigned last_use;
  unsigned char data[VITA_DISC_CHUNK];
} VitaDiscChunk;
extern VitaDiscChunk vita_disc_chunk[VITA_DISC_CHUNKS];
extern FILE *vita_disc_pos_file;  /* stream whose logical position is vita_disc_pos */
extern long vita_disc_pos;
extern unsigned vita_disc_clock;
#ifdef VITA_DISC_PREFETCH
/* One chunk read ahead on a worker thread: after a miss that loads a full
 * chunk, the worker reads 64 KiB from where the next sequential read of the
 * same size will miss; that miss takes those bytes instead of blocking on the
 * card. The worker is the only
 * other user of the streams: every direct stdio call here first waits for it
 * to be idle, and it restores the stream position it found. */
void VitaDiscPrefetchIdle(void);                    /* wait until no read is in flight */
int VitaDiscPrefetchTake(FILE *file, long pos, size_t total, VitaDiscChunk *into);
void VitaDiscPrefetchStart(FILE *file, long base);
void VitaDiscPrefetchDrop(void);                    /* idle + forget the result */
#else
static inline void VitaDiscPrefetchIdle(void) {}
static inline int VitaDiscPrefetchTake(FILE *f, long p, size_t t, VitaDiscChunk *c) { (void)f; (void)p; (void)t; (void)c; return 0; }
static inline void VitaDiscPrefetchStart(FILE *f, long b) { (void)f; (void)b; }
static inline void VitaDiscPrefetchDrop(void) {}
#endif
static inline int VitaDiscSeek(FILE *file, long offset, int origin) {
  if (origin == SEEK_SET && offset >= 0) { vita_disc_pos_file = file; vita_disc_pos = offset; return 0; }
  if (origin == SEEK_CUR && file == vita_disc_pos_file && vita_disc_pos + offset >= 0) { vita_disc_pos += offset; return 0; }
  VT_SCOPE(VT_DISC_IO);
  VitaDiscPrefetchIdle();
  if (file == vita_disc_pos_file) {                 /* resync stdio before a relative seek */
    vita_disc_pos_file = NULL;
    if (origin == SEEK_CUR) { origin = SEEK_SET; offset += vita_disc_pos; }
  }
  return fseek(file, offset, origin);
}
static inline size_t VitaDiscRead(void *buffer, size_t size, size_t count, FILE *file) {
  const size_t total = size * count;
  if (file != vita_disc_pos_file || !size || total > VITA_DISC_CHUNK) {
    VT_SCOPE(VT_DISC_IO);
    VitaDiscPrefetchIdle();
    if (file == vita_disc_pos_file) { fseek(file, vita_disc_pos, SEEK_SET); vita_disc_pos_file = NULL; }
    return fread(buffer, size, count, file);
  }
  const long pos = vita_disc_pos;
  VitaDiscChunk *c = NULL, *victim = &vita_disc_chunk[0];
  for (int i = 0; i < VITA_DISC_CHUNKS; ++i) {
    VitaDiscChunk *k = &vita_disc_chunk[i];
    if (k->file == file && pos >= k->base && (size_t)(pos - k->base) + total <= k->len) { c = k; break; }
    if (k->file == file && pos >= k->base && k->len < VITA_DISC_CHUNK && pos - k->base <= (long)k->len) { c = k; break; } /* EOF chunk */
    if (k->last_use < victim->last_use) victim = k;
  }
  if (!c) {
    c = victim;
    if (!VitaDiscPrefetchTake(file, pos, total, c)) {
      VT_SCOPE(VT_DISC_IO);
      VitaDiscPrefetchIdle();
      c->file = file; c->base = pos; c->len = 0;
      if (fseek(file, pos, SEEK_SET) == 0) c->len = fread(c->data, 1, VITA_DISC_CHUNK, file);
    }
    /* Sequential reads of this size (2352-byte CDDA sectors do not divide the
     * chunk) next miss at the first one that no longer fits: fetch from there. */
    if (c->len == VITA_DISC_CHUNK && total)
      VitaDiscPrefetchStart(file, pos + (long)((c->base + VITA_DISC_CHUNK - pos) / total * total));
  }
  c->last_use = ++vita_disc_clock;
  size_t avail = pos >= c->base && (size_t)(pos - c->base) < c->len ? c->len - (size_t)(pos - c->base) : 0;
  if (avail > total) avail = total;
  memcpy(buffer, c->data + (pos - c->base), avail);
  vita_disc_pos = pos + (long)avail;
  return avail / size;
}
/* Streams may be closed and their FILE addresses reused: forget everything. */
static inline void VitaDiscForget(void) {
  VitaDiscPrefetchDrop();
  for (int i = 0; i < VITA_DISC_CHUNKS; ++i) { vita_disc_chunk[i].file = NULL; vita_disc_chunk[i].len = 0; }
  vita_disc_pos_file = NULL;
}
#define VITA_DISC_IO_STORAGE \
  VitaDiscChunk vita_disc_chunk[VITA_DISC_CHUNKS]; FILE *vita_disc_pos_file; long vita_disc_pos; unsigned vita_disc_clock;
#else
static inline size_t VitaDiscRead(void *buffer, size_t size, size_t count, FILE *file) {
  VT_SCOPE(VT_DISC_IO);
  return fread(buffer, size, count, file);
}
static inline int VitaDiscSeek(FILE *file, long offset, int origin) {
  VT_SCOPE(VT_DISC_IO);
  return fseek(file, offset, origin);
}
static inline void VitaDiscForget(void) {}
#define VITA_DISC_IO_STORAGE
#endif
#endif
