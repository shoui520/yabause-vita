/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VDP2_CELL_REUSE_H
#define VDP2_CELL_REUSE_H
#include <stdint.h>
#include <string.h>
/* Cross-frame reuse of decoded VDP2 cell patterns (fast 4/8bpp path).
 *
 * The staging atlas keeps its texels across frames; a reset only rewinds the
 * bump allocator, and one frame never hands out a rectangle twice. So when a
 * pattern is allocated the rectangle it occupied in the atlas frame the
 * current storage last held, and every decode input and source byte is the
 * same, the rectangle already holds this decode's output.
 *
 * With VDP2_CELL_REUSE_PAGE_VERSIONS the source bytes are not kept: the
 * caller passes the write stamps of the 4 KiB VRAM pages (render_proxy.c),
 * and equal stamps of the pages under the pattern mean no write since the
 * record, so the same bytes.
 *
 * Records are keyed by the rectangle's address in a set-associative table
 * (the zero-copy atlas rotates up to 4 storages, so a record must outlive
 * the other storages' frames); a full set replaces its oldest record (the
 * next decode of that one is not skipped). */
enum { CELL_REUSE_WAYS = 4, CELL_REUSE_SETS = 4096, CELL_REUSE_SLOTS = CELL_REUSE_WAYS * CELL_REUSE_SETS,
       CELL_REUSE_MAX_BYTES = 256 };

typedef struct {
  const uint32_t *dst;        /* atlas rectangle (NULL: empty) */
  unsigned frame, gen;        /* atlas frame and storage generation written in */
  int pitch, patternwh, colornumber, transparent;
  uint32_t charaddr, co, pal, abits;
  uint16_t banks[4];
  unsigned bytes;
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
  uint32_t ver[2];            /* stamps of the first and last source page */
#else
  uint8_t src[CELL_REUSE_MAX_BYTES];
#endif
} Vdp2CellReuseRecord;

typedef struct {
  const uint32_t *dst;
  unsigned prev_frame, frame, gen;
  int pitch, patternwh, colornumber, transparent;
  uint32_t charaddr, co, pal, abits;
  const uint16_t *banks;      /* 4 entries */
  unsigned bytes;
  const uint8_t *src;         /* bytes of character data at charaddr */
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
  const uint32_t *page_ver;   /* 128 page stamps of the VRAM src is in */
#endif
} Vdp2CellReuseQuery;

/* The record of dst in its set, else the set's empty or oldest record. */
static inline unsigned Vdp2CellReuseSlot(const Vdp2CellReuseRecord *records, const uint32_t *dst,
                                         unsigned frame) {
  const uint32_t a = (uint32_t)(uintptr_t)dst >> 2;
  const unsigned set = ((a * 2654435761u) >> (32 - 12)) * CELL_REUSE_WAYS;
  unsigned victim = set, oldest = 0;
  for (unsigned w = set; w < set + CELL_REUSE_WAYS; ++w) {
    if (records[w].dst == dst) return w;
    const unsigned age = records[w].dst ? frame - records[w].frame : ~0u;
    if (age > oldest || w == set) { oldest = age; victim = w; }
  }
  return victim;
}

/* Source bytes of one pattern: patternwh^2 cells of 32 (4bpp) or 64 (8bpp). */
static inline unsigned Vdp2CellReuseBytes(int patternwh, int colornumber) {
  return (unsigned)(patternwh * patternwh) * (colornumber == 0 ? 32u : 64u);
}

static inline int Vdp2CellReuseMatch(const Vdp2CellReuseRecord *r, const Vdp2CellReuseQuery *q) {
  return r->dst == q->dst && r->frame == q->prev_frame && r->gen == q->gen &&
    r->pitch == q->pitch && r->patternwh == q->patternwh && r->colornumber == q->colornumber &&
    r->transparent == q->transparent && r->charaddr == q->charaddr && r->co == q->co &&
    r->pal == q->pal && r->abits == q->abits && r->bytes == q->bytes &&
    !memcmp(r->banks, q->banks, sizeof(r->banks)) &&
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
    r->ver[0] == q->page_ver[(q->charaddr >> 12) & 127] &&
    r->ver[1] == q->page_ver[((q->charaddr + q->bytes - 1) >> 12) & 127];
#else
    !memcmp(r->src, q->src, q->bytes);
#endif
}

static inline void Vdp2CellReuseStore(Vdp2CellReuseRecord *r, const Vdp2CellReuseQuery *q) {
  r->dst = q->dst; r->frame = q->frame; r->gen = q->gen;
  r->pitch = q->pitch; r->patternwh = q->patternwh; r->colornumber = q->colornumber;
  r->transparent = q->transparent; r->charaddr = q->charaddr; r->co = q->co;
  r->pal = q->pal; r->abits = q->abits; r->bytes = q->bytes;
  memcpy(r->banks, q->banks, sizeof(r->banks));
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
  r->ver[0] = q->page_ver[(q->charaddr >> 12) & 127];
  r->ver[1] = q->page_ver[((q->charaddr + q->bytes - 1) >> 12) & 127];
#else
  memcpy(r->src, q->src, q->bytes);
#endif
}
#endif
