/* SPDX-License-Identifier: GPL-2.0-or-later
 * vdp2_cell_decode.h against an oracle transcribed from vidogl.c
 * Vdp2GetPixel4bpp/8bpp + Vdp2GetAlpha (special priority mode != 2). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/video/opengl/vdp2_cell_decode.h"

static uint8_t vram[0x80000];
static uint32_t rng = 0x9e3779b9u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct { int alpha, coloroffset, transparencyenable, specialcolormode, specialcolorfunction, specialcode; uint32_t paladdr; } Info;

static uint16_t T1ReadWord(uint32_t a) { return (uint16_t)(vram[a] << 8 | vram[a + 1]); }
static uint32_t oracle_alpha(const Info *info, int ccmd, uint8_t dot) {
  uint32_t alpha = (uint32_t)info->alpha;
  if (ccmd == 0) {
    switch (info->specialcolormode) {
    case 1: if (info->specialcolorfunction == 0) alpha = 0xFF; break;
    case 2: if (info->specialcolorfunction == 0) alpha = 0xFF;
            else if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) alpha = 0xFF;
            break;
    }
  } else {
    alpha = 0xFF;
    switch (info->specialcolormode) {
    case 1: if (info->specialcolorfunction == 0) alpha = 0x40; break;
    case 2: if (info->specialcolorfunction == 0) alpha = 0x40;
            else if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) alpha = 0x40;
            break;
    }
  }
  return alpha;
}
static uint32_t *oracle4(const Info *info, int ccmd, uint32_t addr, uint32_t *out) {
  uint16_t dotw = T1ReadWord(addr & 0x7FFFF);
  for (int s = 12; s >= 0; s -= 4) {
    uint8_t dot = (dotw >> s) & 0xF;
    if (!(dot & 0xF) && info->transparencyenable) *out++ = 0;
    else { uint32_t cram = info->coloroffset + ((info->paladdr << 4) | (dot & 0xF));
           *out++ = cram | oracle_alpha(info, ccmd, dot) << 24; }
  }
  return out;
}
static uint32_t *oracle8(const Info *info, int ccmd, uint32_t addr, uint32_t *out) {
  uint16_t dotw = T1ReadWord(addr & 0x7FFFF);
  uint8_t dots[2] = {(uint8_t)(dotw >> 8), (uint8_t)dotw};
  for (int k = 0; k < 2; ++k) {
    uint8_t dot = dots[k];
    if (!(dot & 0xFF) && info->transparencyenable) *out++ = 0;
    else { uint32_t cram = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
           *out++ = cram | oracle_alpha(info, ccmd, dot) << 24; }
  }
  return out;
}

int main(void) {
  for (size_t i = 0; i < sizeof(vram); ++i) vram[i] = (uint8_t)next();
  for (int i = 0; i < 4096; i += 2) vram[i] = vram[i + 1] = 0; /* dense zero dots */
  unsigned long cases = 0, fallbacks = 0;
  for (int t = 0; t < 200000; ++t) {
    Info info = {(int)(next() & 0xFF), (int)(next() & 0x7FF), (int)(next() & 1),
                 (int)(next() % 4), (int)(next() & 1), (int)(next() & 0xFF), next() & 0x7F};
    const int ccmd = next() & 1, bpp8 = next() & 1;
    uint32_t addr = (t % 7 == 0) ? (next() & 0xFFE) : (next() & 0x7FFFE);
    const unsigned words = 1 + next() % 8;
    uint32_t alpha;
    if (!Vdp2CellConstantAlpha(ccmd, info.specialcolormode, info.specialcolorfunction, (uint32_t)info.alpha, &alpha)) {
      assert(info.specialcolormode >= 2); ++fallbacks; continue;
    }
    uint32_t want[64], got[64], *w = want;
    for (unsigned k = 0; k < words; ++k) w = bpp8 ? oracle8(&info, ccmd, addr + 2 * k, w) : oracle4(&info, ccmd, addr + 2 * k, w);
    uint32_t *g = bpp8 ?
      Vdp2DecodeWords8(vram, addr, words, got, (uint32_t)info.coloroffset, info.paladdr << 4, alpha << 24, info.transparencyenable) :
      Vdp2DecodeWords4(vram, addr, words, got, (uint32_t)info.coloroffset, info.paladdr << 4, alpha << 24, info.transparencyenable);
    assert(g - got == w - want);
    for (long k = 0; k < w - want; ++k) assert(got[k] == want[k]);
    ++cases;
  }
  printf("vdp2_cell_decode_test_pass cases=%lu mode2_3_fallbacks=%lu\n", cases, fallbacks);
  return 0;
}
