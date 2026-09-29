/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/video/opengl/vdp1_fb_tiles.h"

/* Tile (c,r) inside some rectangle; returns how many contain it. */
static int covered(const Vdp1FbTileRect *rc, int n, int c, int r) {
  int k = 0;
  for (int i = 0; i < n; ++i) k += c >= rc[i].c0 && c < rc[i].c1 && r >= rc[i].r0 && r < rc[i].r1;
  return k;
}

int main(void) {
  static Vdp1FbTiles t;
  Vdp1FbTileRect rc[4096];
  Vdp1FbTilesEmpty(&t);
  assert(Vdp1FbTilesRects(&t, rc, 4096) == 0);

  /* One triangle at pixels 40..50: tiles 2..3, grown to 1..4. */
  const float tri[6] = { 40, 40, 50, 40, 40, 50 };
  Vdp1FbTilesAdd(&t, tri, 6);
  int n = Vdp1FbTilesRects(&t, rc, 4096);
  assert(n == 1 && rc[0].c0 == 1 && rc[0].c1 == 5 && rc[0].r0 == 1 && rc[0].r1 == 5);
  assert(Vdp1FbTilesRects(&t, rc, 0) == -1);

  /* Out-of-grid and negative coordinates clamp; NaN invalidates. */
  const float wide[6] = { -500, -3, 5000, 2000, 10, 10 };
  Vdp1FbTilesEmpty(&t); Vdp1FbTilesAdd(&t, wide, 6);
  n = Vdp1FbTilesRects(&t, rc, 4096);
  assert(n == 1 && rc[0].c0 == 0 && rc[0].c1 == 64 && rc[0].r0 == 0 && rc[0].r1 == 64);
  const float bad[6] = { 0, 0, 0.0f / 0.0f, 1, 2, 2 };
  Vdp1FbTilesEmpty(&t); Vdp1FbTilesAdd(&t, bad, 6); assert(!t.valid);

  /* Random triangles: every texel of every triangle's bounding box lies in
   * exactly one rectangle, with at least a tile of margin around it; tiles
   * outside every grown box are in none. */
  srand(1);
  for (int iter = 0; iter < 2000; ++iter) {
    Vdp1FbTilesEmpty(&t);
    const int tris = 1 + rand() % 40;
    static float xy[40 * 6];
    static unsigned char want[FB_TILE_ROWS][FB_TILE_COLS];
    memset(want, 0, sizeof want);
    for (int i = 0; i < tris * 6; i += 2) {
      xy[i] = (float)(rand() % 760) - 20.0f + (rand() % 100) / 100.0f;
      xy[i + 1] = (float)(rand() % 560) - 20.0f;
    }
    Vdp1FbTilesAdd(&t, xy, tris * 6);
    for (int k = 0; k < tris; ++k) {
      float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
      for (int v = 0; v < 3; ++v) {
        const float x = xy[k * 6 + v * 2], y = xy[k * 6 + v * 2 + 1];
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
      }
      const int c0 = Vdp1FbTileIndex(x0, 64), c1 = Vdp1FbTileIndex(x1, 64);
      const int r0 = Vdp1FbTileIndex(y0, 64), r1 = Vdp1FbTileIndex(y1, 64);
      for (int r = r0 - 1; r <= r1 + 1; ++r)
        for (int c = c0 - 1; c <= c1 + 1; ++c)
          if (r >= 0 && c >= 0 && r < 64 && c < 64) want[r][c] = 1;
    }
    n = Vdp1FbTilesRects(&t, rc, 4096);
    assert(n >= 0);
    for (int r = 0; r < 64; ++r)
      for (int c = 0; c < 64; ++c)
        assert(covered(rc, n, c, r) == want[r][c]);
  }
  puts("vdp1_fb_tiles: ok");
  return 0;
}
