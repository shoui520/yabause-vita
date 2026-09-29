/* SPDX-License-Identifier: GPL-2.0-or-later
 * Specialized VDP2 palette-cell decoders for the common case where every
 * per-pixel input except the dot itself is constant for the cell:
 *   - special priority is not per-dot (specialprimode != 2), and
 *   - special color calculation mode is 0 or 1 (alpha depends only on CCMD,
 *     the mode and the cell's special color function bit).
 * Output texels are bit-identical to Vdp2GetPixel4bpp/8bpp in vidogl.c:
 *   transparent dot 0 -> 0, else (coloroffset + ((paladdr << 4) | dot)) | alpha << 24.
 * VRAM words are read exactly as T1ReadWord(Vdp2Ram, addr & 0x7FFFF).
 */
#ifndef VDP2_CELL_DECODE_H
#define VDP2_CELL_DECODE_H
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* Returns 1 and stores the alpha when it is constant for the cell. Mirrors
 * Vdp2GetAlpha for modes 0 and 1; other modes read the dot or CRAM. */
static inline int Vdp2CellConstantAlpha(int ccmd, int specialcolormode,
                                        int specialcolorfunction, uint32_t alpha,
                                        uint32_t *out) {
  if (ccmd == 0) {
    if (specialcolormode == 0) { *out = alpha; return 1; }
    if (specialcolormode == 1) { *out = specialcolorfunction == 0 ? 0xFF : alpha; return 1; }
  } else {
    if (specialcolormode == 0) { *out = 0xFF; return 1; }
    if (specialcolormode == 1) { *out = specialcolorfunction == 0 ? 0x40 : 0xFF; return 1; }
  }
  return 0;
}

static inline uint16_t Vdp2CellWord(const uint8_t *vram, uint32_t addr) {
  uint16_t raw;
  memcpy(&raw, vram + (addr & 0x7FFFF), sizeof(raw));
  return (uint16_t)((raw >> 8) | (raw << 8)); /* Saturn byte order */
}

/* 4 bpp: `words` VRAM words (four dots each). Returns the advanced output. */
static inline uint32_t *Vdp2DecodeWords4(const uint8_t *vram, uint32_t addr, unsigned words,
                                         uint32_t *out, uint32_t coloroffset, uint32_t pal,
                                         uint32_t alpha_bits, int transparent) {
  for (unsigned w = 0; w < words; ++w, addr += 2) {
    const uint32_t dotw = Vdp2CellWord(vram, addr);
    for (int shift = 12; shift >= 0; shift -= 4) {
      const uint32_t dot = (dotw >> shift) & 0xF;
      *out++ = (!dot && transparent) ? 0u : ((coloroffset + (pal | dot)) | alpha_bits);
    }
  }
  return out;
}

/* 8 bpp: `words` VRAM words (two dots each). */
static inline uint32_t *Vdp2DecodeWords8(const uint8_t *vram, uint32_t addr, unsigned words,
                                         uint32_t *out, uint32_t coloroffset, uint32_t pal,
                                         uint32_t alpha_bits, int transparent) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  /* 8 dots per step. Consecutive words are consecutive bytes in dot order
   * (big-endian words, high byte first), unless the 512 KiB mask wraps. */
  while (words >= 4 && (addr & 0x7FFFF) + 8 <= 0x80000) {
    const uint16x8_t dots16 = vmovl_u8(vld1_u8(vram + (addr & 0x7FFFF)));
    const uint32x4_t pal4 = vdupq_n_u32(pal), co4 = vdupq_n_u32(coloroffset), a4 = vdupq_n_u32(alpha_bits);
    uint32x4_t lo = vmovl_u16(vget_low_u16(dots16)), hi = vmovl_u16(vget_high_u16(dots16));
    uint32x4_t tlo = vorrq_u32(vaddq_u32(co4, vorrq_u32(pal4, lo)), a4);
    uint32x4_t thi = vorrq_u32(vaddq_u32(co4, vorrq_u32(pal4, hi)), a4);
    if (transparent) {
      tlo = vbicq_u32(tlo, vceqq_u32(lo, vdupq_n_u32(0)));
      thi = vbicq_u32(thi, vceqq_u32(hi, vdupq_n_u32(0)));
    }
    vst1q_u32(out, tlo); vst1q_u32(out + 4, thi);
    out += 8; addr += 8; words -= 4;
  }
#endif
  for (unsigned w = 0; w < words; ++w, addr += 2) {
    const uint32_t dotw = Vdp2CellWord(vram, addr);
    const uint32_t hi = dotw >> 8, lo = dotw & 0xFF;
    *out++ = (!hi && transparent) ? 0u : ((coloroffset + (pal | hi)) | alpha_bits);
    *out++ = (!lo && transparent) ? 0u : ((coloroffset + (pal | lo)) | alpha_bits);
  }
  return out;
}

/* Special colour calculation mode 3 (per dot: the MSB of the dot's colour
 * RAM word, as Vdp2GetAlpha reads it through Vdp2ColorRamGetColorRaw).
 * cram_shift is 1 for colour RAM modes 0/1 and 2 for mode 2; a negative
 * shift (other modes) reads 0. The texel carries alpha_msb when the MSB is
 * set, else alpha_clear. */
static inline uint32_t Vdp2CellMsbTexel(const uint8_t *cram, int cram_shift, uint32_t cramindex,
                                        uint32_t alpha_msb, uint32_t alpha_clear) {
  uint16_t word = 0;
  if (cram_shift >= 0) memcpy(&word, cram + ((cramindex << cram_shift) & 0xFFF), 2);
  return cramindex | ((word & 0x8000) ? alpha_msb : alpha_clear);
}
static inline uint32_t *Vdp2DecodeWords4Msb(const uint8_t *vram, uint32_t addr, unsigned words, uint32_t *out,
                                            uint32_t coloroffset, uint32_t pal, int transparent,
                                            const uint8_t *cram, int cram_shift,
                                            uint32_t alpha_msb, uint32_t alpha_clear) {
  for (unsigned w = 0; w < words; ++w, addr += 2) {
    const uint32_t dotw = Vdp2CellWord(vram, addr);
    for (int shift = 12; shift >= 0; shift -= 4) {
      const uint32_t dot = (dotw >> shift) & 0xF;
      *out++ = (!dot && transparent) ? 0u :
        Vdp2CellMsbTexel(cram, cram_shift, coloroffset + (pal | dot), alpha_msb, alpha_clear);
    }
  }
  return out;
}
static inline uint32_t *Vdp2DecodeWords8Msb(const uint8_t *vram, uint32_t addr, unsigned words, uint32_t *out,
                                            uint32_t coloroffset, uint32_t pal, int transparent,
                                            const uint8_t *cram, int cram_shift,
                                            uint32_t alpha_msb, uint32_t alpha_clear) {
  for (unsigned w = 0; w < words; ++w, addr += 2) {
    const uint32_t dotw = Vdp2CellWord(vram, addr);
    const uint32_t hi = dotw >> 8, lo = dotw & 0xFF;
    *out++ = (!hi && transparent) ? 0u :
      Vdp2CellMsbTexel(cram, cram_shift, coloroffset + (pal | hi), alpha_msb, alpha_clear);
    *out++ = (!lo && transparent) ? 0u :
      Vdp2CellMsbTexel(cram, cram_shift, coloroffset + (pal | lo), alpha_msb, alpha_clear);
  }
  return out;
}

/* Whether the fast decode writes only alpha-0 texels for `cells` consecutive
 * cells of `cell_bytes` each at addr (no 512 KiB wrap), decided from the
 * source instead of reading the texels back. A cell in a disabled bank is
 * zero-filled; otherwise a texel is 0 for a transparent dot 0 and carries
 * alpha_bits for any other dot. Returns -1 when alpha_bits is 0 (the alpha
 * byte then depends on the colour sum), else 1 (all alpha 0) or 0. */
static inline int Vdp2CellsTransparent(const uint8_t *vram, uint32_t addr, unsigned cells,
                                       unsigned cell_bytes, const uint16_t *banks,
                                       uint32_t alpha_bits, int transparent) {
  if (!(alpha_bits & 0xFF000000u)) return -1;
  for (unsigned c = 0; c < cells; ++c, addr += cell_bytes) {
    if (banks[addr >> 17] == 0) continue;
    if (!transparent) return 0;
    for (unsigned b = 0; b < cell_bytes; ++b) if (vram[addr + b]) return 0;
  }
  return 1;
}
#endif
