/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/video/opengl/atlas_extent.h"
#include <assert.h>
#include <stdio.h>
static YglAtlasRect skip[2];
static int excluded(unsigned x,unsigned y,unsigned w,unsigned h) {
  for(unsigned i=0;i<2;++i)
    if(skip[i].x==x && skip[i].y==y && skip[i].w==w && skip[i].h==h) return 1;
  return 0;
}
int main(void) {
  unsigned cases=0;
  /* Every pair of exclusions, tie-heavy bottoms and all stream prefixes.
   * Unique x models non-overlapping allocation identities, irrespective of
   * physical packing; compare production top-three logic to an exhaustive scan. */
  for(unsigned mode=0;mode<4;++mode) for(unsigned n=1;n<=32;++n) {
    YglAtlasRect all[32]; YglAtlasExtent e={0};
    for(unsigned i=0;i<n;++i) {
      unsigned bottom=mode==0 ? i+1 : mode==1 ? 32-i : mode==2 ? 20 : (i*13)%17+1;
      all[i]=(YglAtlasRect){i,0,1,bottom};
      YglAtlasExtentAdd(&e,i,0,1,bottom);
    }
    for(unsigned a=0;a<=n;++a) for(unsigned b=0;b<=n;++b) {
      skip[0]=a<n ? all[a] : (YglAtlasRect){99,0,1,1};
      skip[1]=b<n ? all[b] : (YglAtlasRect){99,0,1,1};
      unsigned expected=0;
      for(unsigned i=0;i<n;++i)
        if(!excluded(all[i].x,all[i].y,all[i].w,all[i].h) && all[i].h>expected)
          expected=all[i].h;
      assert(YglAtlasExtentHeight(&e,excluded)==expected); ++cases;
    }
  }
  printf("atlas extent: %u exhaustive exclusion cases passed\n",cases);
}
