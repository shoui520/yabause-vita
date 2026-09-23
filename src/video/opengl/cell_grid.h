/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_CELL_GRID_H
#define YGL_CELL_GRID_H
#include <math.h>
#include <stdint.h>
#include <string.h>
/* Prove disjoint axis-aligned rectangles on one integral grid. Exact float
 * equality is intentional: uncertain/scaled/irregular geometry rejects.
 * A shared edge is owned by one triangle under the rasterizer fill rule.
 * This proof is only useful with the same affine matrix, read-only stencil,
 * no blending/feedback, and normal shaders with no explicit depth output. */
static inline int YglDisjointCellGrid(const float *p,unsigned quads) {
  if(!quads || quads>16384) return 0;
  int width=0,height=0,minx=32768,miny=32768,maxx=-32768,maxy=-32768;
  for(unsigned q=0;q<quads;++q) {
    const float *v=p+q*12;
    for(unsigned i=0;i<12;++i)
      if(!isfinite(v[i]) || v[i]<-16384 || v[i]>16384 || v[i]!=(int)v[i]) return 0;
    if(v[1]!=v[3] || v[2]!=v[4] || v[4]!=v[8] || v[5]!=v[9] ||
       v[5]!=v[11] || v[0]!=v[6] || v[0]!=v[10] || v[1]!=v[7]) return 0;
    int w=v[2]-v[0],h=v[5]-v[1];
    if(w<=0 || h<=0 || w>64 || h>64) return 0;
    if(!q) { width=w; height=h; }
    if(w!=width || h!=height) return 0;
    int x=v[0],y=v[1];
    if(x<minx) minx=x;
    if(x>maxx) maxx=x;
    if(y<miny) miny=y;
    if(y>maxy) maxy=y;
  }
  if((maxx-minx)/width>=128 || (maxy-miny)/height>=128) return 0;
  uint8_t occupied[128*128/8]; memset(occupied,0,sizeof(occupied));
  for(unsigned q=0;q<quads;++q) {
    int x=(int)p[q*12]-minx,y=(int)p[q*12+1]-miny;
    if(x%width || y%height) return 0;
    unsigned index=y/height*128+x/width,bit=1u<<(index&7);
    if(occupied[index>>3]&bit) return 0;
    occupied[index>>3]|=bit;
  }
  return 1;
}
#endif
