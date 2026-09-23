/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_OPAQUE_CELL_H
#define YGL_OPAQUE_CELL_H
#include <math.h>
#include "cell_alpha.h"
/* Normal shader coordinates are affine, unnormalized texel addresses. Only
 * small, inset cells qualify; wrapped/outside/edge coordinates fall back.
 * Inspect every texel in the bounding rectangle, not just its corners. */
static inline int YglOpaqueCell(const uint32_t *pixels,unsigned pitch,
                                unsigned height,const float *uv) {
  float lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
  for(unsigned v=0;v<6;++v) {
    if(uv[4*v+2]!=0 || uv[4*v+3]!=1) return 0;
    for(unsigned a=0;a<2;++a) {
      float x=uv[4*v+a];
      if(!isfinite(x) || x<0) return 0;
      if(x<lo[a]) lo[a]=x;
      if(x>hi[a]) hi[a]=x;
    }
  }
  if(hi[0]>=pitch || hi[1]>=height) return 0;
  unsigned x=(unsigned)lo[0],y=(unsigned)lo[1];
  unsigned right=(unsigned)hi[0],bottom=(unsigned)hi[1];
  /* The producer's ATLAS_BIAS is .025. Retain a .015 inset to reject
   * integer-edge sampling and leave rounding headroom, as the existing
   * transparent-cell elimination does for the same normal shaders. */
  if(lo[0]-x<.015f || lo[1]-y<.015f ||
     right+1-hi[0]<.015f || bottom+1-hi[1]<.015f ||
     right-x>=16 || bottom-y>=16) return 0;
  return YglCellAlphaClass(pixels+y*pitch+x,pitch,right-x+1,bottom-y+1)==1;
}
#endif
