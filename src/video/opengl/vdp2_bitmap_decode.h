/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct-color VDP2 bitmap rows (VITA_VDP2_FAST_BITMAP), bit-identical to
 * Vdp2GetPixel16bppbmp / Vdp2GetPixel32bppbmp in vidogl.c for a row whose
 * source bytes lie inside VRAM (no 512 KiB wrap):
 *   16 bpp: dot = big-endian word; transparent (0) if !(dot & 0x8000) && transparent,
 *           else alpha << 24 | (dot & 0x1F) << 3 | (dot & 0x3E0) << 6 | (dot & 0x7C00) << 9
 *   32 bpp: dot1:dot2 = big-endian words; transparent if !(dot1 & 0x8000) && transparent,
 *           else alpha << 24 | (dot1 & 0xFF) << 16 | dot2
 * abits = alpha << 24. Returns dst + n. */
#ifndef VDP2_BITMAP_DECODE_H
#define VDP2_BITMAP_DECODE_H
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static inline uint32_t *Vdp2DecodeRow32(const uint8_t *src, unsigned n, uint32_t *dst,
                                        uint32_t abits, int transparent) {
  unsigned i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  const uint32x4_t low = vdupq_n_u32(0x00FFFFFFu), a = vdupq_n_u32(abits);
  const uint32x4_t keep_all = vdupq_n_u32(transparent ? 0u : 0xFFFFFFFFu);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t w = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(src + 4 * i)));  /* dot1:dot2 */
    const uint32x4_t keep = vorrq_u32(vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(w), 31)), keep_all);
    vst1q_u32(dst + i, vandq_u32(vorrq_u32(vandq_u32(w, low), a), keep));
  }
#endif
  for (; i < n; ++i) {
    const uint8_t *p = src + 4 * i;
    const uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    dst[i] = (!(w & 0x80000000u) && transparent) ? 0 : ((w & 0x00FFFFFFu) | abits);
  }
  return dst + n;
}

static inline uint32_t *Vdp2DecodeRow16(const uint8_t *src, unsigned n, uint32_t *dst,
                                        uint32_t abits, int transparent) {
  unsigned i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  const uint32x4_t a = vdupq_n_u32(abits);
  const uint32x4_t keep_all = vdupq_n_u32(transparent ? 0u : 0xFFFFFFFFu);
  const uint32x4_t m5 = vdupq_n_u32(0x1Fu), m10 = vdupq_n_u32(0x3E0u), m15 = vdupq_n_u32(0x7C00u);
  for (; i + 8 <= n; i += 8) {
    const uint16x8_t d = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(src + 2 * i)));
    const uint32x4_t h[2] = {vmovl_u16(vget_low_u16(d)), vmovl_u16(vget_high_u16(d))};
    for (int k = 0; k < 2; ++k) {
      uint32x4_t c = vorrq_u32(a, vshlq_n_u32(vandq_u32(h[k], m5), 3));
      c = vorrq_u32(c, vshlq_n_u32(vandq_u32(h[k], m10), 6));
      c = vorrq_u32(c, vshlq_n_u32(vandq_u32(h[k], m15), 9));
      const uint32x4_t keep = vorrq_u32(vreinterpretq_u32_s32(
        vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(h[k], 16)), 31)), keep_all);
      vst1q_u32(dst + i + 4 * k, vandq_u32(c, keep));
    }
  }
#endif
  for (; i < n; ++i) {
    const uint32_t dot = (uint32_t)src[2 * i] << 8 | src[2 * i + 1];
    dst[i] = (!(dot & 0x8000u) && transparent) ? 0
           : (abits | (dot & 0x1Fu) << 3 | (dot & 0x3E0u) << 6 | (dot & 0x7C00u) << 9);
  }
  return dst + n;
}
#endif
