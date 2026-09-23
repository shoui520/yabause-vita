/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/gxm/tile_batch.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
   VitaTileVertex vertices[8]; uint16_t indices[12];
   unsigned checks = 0;
   for (unsigned flip = 0; flip < 4; ++flip)
   for (int y = -9; y <= 18; ++y)
   for (int x = -9; x <= 26; ++x) {
      VitaTileBatch b = {vertices, indices, 0, 2};
      int result = VitaTileBatchAdd(&b, x, y, 8, 16, 32, 64, 24, 16, flip);
      int visible = x + 8 > 0 && y + 8 > 0 && x < 24 && y < 16;
      assert(result == visible && b.count == (unsigned)visible);
      if (!visible) continue;
      for (int py = 0; py < 16; ++py)
      for (int px = 0; px < 24; ++px) {
         if (px < x || px >= x + 8 || py < y || py >= y + 8) continue;
         float fx = (px + 0.5f - vertices[0].x) / (vertices[1].x - vertices[0].x);
         float fy = (py + 0.5f - vertices[0].y) / (vertices[2].y - vertices[0].y);
         int tx = (int)floorf((vertices[0].u + fx * (vertices[1].u - vertices[0].u)) * 32);
         int ty = (int)floorf((vertices[0].v + fy * (vertices[2].v - vertices[0].v)) * 64);
         assert(tx == 8 + ((flip & 1) ? 7 - (px - x) : px - x));
         assert(ty == 16 + ((flip & 2) ? 7 - (py - y) : py - y));
         ++checks;
      }
      assert(VitaTileBatchAdd(&b, 0, 0, 0, 0, 8, 8, 24, 16, 0) == 1);
      assert(indices[6] == 4 && indices[11] == 7);
      VitaTileVertex saved[8]; memcpy(saved, vertices, sizeof(saved));
      assert(VitaTileBatchAdd(&b, 0, 0, 0, 0, 8, 8, 24, 16, 0) == -1);
      assert(b.count == 2 && !memcmp(saved, vertices, sizeof(saved)));
   }
   VitaTileBatch b = {vertices, indices, 0, 2};
   assert(VitaTileBatchAdd(&b, INT_MIN, INT_MAX, 0, 0, 8, 8, 24, 16, 0) == 0);
   assert(VitaTileBatchAdd(&b, 0, 0, 1, 0, 8, 8, 24, 16, 0) == -1);
   assert(VitaTileBatchAdd(&b, 0, 0, 0, 0, 8, 8, 24, 16, 4) == -1);
   VitaTileVertex *large_v = malloc(65536 * sizeof(*large_v));
   uint16_t *large_i = malloc(16384 * 6 * sizeof(*large_i));
   assert(large_v && large_i);
   VitaTileBatch last = {large_v, large_i, 16383, 16384};
   assert(VitaTileBatchAdd(&last, 0, 0, 0, 0, 8, 8, 24, 16, 0) == 1);
   assert(large_i[16383 * 6] == 65532 && large_i[16383 * 6 + 5] == 65535);
   assert(VitaTileBatchAdd(&last, 0, 0, 0, 0, 8, 8, 24, 16, 0) == -1);
   free(large_v); free(large_i);
   printf("tile batch UV/clipping: %u pixel checks passed\n", checks);
}
