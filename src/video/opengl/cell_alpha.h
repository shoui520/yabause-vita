/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_CELL_ALPHA_H
#define YGL_CELL_ALPHA_H
#include <stdint.h>
/* 0: all zero alpha; 1: every alpha nonzero; 2: mixed. No color assumptions. */
static inline unsigned YglCellAlphaClass(const uint32_t *pixels,unsigned pitch,
                                         unsigned width,unsigned height) {
  unsigned zero=0,nonzero=0;
  for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
    if(pixels[y*pitch+x]&0xff000000u) nonzero=1; else zero=1;
    if(zero && nonzero) return 2;
  }
  return nonzero;
}
#endif
