/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VDP1_FB_TILES_H
#define VDP1_FB_TILES_H
#include <stdint.h>
/* Which 16x16 tiles of a VDP1 framebuffer were drawn since its last erase
 * (VDP1 coordinates, grid 1024x1024, beyond every VDP1 framebuffer size).
 * Each triangle marks its bounding tiles grown by one tile on every side,
 * so a caller mapping tile edges to window pixels with any rounding under a
 * tile still covers every drawn texel. Composition draws only the rectangles
 * of marked tiles; they are disjoint, so no pixel is blended twice. */
enum { FB_TILE = 16, FB_TILE_COLS = 64, FB_TILE_ROWS = 64 };

typedef struct {
  int valid;                       /* 0: unknown, draw everything */
  uint64_t rows[FB_TILE_ROWS];     /* bit c: tile column c */
} Vdp1FbTiles;

typedef struct { int c0, r0, c1, r1; } Vdp1FbTileRect;   /* [c0,c1) x [r0,r1) */

static inline void Vdp1FbTilesEmpty(Vdp1FbTiles *t) {
  t->valid = 1;
  for (int r = 0; r < FB_TILE_ROWS; ++r) t->rows[r] = 0;
}

static inline int Vdp1FbTileIndex(float v, int n) {
  if (!(v >= 0.0f)) return 0;                 /* also NaN */
  if (v >= (float)(n * FB_TILE)) return n - 1;
  return (int)v / FB_TILE;
}

/* xy: triangle vertices (6 floats per triangle); count: floats. */
static inline void Vdp1FbTilesAdd(Vdp1FbTiles *t, const float *xy, int count) {
  if (!t->valid) return;
  const int step = count % 6 == 0 ? 6 : count;   /* else one box for all */
  for (int i = 0; i + 1 < count; i += step) {
    float x0 = xy[i], x1 = xy[i], y0 = xy[i + 1], y1 = xy[i + 1];
    for (int k = i; k + 1 < i + step && k + 1 < count; k += 2) {
      if (xy[k] != xy[k] || xy[k + 1] != xy[k + 1]) { t->valid = 0; return; }   /* NaN */
      if (xy[k] < x0) x0 = xy[k];
      if (xy[k] > x1) x1 = xy[k];
      if (xy[k + 1] < y0) y0 = xy[k + 1];
      if (xy[k + 1] > y1) y1 = xy[k + 1];
    }
    int c0 = Vdp1FbTileIndex(x0, FB_TILE_COLS) - 1, c1 = Vdp1FbTileIndex(x1, FB_TILE_COLS) + 1;
    int r0 = Vdp1FbTileIndex(y0, FB_TILE_ROWS) - 1, r1 = Vdp1FbTileIndex(y1, FB_TILE_ROWS) + 1;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > FB_TILE_COLS - 1) c1 = FB_TILE_COLS - 1;
    if (r1 > FB_TILE_ROWS - 1) r1 = FB_TILE_ROWS - 1;
    const uint64_t bits = (c1 - c0 == 63 ? ~0ull : ((1ull << (c1 - c0 + 1)) - 1)) << c0;
    for (int r = r0; r <= r1; ++r) t->rows[r] |= bits;
  }
}

/* Disjoint rectangles covering exactly the marked tiles: runs of each row,
 * rows with the same mask merged. Returns the count, or -1 above max. */
static inline int Vdp1FbTilesRects(const Vdp1FbTiles *t, Vdp1FbTileRect *out, int max) {
  int n = 0;
  for (int r = 0; r < FB_TILE_ROWS;) {
    const uint64_t m = t->rows[r];
    int r1 = r + 1;
    while (r1 < FB_TILE_ROWS && t->rows[r1] == m) ++r1;
    for (int c = 0; c < FB_TILE_COLS;) {
      if (!((m >> c) & 1)) { ++c; continue; }
      int c1 = c + 1;
      while (c1 < FB_TILE_COLS && ((m >> c1) & 1)) ++c1;
      if (n == max) return -1;
      out[n++] = (Vdp1FbTileRect){ c, r, c1, r1 };
      c = c1;
    }
    r = r1;
  }
  return n;
}
#endif
