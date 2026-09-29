/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test: VITA_VDP1_FAST_BANK256 decode vs the original 8bpp
 * 256-color bank loop (vidogl.c Vdp1ReadTexture case 4), over random VRAM,
 * color banks, SPD/END, normal-shadow values, all 16 SPCTL mask types,
 * initial colorcl values and RGB-sprite mode. Vdp1MaskSpritePixel is
 * extracted verbatim from vidogl.c by the test script. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define INLINE inline
static inline u32 VDP1COLOR(u32 C, u32 A, u32 P, u32 shadow, u32 color) {
  return 0x80000000 | (C << 30) | (A << 27) | (P << 24) | (shadow << 23) | color;
}
static inline u32 VDP1COLOR16TO24(u16 temp) {
  return (((u32)temp & 0x1F) << 3 | ((u32)temp & 0x3E0) << 6 | ((u32)temp & 0x7C00) << 9);
}
#include "vdp1_mask_extract.inc"
static u8 vram[0x80000];
static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void reference(u32 charAddr, int w, int h, u32 *out, int stride, u32 cmdcolr, int SPD, int END,
                      int nromal_shadow, int spctl, int colorcl, int priority) {
  u32 colorBank = cmdcolr & 0xFF00;
  for (int i = 0; i < h; i++) {
    int endcnt = 0;
    for (int j = 0; j < w; j++) {
      u32 dot = vram[charAddr & 0x7FFFF];
      charAddr++;
      if (endcnt >= 2) *out++ = 0x0;
      else if ((dot == 0) && !SPD) *out++ = 0x00;
      else if ((dot == 0xFF) && !END) { *out++ = 0x0; endcnt++; }
      else if ((int)(dot | colorBank) == nromal_shadow) *out++ = VDP1COLOR(1, 0, priority, 1, 0);
      else {
        int colorindex = (int)(dot | colorBank);
        if ((colorindex & 0x8000) && (spctl & 0x20))
          *out++ = VDP1COLOR(0, colorcl, priority, 0, VDP1COLOR16TO24(colorindex));
        else {
          Vdp1MaskSpritePixel(spctl & 0xF, (u16 *)&colorindex, &colorcl);
          *out++ = VDP1COLOR(1, colorcl, priority, 0, colorindex);
        }
      }
    }
    out += stride;
  }
}

/* The fast path, extracted verbatim from vidogl.c (BANK256_BEGIN..END), in a
 * harness supplying the same names. It returns after decoding. */
typedef struct { u32 *textdata; int w; } YglTexture;
typedef struct { int w, h; } YglSprite;
typedef struct { u16 CMDCOLR; } vdp1cmd_struct;
typedef struct { u16 SPCTL; } Vdp2Regs_t;
static void fast(u32 charAddr, int w, int h, u32 *out, int stride, u32 cmdcolr, int SPD, int END,
                 int nromal_shadow, int spctl, int colorcl, int priority) {
  u8 *Vdp1Ram = vram;
  YglTexture tex = {out, stride}, *texture = &tex;
  YglSprite spr = {w, h}, *sprite = &spr;
  vdp1cmd_struct c = {(u16)cmdcolr}, *cmd = &c;
  Vdp2Regs_t regs = {(u16)spctl}, *fixVdp2Regs = &regs;
  const int mode = 4, MSB_SHADOW = 0, rgb_sprites = (spctl & 0x20) != 0;
  (void)mode;
#include "bank256_extract.inc"
}

int main(void) {
  static u32 a[64 * 72], b[64 * 72];
  unsigned cases = 0;
  for (int c = 0; c < 200000; ++c) {
    const int mode = rnd() % 4;   /* dense zeros/0xFF, few distinct, random, all */
    for (unsigned k = 0; k < 4096; ++k) {
      const u32 r = rnd();
      vram[k] = mode == 0 ? (u8)((r & 3) == 0 ? 0 : (r & 3) == 1 ? 0xFF : r >> 4) :
                mode == 1 ? (u8)(r % 5) : (u8)r;
    }
    const int w = 1 + rnd() % 64, h = 1 + rnd() % 8, stride = rnd() % 8;
    const u32 cmdcolr = rnd() & 0xFFFF, addr = 0x7FF00 + (rnd() % 0x300);
    for (unsigned k = 0; k < 0x80000; k += 0x7F000) (void)k;
    /* place data near the 512 KiB wrap sometimes */
    memcpy(vram + 0x7F000, vram, 0x1000);
    const int SPD = rnd() & 1, END = rnd() & 1, spctl = rnd() & 0x3F;
    const int colorcl = (rnd() & 1) ? (int)(rnd() % 8) : (int)(rnd() % 64);
    const int priority = rnd() % 8;
    const int ns = (rnd() & 1) ? (int)(cmdcolr & 0xFF00) | (int)(vram[rnd() % 64]) : (int)(rnd() & 0xFFFF);
    const u32 start = (rnd() & 1) ? addr : rnd() % 4000;
    memset(a, 0xAB, sizeof(a)); memset(b, 0xAB, sizeof(b));
    reference(start, w, h, a, stride, cmdcolr, SPD, END, ns, spctl, colorcl, priority);
    fast(start, w, h, b, stride, cmdcolr, SPD, END, ns, spctl, colorcl, priority);
    if (memcmp(a, b, sizeof(a))) {
      printf("FAIL case %d w=%d h=%d spctl=%x cc=%d SPD=%d END=%d\n", c, w, h, spctl, colorcl, SPD, END);
      return 1;
    }
    ++cases;
  }
  printf("VDP1 bank256: %u sprites identical to the original loop\n", cases);
  return 0;
}
