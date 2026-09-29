/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/video/opengl/fb_meta_lut.h"

/* The FB_FAST decisions of framebuffer_common.cg (FB_LINE 0, condition < 3)
 * against the table: discard before the palette, priority index, paletted,
 * palette index 0 discard and output alpha. */
int main(void) {
  srand(7);
  uint32_t lut[256];
  for (int iter = 0; iter < 20000; ++iter) {
    float pri[8], alpha[8];
    for (int i = 0; i < 8; ++i) {
      pri[i] = (float)(rand() & 7) / 10.0f + 0.05f;
      alpha[i] = (float)(0xFF - (((rand() & 0x1F) << 3) & 0xF8)) / 255.0f;
    }
    const int mode = rand() & 3, condition = rand() % 3, sprite_window = rand() & 1;
    const float from = (float)(rand() % 9) / 10.0f, to = from + (float)(1 + rand() % 8) / 10.0f;
    const float cctl = (float)(rand() & 7) / 10.0f + 0.05f;
    FbMetaLutBuild(lut, mode, condition, pri, alpha, from, to, cctl, sprite_window);
    for (int m = 0; m < 256; ++m) {
      const uint32_t t = lut[m];
      const int keep = t >> 24 != 0;
      const float depth = pri[m & 7];
      const int want_keep = m >= 128 && !(depth < from || depth > to);
      assert(keep == want_keep);
      if (!keep) continue;
      assert((int)(t & 0xFF) == (m & 7));
      assert(((t >> 16 & 0xFF) != 0) == ((m & 0x40) != 0));
      assert((t >> 24 < 255) == (sprite_window != 0 || (m & 7) == 0));
      const int enabled = condition == 0 ? depth <= cctl : condition == 1 ? depth == cctl : depth >= cctl;
      const float a = alpha[(m >> 3) & 7];
      const float want = mode == 0 ? 1.0f : mode == 1 ? a : mode == 2 ? (enabled ? a : 1.0f)
                                                                        : (enabled ? 1.0f : 0.0f);
      assert((float)(t >> 8 & 0xFF) / 255.0f == want);
    }
  }
  puts("fb_meta_lut: ok");
  return 0;
}
