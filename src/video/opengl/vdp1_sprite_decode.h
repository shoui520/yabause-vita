/* SPDX-License-Identifier: GPL-2.0-or-later
 * Specialized VDP1 sprite decoders for per-sprite cases where every per-pixel
 * input except the dot is constant. Bit-identical to the corresponding
 * Vdp1ReadTexture loops in vidogl.c when their preconditions hold:
 *
 * RGB16 (color mode 5): END (end codes disabled), !MSB_SHADOW, !SPD and
 *   SPCTL bit 5 (RGB sprites). The palette branch, which mutates colorcl via
 *   Vdp1MaskSpritePixel, is then unreachable: dots without MSB are transparent.
 * Bank 64 (color mode 2): END and !MSB_SHADOW. The palette index keeps the
 *   constant colorBank high bits, so its MSB test is per sprite.
 * Bank 16 (color mode 0): always. The loop never changes colorcl or
 *   priority, so each nibble maps through a per-sprite 16-entry table; end
 *   codes (nibble 0xF without END) keep the original per-row counter.
 *
 * VDP1 RAM words are read as T1ReadWord(Vdp1Ram, addr & 0x7FFFF), bytes as
 * T1ReadByte(Vdp1Ram, addr & 0x7FFFF).
 */
#ifndef VDP1_SPRITE_DECODE_H
#define VDP1_SPRITE_DECODE_H
#include <stdint.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include <string.h>

static inline uint32_t Vdp1Texel(uint32_t C, uint32_t A, uint32_t P, uint32_t shadow, uint32_t color) {
  return 0x80000000u | (C << 30) | (A << 27) | (P << 24) | (shadow << 23) | color;
}
static inline uint32_t Vdp1Rgb24(uint16_t t) {
  return ((uint32_t)t & 0x1F) << 3 | ((uint32_t)t & 0x3E0) << 6 | ((uint32_t)t & 0x7C00) << 9;
}

/* One sprite row of RGB16 texels. Returns the advanced output pointer. */
static inline uint32_t *Vdp1DecodeRgb16Row(const uint8_t *vram, uint32_t addr, unsigned width,
                                           uint32_t *out, uint32_t colorcl, uint32_t priority,
                                           uint32_t normal_shadow) {
  const uint32_t shadow_texel = Vdp1Texel(0, 1, priority, 1, 0);
  const uint32_t rgb_base = Vdp1Texel(0, colorcl, priority, 0, 0);
  for (unsigned x = 0; x < width; ++x, addr += 2) {
    uint16_t raw; memcpy(&raw, vram + (addr & 0x7FFFF), 2);
    const uint16_t dot = (uint16_t)((raw >> 8) | (raw << 8));
    if (!(dot & 0x8000)) *out++ = 0;
    else if (normal_shadow != 0 && dot == normal_shadow) *out++ = shadow_texel;
    else *out++ = rgb_base | Vdp1Rgb24(dot);
  }
  return out;
}

/* One sprite row of 64-color bank texels. */
static inline uint32_t *Vdp1DecodeBank64Row(const uint8_t *vram, uint32_t addr, unsigned width,
                                            uint32_t *out, uint32_t color_bank, uint32_t colorcl,
                                            uint32_t priority, uint32_t normal_shadow, int spd,
                                            int rgb_sprites) {
  const uint32_t shadow_texel = Vdp1Texel(1, 0, priority, 1, 0);
  const int direct = (color_bank & 0x8000) && rgb_sprites;
  const uint32_t base = direct ? Vdp1Texel(0, colorcl, priority, 0, 0) : Vdp1Texel(1, colorcl, priority, 0, 0);
  for (unsigned x = 0; x < width; ++x, ++addr) {
    const uint32_t dot = vram[addr & 0x7FFFF];
    const uint32_t index = (dot & 0x3F) | color_bank;
    if (dot == 0 && !spd) *out++ = 0;
    else if (index == normal_shadow) *out++ = shadow_texel;
    else *out++ = base | (direct ? Vdp1Rgb24((uint16_t)index) : index);
  }
  return out;
}

/* 4 bpp LUT (color mode 1). The loop in vidogl.c carries priority, colorcl,
 * shadow and normalshadow from pixel to pixel: palette pixels overwrite all
 * four with Vdp1ProcessSpritePixel(type, entry) (a pure function of the
 * entry), while MSB-shadow, RGB and zero-index pixels read the current
 * priority/colorcl. Per sprite, each of the 16 LUT words therefore reduces
 * to one of four kinds; palette kinds carry their texel and new state.
 * End-code, SPD and endcnt handling is the original's, two dots per byte. */
enum { VDP1_LUT4_MSB, VDP1_LUT4_RGB, VDP1_LUT4_PALETTE, VDP1_LUT4_ZERO };
typedef struct {
  uint8_t kind;
  uint32_t value;                      /* RGB24 (RGB) or texel (PALETTE) */
  int priority, colorcl, shadow, normalshadow; /* PALETTE: state after the pixel */
} Vdp1Lut4Entry;
typedef struct { int priority, colorcl, shadow, normalshadow; } Vdp1Lut4State;

static inline uint32_t Vdp1Lut4Dot(unsigned nibble, const Vdp1Lut4Entry *lut, Vdp1Lut4State *st,
                                   int spd, int end, int *endcnt) {
  if (!end && *endcnt >= 2) return 0;
  if (nibble == 0 && !spd) return 0;
  if (nibble == 0x0F && !end) { ++*endcnt; return 0; }
  const Vdp1Lut4Entry *e = &lut[nibble];
  switch (e->kind) {
  case VDP1_LUT4_MSB: return Vdp1Texel(1, 0, (uint32_t)st->priority, 1, 0);
  case VDP1_LUT4_RGB: return Vdp1Texel(0, (uint32_t)st->colorcl, 0, 0, e->value);
  case VDP1_LUT4_PALETTE:
    st->priority = e->priority; st->colorcl = e->colorcl;
    st->shadow = e->shadow; st->normalshadow = e->normalshadow;
    return e->value;
  default: return Vdp1Texel(1, (uint32_t)st->colorcl, (uint32_t)st->priority, 0, 0);
  }
}
/* One row: while (j < width) two dots per byte, like the original loop.
 * *addr advances by the bytes consumed. */
static inline uint32_t *Vdp1DecodeLut4Row(const uint8_t *vram, uint32_t *addr, unsigned width,
                                          uint32_t *out, const Vdp1Lut4Entry *lut,
                                          Vdp1Lut4State *st, int spd, int end) {
  int endcnt = 0;
  for (unsigned j = 0; j < width; j += 2) {
    const uint32_t dot = vram[*addr & 0x7FFFF];
    *out++ = Vdp1Lut4Dot(dot >> 4, lut, st, spd, end, &endcnt);
    *out++ = Vdp1Lut4Dot(dot & 0xF, lut, st, spd, end, &endcnt);
    ++*addr;
  }
  return out;
}

/* Whole sprite. With end codes disabled (end) and every palette entry
 * leaving the same state, the carried state is constant from the first row
 * that starts in it, and each nibble's texel is a pure function of the
 * nibble: those rows are a branch-free 16-entry table lookup. Other rows use
 * Vdp1DecodeLut4Row. Same outputs, *addr and final *st as row-by-row. */
static inline void Vdp1DecodeLut4Sprite(const uint8_t *vram, uint32_t *addr, unsigned width, unsigned height,
                                        uint32_t **outp, unsigned pad, const Vdp1Lut4Entry *lut,
                                        Vdp1Lut4State *st, int spd, int end) {
  uint32_t *out = *outp;
  int agree = end, have = 0;
  Vdp1Lut4State fixed = *st;
  for (unsigned v = 0; v < 16 && agree; ++v) {
    if (lut[v].kind != VDP1_LUT4_PALETTE || (v == 0 && !spd)) continue;
    const Vdp1Lut4State e = {lut[v].priority, lut[v].colorcl, lut[v].shadow, lut[v].normalshadow};
    if (!have) { fixed = e; have = 1; }
    else if (e.priority != fixed.priority || e.colorcl != fixed.colorcl ||
             e.shadow != fixed.shadow || e.normalshadow != fixed.normalshadow) agree = 0;
  }
  uint32_t table[16];
  int table_ok = 0;
  for (unsigned i = 0; i < height; ++i) {
    if (agree && !table_ok && (!have || (st->priority == fixed.priority && st->colorcl == fixed.colorcl &&
                                         st->shadow == fixed.shadow && st->normalshadow == fixed.normalshadow))) {
      for (unsigned n = 0; n < 16; ++n) {
        Vdp1Lut4State copy = *st; int endcnt = 0;
        table[n] = Vdp1Lut4Dot(n, lut, &copy, spd, end, &endcnt);
      }
      table_ok = 1;
    }
    if (table_ok) {
      uint32_t a = *addr;
      unsigned j = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
      /* Two bytes -> four texels per 16-byte store (scalar stores into atlas
       * lines not in cache measured ~2x slower on the Vita). */
      for (; j + 4 <= width; j += 4, a += 2) {
        const uint32_t d0 = vram[a & 0x7FFFF], d1 = vram[(a + 1) & 0x7FFFF];
        uint32x4_t v = vdupq_n_u32(table[d0 >> 4]);
        v = vsetq_lane_u32(table[d0 & 0xF], v, 1);
        v = vsetq_lane_u32(table[d1 >> 4], v, 2);
        v = vsetq_lane_u32(table[d1 & 0xF], v, 3);
        vst1q_u32(out, v);
        out += 4;
      }
#endif
      for (; j < width; j += 2, ++a) {
        const uint32_t dot = vram[a & 0x7FFFF];
        out[0] = table[dot >> 4]; out[1] = table[dot & 0xF];
        out += 2;
      }
      *addr = a;
    } else {
      out = Vdp1DecodeLut4Row(vram, addr, width, out, lut, st, spd, end);
    }
    out += pad;
  }
  *outp = out;
}
/* Color mode 0. table[n] is the original loop's texel for nibble n; with end
 * codes enabled (!end) nibble 0xF is an end code instead. Each row consumes
 * (width + 1) / 2 bytes and writes two texels per byte, then skips pad. */
static inline void Vdp1DecodeBank4Sprite(const uint8_t *vram, uint32_t addr, unsigned width, unsigned height,
                                         uint32_t **outp, unsigned pad, const uint32_t *table, int end) {
  uint32_t *out = *outp;
  const unsigned bytes = (width + 1) / 2;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  uint8x8x2_t plane[4];
  for (int b = 0; b < 4; ++b) {
    uint8_t t[16];
    for (int n = 0; n < 16; ++n) t[n] = (uint8_t)(table[n] >> (8 * b));
    plane[b].val[0] = vld1_u8(t);
    plane[b].val[1] = vld1_u8(t + 8);
  }
#endif
  for (unsigned i = 0; i < height; ++i) {
    unsigned k = 0;
    if (end) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
      /* Eight bytes -> sixteen texels: byte-plane table lookups stored
       * interleaved as little-endian words. */
      for (; k + 8 <= bytes && (addr & 0x7FFFF) + 8 <= 0x80000; k += 8, addr += 8) {
        const uint8x8_t d = vld1_u8(vram + (addr & 0x7FFFF));
        const uint8x8x2_t z = vzip_u8(vshr_n_u8(d, 4), vand_u8(d, vdup_n_u8(0xF)));
        for (int h = 0; h < 2; ++h) {
          uint8x8x4_t o;
          o.val[0] = vtbl2_u8(plane[0], z.val[h]);
          o.val[1] = vtbl2_u8(plane[1], z.val[h]);
          o.val[2] = vtbl2_u8(plane[2], z.val[h]);
          o.val[3] = vtbl2_u8(plane[3], z.val[h]);
          vst4_u8((uint8_t *)out, o);
          out += 8;
        }
      }
#endif
      for (; k < bytes; ++k, ++addr) {
        const uint32_t dot = vram[addr & 0x7FFFF];
        out[0] = table[dot >> 4];
        out[1] = table[dot & 0xF];
        out += 2;
      }
    } else {
      int endcnt = 0;
      for (; k < bytes; ++k, ++addr) {
        const uint32_t dot = vram[addr & 0x7FFFF];
        for (int s = 4; s >= 0; s -= 4) {
          const uint32_t n = (dot >> s) & 0xF;
          if (endcnt >= 2) *out++ = 0;
          else if (n == 0xF) { *out++ = 0; ++endcnt; }
          else *out++ = table[n];
        }
      }
    }
    out += pad;
  }
  *outp = out;
}
#endif
