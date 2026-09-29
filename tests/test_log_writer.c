/* SPDX-License-Identifier: GPL-2.0-or-later
 * Asynchronous log writer: ordered, complete output across wraparound,
 * concurrent producers, drain fences and stop. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/vita/log_writer.h"

static VitaLogWriter writer;
enum { PRODUCERS = 4, LINES = 20000 };

static void *producer(void *arg) {
  const int id = (int)(size_t)arg;
  char line[3000];
  for (int i = 0; i < LINES; ++i) {
    /* Varying lengths, some larger than a tenth of the ring, force wraps. */
    const int pad = (i * 37 + id * 11) % (i % 97 == 0 ? 2900 : 60);
    int n = snprintf(line, sizeof(line), "p%d %d ", id, i);
    memset(line + n, 'a' + id, (size_t)pad); n += pad;
    line[n++] = '\n';
    VitaLogWriterAppend(&writer, line, (size_t)n);
    if (i % 1000 == 0) VitaLogWriterDrain(&writer);
  }
  return NULL;
}

int main(void) {
  FILE *f = tmpfile();
  assert(f);
  assert(VitaLogWriterStart(&writer, f, NULL) == 0);
  pthread_t threads[PRODUCERS];
  for (int i = 0; i < PRODUCERS; ++i) assert(!pthread_create(&threads[i], NULL, producer, (void *)(size_t)i));
  for (int i = 0; i < PRODUCERS; ++i) pthread_join(threads[i], NULL);
  VitaLogWriterDrain(&writer);
  const unsigned long long total = writer.queued;
  assert(writer.written == total);
  VitaLogWriterStop(&writer);
  assert(ftell(f) == (long)total);
  rewind(f);
  /* Every line intact and each producer's lines in order. */
  int next[PRODUCERS] = {0};
  char *line = NULL; size_t cap = 0; ssize_t len; unsigned long lines = 0;
  while ((len = getline(&line, &cap, f)) > 0) {
    int id, i, off;
    assert(sscanf(line, "p%d %d %n", &id, &i, &off) == 2);
    assert(id >= 0 && id < PRODUCERS && i == next[id]);
    ++next[id]; ++lines;
    for (ssize_t k = off; k < len - 1; ++k) assert(line[k] == 'a' + id);
  }
  for (int i = 0; i < PRODUCERS; ++i) assert(next[i] == LINES);
  free(line); fclose(f);
  printf("log_writer_test_pass lines=%lu bytes=%llu\n", lines, total);
  return 0;
}
