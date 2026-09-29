/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/video/opengl/vdp2_cell_decode.h"

static uint8_t vram[0x80000];

int main(void) {
  srand(1);
  unsigned checked = 0, clear = 0;
  for (int iter = 0; iter < 200000; ++iter) {
    const int color = rand() & 1, patternwh = 1 + (rand() & 1), transparent = rand() & 1;
    const unsigned cell_bytes = color ? 64 : 32, cells = (unsigned)(patternwh * patternwh);
    const uint32_t addr = (uint32_t)((rand() % (0x80000 / 32)) * 32);
    if (addr + cells * cell_bytes > 0x80000) continue;
    /* Mostly-zero sources so all-transparent patterns are common. */
    for (unsigned b = 0; b < cells * cell_bytes; ++b)
      vram[addr + b] = (rand() % 97 == 0) ? (uint8_t)(rand() & 0xFF) : 0;
    uint16_t banks[4];
    for (int k = 0; k < 4; ++k) banks[k] = (rand() % 5) != 0;
    const uint32_t alpha_bits = (uint32_t)(rand() & 0xFF) << 24;
    const uint32_t co = (uint32_t)(rand() & 0x7FF), pal = (uint32_t)(rand() & 0x7F) << 4;
    const int got = Vdp2CellsTransparent(vram, addr, cells, cell_bytes, banks, alpha_bits, transparent);
    if (!(alpha_bits & 0xFF000000u)) { assert(got == -1); continue; }
    /* Reference: decode each cell as Vdp2DrawCell's fast path does. */
    uint32_t out[64], any = 0;
    for (unsigned c = 0; c < cells; ++c) {
      const uint32_t a = addr + c * cell_bytes;
      if (banks[a >> 17] == 0) continue;   /* zero-filled */
      if (color) Vdp2DecodeWords8(vram, a, 32, out, co, pal, alpha_bits, transparent);
      else       Vdp2DecodeWords4(vram, a, 16, out, co, pal, alpha_bits, transparent);
      for (int t = 0; t < 64; ++t) any |= out[t] & 0xFF000000u;
    }
    assert(got == (any == 0));
    ++checked; clear += got;
  }
  printf("vdp2_cells_transparent: ok checked=%u all_clear=%u\n", checked, clear);
  return 0;
}
