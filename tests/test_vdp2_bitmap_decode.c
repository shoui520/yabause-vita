/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct-color bitmap row decoders (vdp2_bitmap_decode.h) against the
 * per-pixel vidogl.c expressions (Vdp2GetPixel16bppbmp / 32bppbmp with the
 * little-endian T1ReadWord and SAT2YAB1/SAT2YAB2 of ygl.h). Scalar on x86,
 * NEON on ARM (qemu / Cortex-A9). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/video/opengl/vdp2_bitmap_decode.h"

#define SAT2YAB1(alpha,temp) (alpha << 24 | (temp & 0x1F) << 3 | (temp & 0x3E0) << 6 | (temp & 0x7C00) << 9)
#define SAT2YAB2(alpha,dot1,dot2) (alpha << 24 | ((dot1 & 0xFF) << 16) | (dot2 & 0xFF00) | (dot2 & 0xFF))
static uint8_t vram[0x80000];
static uint16_t T1ReadWord(uint8_t *mem, uint32_t addr) { return (uint16_t)(mem[addr] << 8 | mem[addr + 1]); }
static uint32_t ref16(int alpha, int transparencyenable, uint32_t addr) {
  uint32_t color;
  uint16_t dot = T1ReadWord(vram, addr & 0x7FFFF);
  if (!(dot & 0x8000) && transparencyenable) color = 0x00000000;
  else color = SAT2YAB1(alpha, dot);
  return color;
}
static uint32_t ref32(int alpha, int transparencyenable, uint32_t addr) {
  uint32_t color;
  uint16_t dot1, dot2;
  dot1 = T1ReadWord(vram, addr & 0x7FFFF);
  dot2 = T1ReadWord(vram, addr + 2 & 0x7FFFF);
  if (!(dot1 & 0x8000) && transparencyenable) color = 0x00000000;
  else color = SAT2YAB2(alpha, dot1, dot2);
  return color;
}

static uint32_t seed = 7;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

int main(void) {
  for (unsigned i = 0; i < sizeof(vram); ++i) vram[i] = (uint8_t)rnd();
  static uint32_t got[80], want[80];
  unsigned cases = 0;
  for (int t = 0; t < 200000; ++t) {
    const unsigned n = rnd() % 71;
    const int alpha = (t & 7) == 0 ? 0xFF : (t & 7) == 1 ? 0 : (int)(rnd() & 0xFF);
    const int transparent = rnd() & 1;
    const int wide = rnd() & 1;
    const uint32_t addr = (rnd() % (0x80000 - 4 * 72)) & ~1u;
    memset(got, 0xA5, sizeof(got));
    uint32_t *end = wide ? Vdp2DecodeRow32(vram + addr, n, got, (uint32_t)alpha << 24, transparent)
                         : Vdp2DecodeRow16(vram + addr, n, got, (uint32_t)alpha << 24, transparent);
    if (end != got + n || got[n] != 0xA5A5A5A5u) { printf("FAIL bounds t=%d\n", t); return 1; }
    for (unsigned i = 0; i < n; ++i)
      want[i] = wide ? ref32(alpha, transparent, addr + 4 * i) : ref16(alpha, transparent, addr + 2 * i);
    if (memcmp(got, want, n * 4)) {
      for (unsigned i = 0; i < n; ++i) if (got[i] != want[i]) {
        printf("FAIL t=%d %s n=%u i=%u got %08x want %08x\n", t, wide ? "32" : "16", n, i, got[i], want[i]);
        return 1;
      }
    }
    ++cases;
  }
  printf("VDP2 direct-color bitmap rows: %u cases identical (%s)\n", cases,
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
         "NEON"
#else
         "scalar"
#endif
  );
  return 0;
}
