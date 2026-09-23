/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_FRAME_CAPTURE_H
#define VITA_FRAME_CAPTURE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* OpenGL default-framebuffer RGBA readback is bottom row first. PPM is top first. */
static int VitaWriteRgbRows(FILE *file, const uint8_t *rgba, unsigned w, unsigned h,
                           unsigned stride, int bottom_up) {
  if (!file || !rgba || !w || !h || w > 4096 || h > 4096 ||
      stride < w || stride > 4096) return -1;
  uint8_t *row = malloc((size_t)w * 3);
  if (!row) return -1;
  int result = fprintf(file, "P6\n%u %u\n255\n", w, h) < 0 ? -1 : 0;
  for (unsigned y = 0; result == 0 && y < h; ++y) {
    const uint8_t *source = rgba + (size_t)(bottom_up ? h-1-y : y)*stride*4;
    for (unsigned x = 0; x < w; ++x)
      for (unsigned c = 0; c < 3; ++c) row[x*3+c] = source[x*4+c];
    if (fwrite(row, 3, w, file) != w) result = -1;
  }
  free(row);
  return result;
}
static int VitaWriteRgbFrame(FILE *file, const uint8_t *rgba, unsigned w, unsigned h) {
  return VitaWriteRgbRows(file, rgba, w, h, w, 1);
}
#endif
