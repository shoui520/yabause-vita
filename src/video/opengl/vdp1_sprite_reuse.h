/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VDP1_SPRITE_REUSE_H
#define VDP1_SPRITE_REUSE_H
#include <stdint.h>
#include <string.h>
/* Cross-frame reuse of decoded VDP1 sprite textures (Vdp1ReadTexture).
 *
 * As for VDP2 cells (vdp2_cell_reuse.h): the staging atlas keeps its texels
 * across frames and one atlas frame never hands out a rectangle twice, so a
 * sprite allocated the rectangle it occupied in the atlas frame the current
 * storage last held, with the same decode inputs and unchanged source bytes,
 * already finds this decode's output there.
 *
 * The decode reads the command's CMDSRCA, CMDPMOD, CMDCOLR and size, SPCTL,
 * and VDP1 RAM: the character data and, in LUT mode, the 16-word table at
 * CMDCOLR * 8. Source bytes are compared through the 4 KiB page write stamps
 * of the snapshot (render_proxy.c): stamps only grow, and a write after a
 * snapshot is stamped above every stamp that snapshot copied, so an equal
 * maximum over the pages means no write since the record. */
enum { SPRITE_REUSE_WAYS = 4, SPRITE_REUSE_SETS = 1024 };

typedef struct {
  const uint32_t *dst;       /* atlas rectangle (NULL: empty) */
  unsigned frame, gen;       /* atlas frame and storage generation written in */
  uint32_t size, pitch, srca, pmod, colr, spctl, ver;
} Vdp1SpriteReuseRecord;

/* Maximum page stamp over the bytes [start, start + bytes) of VDP1 RAM
 * (addresses wrap at 512 KiB, as the decoder's reads do). */
static inline uint32_t Vdp1SpriteReusePages(const uint32_t *page_ver, uint32_t start, uint32_t bytes,
                                            uint32_t ver) {
  if (!bytes) return ver;
  const uint32_t first = (start & 0x7FFFF) >> 12;
  uint32_t n = (((start & 0xFFF) + bytes - 1) >> 12) + 1;
  if (n > 128) n = 128;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t v = page_ver[(first + i) & 127];
    if (v > ver) ver = v;
  }
  return ver;
}

/* The record of dst in its set, else the set's empty or oldest record. */
static inline Vdp1SpriteReuseRecord *Vdp1SpriteReuseSlot(Vdp1SpriteReuseRecord *records, const uint32_t *dst,
                                                         unsigned frame) {
  const uint32_t a = (uint32_t)(uintptr_t)dst >> 2;
  const unsigned set = ((a * 2654435761u) >> (32 - 10)) * SPRITE_REUSE_WAYS;
  unsigned victim = set, oldest = 0;
  for (unsigned w = set; w < set + SPRITE_REUSE_WAYS; ++w) {
    if (records[w].dst == dst) return &records[w];
    const unsigned age = records[w].dst ? frame - records[w].frame : ~0u;
    if (age > oldest || w == set) { oldest = age; victim = w; }
  }
  return &records[victim];
}

/* Decoded sprites kept in cached RAM by their decode inputs (the reuse
 * record's, with CCCTL). Moving 3D scenes allocate a sprite somewhere else
 * every frame, so the rectangle match above fails; a hit here copies the
 * texels into the new rectangle (or draws them from the atlas store,
 * vdp1_atlas_store.h). */
enum { SPRITE_CACHE_WAYS = 4, SPRITE_CACHE_SETS = 512, SPRITE_CACHE_BUDGET = 8 << 20,
       SPRITE_CACHE_MAX_TEXELS = 256 * 256 };

typedef struct {
  uint32_t size, srca, pmod, colr, spctl, ccctl, ver;
  unsigned used;             /* atlas frame of the last hit or store */
  uint32_t *texels;          /* w * h, rows packed; NULL: empty */
  uint32_t cap;              /* texels allocated */
  /* Atlas store rectangle (vdp1_atlas_store.h), valid while store_epoch is
   * the store's; store_gen: atlas storages already holding the texels. */
  int store_shelf;
  uint16_t store_x, store_y;
  unsigned store_epoch, store_next;
  uint32_t store_gen[4];
} Vdp1SpriteCacheEntry;

typedef struct {
  Vdp1SpriteCacheEntry e[SPRITE_CACHE_SETS * SPRITE_CACHE_WAYS];
  size_t bytes;              /* allocated texels, bounded by the budget */
} Vdp1SpriteCache;

static inline int Vdp1SpriteCacheKeyEqual(const Vdp1SpriteCacheEntry *e, const Vdp1SpriteReuseRecord *q,
                                          uint32_t ccctl) {
  return e->texels && e->size == q->size && e->srca == q->srca && e->pmod == q->pmod && e->colr == q->colr &&
    e->spctl == q->spctl && e->ccctl == ccctl && e->ver == q->ver;
}

static inline uint32_t Vdp1SpriteCacheHash(const Vdp1SpriteReuseRecord *q) {
  uint32_t a = q->srca * 0x9E3779B1u ^ q->size * 0x85EBCA77u ^ q->colr * 0xC2B2AE3Du ^ q->pmod;
  a ^= a >> 15; a *= 0x2C1B3C6Du; a ^= a >> 12;
  return a & (SPRITE_CACHE_SETS - 1);
}

/* The entry holding q's decode, else NULL; nothing is replaced. */
static inline Vdp1SpriteCacheEntry *Vdp1SpriteCacheFind(Vdp1SpriteCache *c, const Vdp1SpriteReuseRecord *q,
                                                        uint32_t ccctl) {
  Vdp1SpriteCacheEntry *set = &c->e[Vdp1SpriteCacheHash(q) * SPRITE_CACHE_WAYS];
  for (unsigned w = 0; w < SPRITE_CACHE_WAYS; ++w)
    if (Vdp1SpriteCacheKeyEqual(&set[w], q, ccctl)) return &set[w];
  return NULL;
}

/* The entry holding q's decode (*hit), else the one to store it in, with
 * room for its texels; NULL when the budget has none. */
static inline Vdp1SpriteCacheEntry *Vdp1SpriteCacheSlot(Vdp1SpriteCache *c, const Vdp1SpriteReuseRecord *q,
                                                        uint32_t ccctl, unsigned now, int *hit) {
  Vdp1SpriteCacheEntry *set = &c->e[Vdp1SpriteCacheHash(q) * SPRITE_CACHE_WAYS], *victim = set;
  *hit = 0;
  for (unsigned w = 0; w < SPRITE_CACHE_WAYS; ++w) {
    Vdp1SpriteCacheEntry *e = &set[w];
    if (Vdp1SpriteCacheKeyEqual(e, q, ccctl)) { *hit = 1; e->used = now; return e; }
    if (!e->texels) { if (victim->texels) victim = e; }
    else if (victim->texels && now - e->used > now - victim->used) victim = e;
  }
  const uint32_t need = (q->size & 0xFFFF) * (q->size >> 16);
  if (!need || need > SPRITE_CACHE_MAX_TEXELS) return NULL;
  if (victim->cap < need) {
    if (c->bytes - (size_t)victim->cap * 4 + (size_t)need * 4 > SPRITE_CACHE_BUDGET) return NULL;
    uint32_t *t = (uint32_t *)realloc(victim->texels, (size_t)need * 4);
    if (!t) return NULL;
    c->bytes += (size_t)(need - victim->cap) * 4;
    victim->texels = t; victim->cap = need;
  }
  victim->size = 0;          /* empty until stored */
  return victim;
}

static inline void Vdp1SpriteCacheStore(Vdp1SpriteCacheEntry *e, const Vdp1SpriteReuseRecord *q, uint32_t ccctl,
                                        unsigned now) {
  e->size = q->size; e->srca = q->srca; e->pmod = q->pmod; e->colr = q->colr;
  e->spctl = q->spctl; e->ccctl = ccctl; e->ver = q->ver; e->used = now;
}

static inline int Vdp1SpriteReuseMatch(const Vdp1SpriteReuseRecord *r, const Vdp1SpriteReuseRecord *q,
                                       unsigned prev_frame) {
  return r->dst == q->dst && r->frame == prev_frame && r->gen == q->gen && r->size == q->size &&
    r->pitch == q->pitch && r->srca == q->srca && r->pmod == q->pmod && r->colr == q->colr &&
    r->spctl == q->spctl && r->ver == q->ver;
}
#endif
