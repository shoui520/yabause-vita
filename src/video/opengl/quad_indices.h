/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_QUAD_INDICES_H
#define YGL_QUAD_INDICES_H
#include <stdint.h>
enum { YGL_QUAD_INDEX_COUNT = 65532 };
static inline void YglFillQuadIndices(uint16_t *indices, unsigned count) {
  for(unsigned i=0;i<count;i+=6) {
    indices[i]=i; indices[i+1]=i+1; indices[i+2]=i+2;
    indices[i+3]=i; indices[i+4]=i+2; indices[i+5]=i+5;
  }
}
#endif
