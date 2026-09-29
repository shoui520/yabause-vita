/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/video/opengl/vdp2_cell_reuse.h"

static uint32_t atlas[64 * 64];
static uint8_t vram[512];
static uint16_t banks[4] = {1, 1, 1, 1};

#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
/* Stands in for the snapshot stamps: a test write to vram bumps the stamp
 * of its page (only page 0 holds test bytes). */
static uint32_t page_ver[128];
#define WRITE(expr) (expr, ++page_ver[0])
#else
#define WRITE(expr) (expr)
#endif

static Vdp2CellReuseQuery base(void) {
  return (Vdp2CellReuseQuery){ atlas + 16, 7, 8, 3, 48, 2, 1, 1, 0x100, 0, 0x40, 0x1f000000u,
                               banks, Vdp2CellReuseBytes(2, 1), vram
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
                               , page_ver
#endif
                               };
}

int main(void) {
  assert(Vdp2CellReuseBytes(1, 0) == 32 && Vdp2CellReuseBytes(2, 0) == 128);
  assert(Vdp2CellReuseBytes(1, 1) == 64 && Vdp2CellReuseBytes(2, 1) == 256);
  for (int i = 0; i < 512; ++i) vram[i] = (uint8_t)(i * 7);
  static Vdp2CellReuseRecord r;
  Vdp2CellReuseQuery q = base();
  assert(!Vdp2CellReuseMatch(&r, &q));                 /* empty record */
  q.frame = 7; Vdp2CellReuseStore(&r, &q);             /* written in atlas frame 7 */
  q = base();
  assert(Vdp2CellReuseMatch(&r, &q));
  /* Every input must invalidate. */
  Vdp2CellReuseQuery v;
  v = q; v.dst = atlas + 32; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.prev_frame = 6; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.gen = 4; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.pitch = 40; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.patternwh = 1; v.bytes = 64; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.colornumber = 0; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.transparent = 0; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.charaddr = 0x120; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.co = 1; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.pal = 0x50; assert(!Vdp2CellReuseMatch(&r, &v));
  v = q; v.abits = 0x10000000u; assert(!Vdp2CellReuseMatch(&r, &v));
  uint16_t other[4] = {1, 0, 1, 1};
  v = q; v.banks = other; assert(!Vdp2CellReuseMatch(&r, &v));
#ifdef VDP2_CELL_REUSE_PAGE_VERSIONS
  /* Any write to a page under the pattern invalidates, even one that
   * restores the bytes; stamps of other pages do not matter. */
  page_ver[1] = 9; page_ver[127] = 9; assert(Vdp2CellReuseMatch(&r, &q));
  WRITE(vram[255] ^= 1); assert(!Vdp2CellReuseMatch(&r, &q));
  WRITE(vram[255] ^= 1); assert(!Vdp2CellReuseMatch(&r, &q));
  q.frame = 8; Vdp2CellReuseStore(&r, &q); q.prev_frame = 8; assert(Vdp2CellReuseMatch(&r, &q));
  /* A pattern across a page boundary (0x1F80..0x207F): both pages count. */
  Vdp2CellReuseQuery x = q; x.charaddr = 0x1F80; x.frame = 8;
  Vdp2CellReuseRecord rx = {0}; Vdp2CellReuseStore(&rx, &x); assert(Vdp2CellReuseMatch(&rx, &x));
  ++page_ver[3]; assert(Vdp2CellReuseMatch(&rx, &x));
  ++page_ver[2]; assert(!Vdp2CellReuseMatch(&rx, &x));  /* last page */
  Vdp2CellReuseStore(&rx, &x); assert(Vdp2CellReuseMatch(&rx, &x));
  ++page_ver[1]; assert(!Vdp2CellReuseMatch(&rx, &x));  /* first page */
  /* A pattern ending exactly at a page end does not depend on the next page. */
  x.charaddr = 0x1F00; Vdp2CellReuseStore(&rx, &x);
  ++page_ver[2]; assert(Vdp2CellReuseMatch(&rx, &x));
#else
  vram[255] ^= 1; assert(!Vdp2CellReuseMatch(&r, &q)); /* last source byte */
  vram[255] ^= 1; assert(Vdp2CellReuseMatch(&r, &q));
  vram[256] ^= 1; assert(Vdp2CellReuseMatch(&r, &q));  /* outside the pattern */
  /* The stored copy is independent of later VRAM writes. */
  vram[0] ^= 0xff; assert(!Vdp2CellReuseMatch(&r, &q));
#endif

  /* Set-associative table: a stored dst is found again; a full set evicts
   * its oldest record, never a newer one. */
  static Vdp2CellReuseRecord table[CELL_REUSE_SLOTS];
  static uint32_t big[1 << 20];
  const uint32_t *same[CELL_REUSE_WAYS + 1];
  unsigned found = 0, set0 = Vdp2CellReuseSlot(table, big, 0) / CELL_REUSE_WAYS;
  for (unsigned i = 0; i < (1u << 20) && found <= CELL_REUSE_WAYS; ++i) {
    const unsigned slot = Vdp2CellReuseSlot(table, big + i, 0);
    assert(slot < CELL_REUSE_SLOTS);
    if (slot / CELL_REUSE_WAYS == set0) same[found++] = big + i;
  }
  assert(found == CELL_REUSE_WAYS + 1);
  for (unsigned i = 0; i < CELL_REUSE_WAYS; ++i) {
    Vdp2CellReuseQuery s = base(); s.dst = same[i]; s.frame = 10 + i;
    Vdp2CellReuseRecord *rec = &table[Vdp2CellReuseSlot(table, s.dst, s.frame)];
    assert(!rec->dst);                                   /* empty ways first */
    Vdp2CellReuseStore(rec, &s);
  }
  for (unsigned i = 0; i < CELL_REUSE_WAYS; ++i)
    assert(table[Vdp2CellReuseSlot(table, same[i], 20)].dst == same[i]);
  Vdp2CellReuseRecord *victim = &table[Vdp2CellReuseSlot(table, same[CELL_REUSE_WAYS], 20)];
  assert(victim->dst == same[0] && victim->frame == 10); /* oldest */
  table[Vdp2CellReuseSlot(table, same[0], 20)].frame = 19; /* refreshed by a reuse */
  victim = &table[Vdp2CellReuseSlot(table, same[CELL_REUSE_WAYS], 20)];
  assert(victim->dst == same[1]);
  puts("vdp2_cell_reuse: ok");
  return 0;
}
