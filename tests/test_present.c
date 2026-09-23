/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "present.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

int main(void) {
  uint32_t *source = malloc(704 * 512 * sizeof(*source));
  uint32_t *output = malloc(512 * 272 * sizeof(*output));
  assert(source && output);
  for (unsigned i = 0; i < 704 * 512; ++i) source[i] = i * 2654435761u;
  const unsigned widths[] = {1, 320, 352, 640, 704};
  const unsigned heights[] = {1, 224, 240, 256, 448, 480, 512};
  VitaPresentMap map = {0};
  assert(VitaPresentMapInit(&map, 0, 224) == -1);
  assert(VitaPresentMapInit(&map, 705, 224) == -1);
  assert(VitaPresentMapInit(&map, 320, 513) == -1);
  for (unsigned wi = 0; wi < sizeof(widths)/sizeof(*widths); ++wi)
    for (unsigned hi = 0; hi < sizeof(heights)/sizeof(*heights); ++hi) {
      unsigned w = widths[wi], h = heights[hi];
      for (unsigned i = 0; i < 512 * 272; ++i) output[i] = 0x12345678;
      assert(VitaPresentMapInit(&map, w, h) == 0);
      assert(VitaPresentMapInit(&map, w, h) == 0); /* Cached mapping. */
      VitaPresentCopy(&map, output, source);
      for (unsigned y = 0; y < 272; ++y)
        for (unsigned x = 0; x < 512; ++x) {
          uint32_t expected = x < 480 ?
            source[((uint64_t)y * h / 272) * w + (uint64_t)x * w / 480] | 0xff000000u :
            0x12345678;
          assert(output[y * 512 + x] == expected);
        }
    }
  free(source); free(output);
  puts("presentation mapping: 35 sizes match reference; padding untouched");
}
