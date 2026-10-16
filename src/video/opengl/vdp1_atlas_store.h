/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VDP1_ATLAS_STORE_H
#define VDP1_ATLAS_STORE_H
#include <stdint.h>
#include <string.h>
/* Rectangles for cached VDP1 sprites (vdp1_sprite_reuse.h) in rows the atlas
 * keeps across frames, so a sprite drawn again is sampled where it already is
 * instead of being copied into a new rectangle every frame.
 *
 * The rows are shelves of one height each, with rectangles packed by x. A
 * rectangle is held until its owner frees it or is evicted, which only an
 * owner not drawn in the current atlas frame can be (its texels may still be
 * sampled by this frame's earlier quads otherwise). */
enum { ATLAS_STORE_SHELVES = 256, ATLAS_STORE_SLOTS = 64 };

typedef struct { uint16_t x, w; void *owner; } AtlasStoreSlot;
typedef struct { uint16_t y, h, n; AtlasStoreSlot s[ATLAS_STORE_SLOTS]; } AtlasStoreShelf;
typedef struct {
  AtlasStoreShelf shelf[ATLAS_STORE_SHELVES];
  unsigned shelves, next_y;  /* shelves in use; first row past the last one */
  unsigned width, limit;     /* row width and the rows the store may use */
} AtlasStore;

static inline void AtlasStoreReset(AtlasStore *st, unsigned width, unsigned limit) {
  st->shelves = st->next_y = 0;
  st->width = width; st->limit = limit;
}

/* x of a w-wide gap in the shelf at insertion index *at, or -1. */
static inline int AtlasStoreGap(const AtlasStoreShelf *sh, unsigned width, unsigned w, unsigned *at) {
  unsigned end = 0;
  if (sh->n == ATLAS_STORE_SLOTS) return -1;
  for (unsigned i = 0; i < sh->n; ++i) {
    if (sh->s[i].x - end >= w) { *at = i; return (int)end; }
    end = sh->s[i].x + sh->s[i].w;
  }
  if (width - end >= w) { *at = sh->n; return (int)end; }
  return -1;
}

/* A w x h rectangle for owner: its shelf index, and *x, *y; -1 when full. */
static inline int AtlasStoreAlloc(AtlasStore *st, unsigned w, unsigned h, void *owner,
                                  unsigned *x, unsigned *y) {
  const unsigned hc = (h + 7) & ~7u;
  if (!w || !hc || w > st->width) return -1;
  for (unsigned i = 0; i < st->shelves; ++i) {
    AtlasStoreShelf *sh = &st->shelf[i];
    if (sh->h < hc || (sh->n && sh->h > hc + 8)) continue;
    unsigned at;
    const int gx = AtlasStoreGap(sh, st->width, w, &at);
    if (gx < 0) continue;
    memmove(&sh->s[at + 1], &sh->s[at], (sh->n - at) * sizeof(sh->s[0]));
    sh->s[at].x = (uint16_t)gx; sh->s[at].w = (uint16_t)w; sh->s[at].owner = owner;
    ++sh->n;
    *x = (unsigned)gx; *y = sh->y;
    return (int)i;
  }
  if (st->shelves == ATLAS_STORE_SHELVES || st->next_y + hc > st->limit) return -1;
  AtlasStoreShelf *sh = &st->shelf[st->shelves];
  sh->y = (uint16_t)st->next_y; sh->h = (uint16_t)hc; sh->n = 1;
  sh->s[0].x = 0; sh->s[0].w = (uint16_t)w; sh->s[0].owner = owner;
  st->next_y += hc;
  *x = 0; *y = sh->y;
  return (int)st->shelves++;
}

static inline void AtlasStoreFree(AtlasStore *st, int shelf, unsigned x) {
  if (shelf < 0 || (unsigned)shelf >= st->shelves) return;
  AtlasStoreShelf *sh = &st->shelf[shelf];
  for (unsigned i = 0; i < sh->n; ++i)
    if (sh->s[i].x == x) {
      memmove(&sh->s[i], &sh->s[i + 1], (sh->n - i - 1) * sizeof(sh->s[0]));
      --sh->n;
      return;
    }
}
#endif
