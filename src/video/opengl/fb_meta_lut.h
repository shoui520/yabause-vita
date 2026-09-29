/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FB_META_LUT_H
#define FB_META_LUT_H
#include <math.h>
#include <stdint.h>
/* Per-draw lookup of everything the sprite composition decides from the
 * metadata byte (framebuffer alpha) and draw uniforms alone, so the shader
 * reads one texel instead of decoding bit fields and selecting uniforms.
 * Texel of metadata m (RGBA8):
 *   r: priority index (the shader still selects the exact u_pri depth)
 *   g: output alpha byte (FB_MODE / FB_CONDITION applied)
 *   b: 255 when paletted (bit 6), else 0
 *   a: 0 discard; 128 also discard palette index 0; 255 keep
 * Modes/conditions as framebuffer_common.cg; condition 3 (palette MSB) is
 * decided by the palette entry and has no table. */
static inline void FbMetaLutBuild(uint32_t out[256], int mode, int condition, const float pri[8],
                                  const float alpha[8], float from, float to, float cctl,
                                  int sprite_window) {
  for (int m = 0; m < 256; ++m) {
    const int p = m & 7, ai = (m >> 3) & 7;
    const float depth = pri[p];
    if (m < 128 || depth < from || depth > to) { out[m] = 0; continue; }
    const int enabled = condition == 0 ? depth <= cctl : condition == 1 ? depth == cctl : depth >= cctl;
    const uint32_t a8 = (uint32_t)floorf(alpha[ai] * 255.0f + 0.5f);
    const uint32_t g = mode == 0 ? 255u : mode == 1 ? a8 : mode == 2 ? (enabled ? a8 : 255u)
                                                                       : (enabled ? 255u : 0u);
    const uint32_t b = (m & 0x40) ? 255u : 0u;
    const uint32_t a = (sprite_window || p == 0) ? 128u : 255u;
    out[m] = (uint32_t)p | g << 8 | b << 16 | a << 24;
  }
}
#endif
