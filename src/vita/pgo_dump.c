/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VITA_PGO=generate: the SDK's libgcov has no file I/O, so the objects list
 * their gcov_info in the gcov_info section (-fprofile-info-section) and this
 * serializes them with __gcov_info_to_gcda into one appended file:
 *   u32 0 (run marker), then per object: u32 name length, name,
 *   u32 data length, .gcda bytes.
 * tools/pgo_split.py turns the runs into .gcda trees for gcov-tool merge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gcov_info;
extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];
extern void __gcov_info_to_gcda(const struct gcov_info *,
                                void (*)(const char *, void *),
                                void (*)(const void *, unsigned, void *),
                                void *(*)(unsigned, void *), void *);

typedef struct { unsigned char *data; unsigned len, cap; char name[512]; } Record;

static void OnFilename(const char *name, void *arg) {
  Record *r = arg;
  snprintf(r->name, sizeof(r->name), "%s", name);
}

static void OnData(const void *p, unsigned n, void *arg) {
  Record *r = arg;
  if (r->len + n > r->cap) {
    unsigned cap = r->cap ? r->cap : 4096;
    while (cap < r->len + n) cap *= 2;
    unsigned char *d = realloc(r->data, cap);
    if (!d) return;
    r->data = d; r->cap = cap;
  }
  memcpy(r->data + r->len, p, n);
  r->len += n;
}

static void *OnAllocate(unsigned n, void *arg) { (void)arg; return malloc(n); }

void VitaPgoDump(void) {
  FILE *f = fopen("ux0:data/yabause-vita/pgo.bin", "ab");
  if (!f) return;
  const unsigned marker = 0;
  fwrite(&marker, 4, 1, f);
  Record r = {0};
  for (const struct gcov_info *const *i = __start_gcov_info; i < __stop_gcov_info; ++i) {
    r.len = 0; r.name[0] = 0;
    __gcov_info_to_gcda(*i, OnFilename, OnData, OnAllocate, &r);
    const unsigned name_len = (unsigned)strlen(r.name);
    if (!name_len) continue;
    fwrite(&name_len, 4, 1, f); fwrite(r.name, 1, name_len, f);
    fwrite(&r.len, 4, 1, f); fwrite(r.data, 1, r.len, f);
  }
  free(r.data);
  fclose(f);
}
