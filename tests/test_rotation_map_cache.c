/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/video/opengl/rotation_map_cache.h"
int main(void) {
  VitaRotationMapPolicy p = {0};
  VitaRotationMapKey k = {0};
  k.bitmap[1] = k.bitmap[2] = 2048;
  k.map[0] = k.map[1] = 9;
  assert(YglRotationMapAdmit(&k));
  assert(YglRotationMapExtent(&k,0)==512 && YglRotationMapExtent(&k,1)==512);
  for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x) k.planes[y*4+x]=(float)(y*2+x%2);
  assert(YglRotationMapExtent(&k,0)==1024 && YglRotationMapExtent(&k,1)==2048);
  for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x) k.planes[y*4+x]=(float)((y%2)*4+x);
  assert(YglRotationMapExtent(&k,0)==2048 && YglRotationMapExtent(&k,1)==1024);
  k.planes[15]=99;
  assert(YglRotationMapExtent(&k,0)==2048 && YglRotationMapExtent(&k,1)==2048);
  k.bitmap[1]=512; assert(YglRotationMapExtent(&k,0)==512); k.bitmap[1]=2048;
  memset(k.planes,0,sizeof(k.planes));
  for (unsigned i=0;i<7;++i) assert(YglRotationMapObserve(&p,&k)==0);
  assert(YglRotationMapObserve(&p,&k)==2);
  assert(!p.valid); /* failed allocation must never create a hit */
  YglRotationMapCommit(&p,&k);
  assert(YglRotationMapObserve(&p,&k)==1);
  VitaRotationMapKey alternating = k;
  ++alternating.generation;
  for (unsigned i=0;i<20;++i) {
    assert(YglRotationMapObserve(&p,&alternating)==0);
    assert(YglRotationMapObserve(&p,&k)==1);
  }
  /* Every byte of every source/decoding field participates in identity. */
  for (unsigned i=0;i<sizeof(k);++i) {
    VitaRotationMapKey changed = k;
    ((unsigned char *)&changed)[i] ^= 1;
    assert(YglRotationMapObserve(&p,&changed)!=1);
    assert(YglRotationMapObserve(&p,&k)==1);
  }
  k.bitmap[1] = 4096; assert(!YglRotationMapAdmit(&k));
  k.bitmap[1] = 2047; assert(!YglRotationMapAdmit(&k));
  k.bitmap[1] = 2048; k.bitmap[3] = 1; assert(!YglRotationMapAdmit(&k));
  k.bitmap[3] = 3; assert(!YglRotationMapAdmit(&k));
  k.bitmap[3] = 2; assert(YglRotationMapAdmit(&k));
  k.pattern[3] = 1; assert(!YglRotationMapAdmit(&k));
  puts("rotation_map_cache_test_pass");
}
