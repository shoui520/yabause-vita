/* SPDX-License-Identifier: GPL-2.0-or-later
 * vdp1_sprite_decode.h against the Vdp1ReadTexture mode 5 / mode 2 / mode 0 loops
 * (vidogl.c), transcribed including their end-code and palette branches. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/video/opengl/vdp1_sprite_decode.h"

static uint8_t vram[0x80000];
static uint32_t rng = 0x2545f491u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint16_t rw(uint32_t a) { return (uint16_t)(vram[a] << 8 | vram[a + 1]); }

/* Vdp1MaskSpritePixel types reachable only on paths the fast decoders reject. */
static void oracle_rgb16(uint32_t addr, unsigned w, uint32_t *out, int spd, int end, int msb,
                         int colorcl, int priority, int normal_shadow, unsigned spctl) {
  int endcnt = 0;
  for (unsigned j = 0; j < w; ++j) {
    uint32_t dot = rw(addr & 0x7FFFF); addr += 2;
    if (endcnt == 2) *out++ = 0;
    else if (!(dot & 0x8000) && !spd) *out++ = 0;
    else if (dot == 0x7FFF && !end) { *out++ = 0; endcnt++; }
    else if (msb || (normal_shadow != 0 && dot == (uint32_t)normal_shadow)) *out++ = Vdp1Texel(0, 1, priority, 1, 0);
    else if ((dot & 0x8000) && (spctl & 0x20)) *out++ = Vdp1Texel(0, colorcl, priority, 0, Vdp1Rgb24((uint16_t)dot));
    else assert(0 && "palette branch must be unreachable under the fast preconditions");
  }
}
static void oracle_bank64(uint32_t addr, unsigned w, uint32_t *out, int spd, int end, int msb,
                          uint32_t colorBank, int colorcl, int priority, int normal_shadow, unsigned spctl) {
  int endcnt = 0;
  for (unsigned j = 0; j < w; ++j) {
    uint32_t dot = vram[addr & 0x7FFFF]; addr++;
    if (endcnt >= 2) *out++ = 0;
    else if (dot == 0 && !spd) *out++ = 0;
    else if (dot == 0xFF && !end) { *out++ = 0; endcnt++; }
    else if (msb) *out++ = Vdp1Texel(1, 0, priority, 1, 0);
    else if ((int)((dot & 0x3F) | colorBank) == normal_shadow) *out++ = Vdp1Texel(1, 0, priority, 1, 0);
    else {
      const int colorindex = (int)((dot & 0x3F) | colorBank);
      if ((colorindex & 0x8000) && (spctl & 0x20)) *out++ = Vdp1Texel(0, colorcl, priority, 0, Vdp1Rgb24((uint16_t)colorindex));
      else *out++ = Vdp1Texel(1, colorcl, priority, 0, (uint32_t)colorindex);
    }
  }
}

/* The Vdp1ReadTexture case 0 (4 bpp bank) loop, transcribed. */
static uint32_t oracle_bank4(uint32_t charAddr, unsigned w, unsigned h, uint32_t *out, unsigned pad,
                             int spd, int end, int msb, uint32_t colorBank, int colorcl, int priority,
                             int normal_shadow, unsigned spctl) {
  for (unsigned i = 0; i < h; i++) {
    unsigned j = 0; int endcnt = 0;
    while (j < w) {
      const uint32_t dot = vram[charAddr & 0x7FFFF];
      for (int half = 0; half < 2; ++half) {
        const uint32_t nib = half ? (dot & 0xF) : (dot >> 4);
        if (endcnt >= 2) *out++ = 0;
        else if (nib == 0 && !spd) *out++ = 0;
        else if (nib == 0x0F && !end) { *out++ = 0; endcnt++; }
        else if (msb) *out++ = Vdp1Texel(1, 0, (uint32_t)priority, 1, 0);
        else if ((int)(nib | colorBank) == normal_shadow) *out++ = Vdp1Texel(1, 0, (uint32_t)priority, 1, 0);
        else {
          const int colorindex = (int)(nib | colorBank);
          if ((colorindex & 0x8000) && (spctl & 0x20)) *out++ = Vdp1Texel(0, (uint32_t)colorcl, (uint32_t)priority, 0, Vdp1Rgb24((uint16_t)colorindex));
          else *out++ = Vdp1Texel(1, (uint32_t)colorcl, (uint32_t)priority, 0, (uint32_t)colorindex);
        }
        j += 1;
      }
      charAddr += 1;
    }
    out += pad;
  }
  return charAddr;
}

/* Deterministic stand-in for Vdp1ProcessSpritePixel: all four outputs and
 * the pixel are a pure function of (type, pixel), like the real one. */
static void fake_process(int type, uint16_t *pixel, int *shadow, int *normalshadow, int *priority, int *colorcalc) {
  const uint32_t h = (uint32_t)(*pixel * 2654435761u) ^ (uint32_t)type * 97u;
  *shadow = (h >> 3) % 7 == 0; *normalshadow = (h >> 7) % 11 == 0;
  *priority = (int)((h >> 11) & 7); *colorcalc = (int)((h >> 15) & 7);
  *pixel = (uint16_t)(*pixel & 0x7FF);
}
/* The Vdp1ReadTexture case 1 loop, transcribed. */
static uint32_t oracle_lut4(uint32_t charAddr, unsigned w, unsigned h, uint32_t *out, unsigned pitch_pad,
                            int spd, int end, int msb_shadow, int rgb_sprites, int type, uint32_t colorLut,
                            int *priority, int *colorcl, int *shadow, int *normalshadow) {
  for (unsigned i = 0; i < h; i++) {
    unsigned j = 0; int endcnt = 0;
    while (j < w) {
      const uint32_t dot = vram[charAddr & 0x7FFFF];
      for (int half = 0; half < 2; ++half) {
        const unsigned nib = half ? (dot & 0xF) : (dot >> 4);
        if (!end && endcnt >= 2) *out++ = 0;
        else if (nib == 0 && !spd) *out++ = 0;
        else if (nib == 0x0F && !end) { *out++ = 0; endcnt++; }
        else {
          const int colorindex = rw((nib * 2 + colorLut) & 0x7FFFF);
          if ((colorindex & 0x8000) && msb_shadow) *out++ = Vdp1Texel(1, 0, (uint32_t)*priority, 1, 0);
          else if (colorindex != 0) {
            if ((colorindex & 0x8000) && rgb_sprites) *out++ = Vdp1Texel(0, (uint32_t)*colorcl, 0, 0, Vdp1Rgb24((uint16_t)colorindex));
            else {
              uint16_t temp = (uint16_t)colorindex;
              fake_process(type, &temp, shadow, normalshadow, priority, colorcl);
              if (*shadow || *normalshadow) *out++ = Vdp1Texel(1, 0, (uint32_t)*priority, 1, 0);
              else *out++ = Vdp1Texel(1, (uint32_t)*colorcl, (uint32_t)*priority, 0, temp);
            }
          } else *out++ = Vdp1Texel(1, (uint32_t)*colorcl, (uint32_t)*priority, 0, 0);
        }
        j += 1;
      }
      charAddr += 1;
    }
    out += pitch_pad;
  }
  return charAddr;
}

int main(void) {
  for (size_t i = 0; i < sizeof(vram); ++i) vram[i] = (uint8_t)next();
  for (size_t i = 0; i < 8192; ++i) vram[i] = (uint8_t)(next() & 1 ? 0 : 0x80); /* dense zero / MSB-only */
  unsigned long cases = 0;
  for (int t = 0; t < 200000; ++t) {
    const unsigned w = 1 + next() % 64;
    const uint32_t addr = (t % 5 == 0) ? (next() & 0x1FFE) : (next() & 0x7FFFE);
    const int colorcl = (int)(next() & 7), priority = (int)(next() & 7);
    uint32_t want[64], got[64];
    if (t & 1) {
      /* RGB16 fast preconditions: END, !MSB, !SPD, SPCTL bit5. */
      int ns = (int)(next() % 4 == 0 ? 0 : (0x8000 | (next() & 0x7FFF)));
      if (t % 3 == 0) ns = (int)rw(addr & 0x7FFFF) | 0x8000; /* force matches */
      oracle_rgb16(addr, w, want, 0, 1, 0, colorcl, priority, ns, 0x25);
      Vdp1DecodeRgb16Row(vram, addr, w, got, (uint32_t)colorcl, (uint32_t)priority, (uint32_t)ns);
    } else {
      const uint32_t bank = next() & 0xFFC0;
      const int spd = (int)(next() & 1);
      const unsigned spctl = next() & 0x3F;
      int ns = (int)(next() % 3 == 0 ? ((vram[addr & 0x7FFFF] & 0x3F) | bank) : (next() & 0xFFFF));
      if (t % 7 == 0) ns = -1;
      oracle_bank64(addr, w, want, spd, 1, 0, bank, colorcl, priority, ns, spctl);
      Vdp1DecodeBank64Row(vram, addr, w, got, bank, (uint32_t)colorcl, (uint32_t)priority, (uint32_t)ns, spd, (spctl & 0x20) != 0);
    }
    for (unsigned k = 0; k < w; ++k) assert(got[k] == want[k]);
    ++cases;
  }
  /* LUT4: whole sprites, state carried across pixels and rows. */
  for (int t = 0; t < 60000; ++t) {
    static uint32_t want[80 * 20], got[80 * 20];
    const unsigned w = 1 + next() % 33, h = 1 + next() % 8, pad = next() % 4;
    const uint32_t charAddr = next() & 0x7FFFF, colorLut = (next() & 0xFFFF) * 8;
    const int spd = (int)(next() & 1), end = (int)(next() & 1), msb = (int)(next() % 4 == 0);
    const int rgb = (int)(next() & 1), type = (int)(next() & 15);
    for (unsigned v = 0; v < 16; ++v) { /* LUT words: zero, MSB set and palette mixes */
      const uint32_t a = (v * 2 + colorLut) & 0x7FFFF;
      const unsigned k = next() % 4;
      static uint16_t fixed_pal, fixed_msb;
      if (v == 0) { fixed_pal = (uint16_t)(next() & 0x7FFF); fixed_msb = (uint16_t)(0x8000 | next()); }
      const uint16_t word = (t & 1) ? (k == 0 ? 0 : k == 1 ? fixed_msb : fixed_pal) :
                            k == 0 ? 0 : k == 1 ? (uint16_t)(0x8000 | next()) : (uint16_t)(next() & 0x7FFF);
      vram[a] = (uint8_t)(word >> 8); vram[(a + 1) & 0x7FFFF] = (uint8_t)word;
    }
    int p0 = (int)(next() & 7), c0 = (int)(next() & 7), s0 = (int)(next() & 1), n0 = (int)(next() & 1);
    int pa = p0, ca = c0, sa = s0, na = n0;
    memset(want, 0xAB, sizeof(want)); memset(got, 0xAB, sizeof(got));
    const uint32_t end_a = oracle_lut4(charAddr, w, h, want, pad, spd, end, msb, rgb, type, colorLut, &pa, &ca, &sa, &na);
    Vdp1Lut4Entry lut[16];
    for (unsigned v = 0; v < 16; ++v) {
      const int colorindex = rw((v * 2 + colorLut) & 0x7FFFF);
      if ((colorindex & 0x8000) && msb) lut[v].kind = VDP1_LUT4_MSB;
      else if (colorindex == 0) lut[v].kind = VDP1_LUT4_ZERO;
      else if ((colorindex & 0x8000) && rgb) { lut[v].kind = VDP1_LUT4_RGB; lut[v].value = Vdp1Rgb24((uint16_t)colorindex); }
      else {
        uint16_t temp = (uint16_t)colorindex; int sh, nsh, pr, cc;
        fake_process(type, &temp, &sh, &nsh, &pr, &cc);
        lut[v].kind = VDP1_LUT4_PALETTE; lut[v].priority = pr; lut[v].colorcl = cc;
        lut[v].shadow = sh; lut[v].normalshadow = nsh;
        lut[v].value = (sh || nsh) ? Vdp1Texel(1, 0, (uint32_t)pr, 1, 0) : Vdp1Texel(1, (uint32_t)cc, (uint32_t)pr, 0, temp);
      }
    }
    Vdp1Lut4State st = {p0, c0, s0, n0};
    uint32_t addr = charAddr, *o = got;
    for (unsigned i = 0; i < h; ++i) { o = Vdp1DecodeLut4Row(vram, &addr, w, o, lut, &st, spd, end); o += pad; }
    assert(addr == end_a);
    assert(!memcmp(want, got, sizeof(want)));
    assert(st.priority == pa && st.colorcl == ca && st.shadow == sa && st.normalshadow == na);
    /* Whole-sprite table path: same outputs, address and final state. */
    memset(got, 0xAB, sizeof(got));
    Vdp1Lut4State st2 = {p0, c0, s0, n0};
    uint32_t addr2 = charAddr, *o2 = got;
    Vdp1DecodeLut4Sprite(vram, &addr2, w, h, &o2, pad, lut, &st2, spd, end);
    assert(addr2 == end_a);
    assert(!memcmp(want, got, sizeof(want)));
    assert(st2.priority == pa && st2.colorcl == ca && st2.shadow == sa && st2.normalshadow == na);
    ++cases;
  }
  /* Bank 4: the per-sprite table as vidogl.c builds it, end codes, odd widths, wrap. */
  for (int t = 0; t < 60000; ++t) {
    static uint32_t want[130 * 12], got[130 * 12];
    const unsigned w = 1 + next() % 72, h = 1 + next() % 10, pad = next() % 5;
    const uint32_t charAddr = (t % 11 == 0) ? (0x80000 - (next() % 64)) : (next() & 0x7FFFF);
    const int spd = (int)(next() & 1), end = (int)(next() % 4 != 0), msb = (int)(next() % 6 == 0);
    const uint32_t bank = next() & 0xFFF0;
    const int colorcl = (int)(next() & 7), priority = (int)(next() & 7);
    const unsigned spctl = next() & 0x3F;
    int ns = (int)(next() % 3 == 0 ? ((next() & 0xF) | bank) : (next() & 0xFFFF));
    if (t % 7 == 0) ns = -1;
    memset(want, 0xAB, sizeof(want)); memset(got, 0xAB, sizeof(got));
    oracle_bank4(charAddr, w, h, want, pad, spd, end, msb, bank, colorcl, priority, ns, spctl);
    uint32_t table[16];
    for (unsigned n = 0; n < 16; ++n) {
      const int colorindex = (int)(n | bank);
      if (n == 0 && !spd) table[n] = 0;
      else if (n == 0xF && !end) table[n] = 0;
      else if (msb || colorindex == ns) table[n] = Vdp1Texel(1, 0, (uint32_t)priority, 1, 0);
      else if ((colorindex & 0x8000) && (spctl & 0x20)) table[n] = Vdp1Texel(0, (uint32_t)colorcl, (uint32_t)priority, 0, Vdp1Rgb24((uint16_t)colorindex));
      else table[n] = Vdp1Texel(1, (uint32_t)colorcl, (uint32_t)priority, 0, (uint32_t)colorindex);
    }
    uint32_t *o = got;
    Vdp1DecodeBank4Sprite(vram, charAddr, w, h, &o, pad, table, end);
    assert(o == got + h * (((w + 1) / 2) * 2 + pad));
    assert(!memcmp(want, got, sizeof(want)));
    ++cases;
  }
  printf("vdp1_sprite_decode_test_pass cases=%lu\n", cases);
  return 0;
}
