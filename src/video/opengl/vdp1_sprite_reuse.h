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

static inline int Vdp1SpriteReuseMatch(const Vdp1SpriteReuseRecord *r, const Vdp1SpriteReuseRecord *q,
                                       unsigned prev_frame) {
  return r->dst == q->dst && r->frame == prev_frame && r->gen == q->gen && r->size == q->size &&
    r->pitch == q->pitch && r->srca == q->srca && r->pmod == q->pmod && r->colr == q->colr &&
    r->spctl == q->spctl && r->ver == q->ver;
}
#endif
