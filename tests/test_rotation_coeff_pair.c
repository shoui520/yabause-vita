/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/video/opengl/rotation_coeff_pair.h"
int main(void) {
  float rows[512][4]={{0}};
  assert(YglRotationPairSafe(rows,512));
  const uint32_t rejected[]={0x80000000,1,0x007fffff,0x80000001,
    0x7f800000,0xff800000,0x7fc00000,0x7f800001};
  const uint32_t accepted[]={0,0x00800000,0x80800000,0x7f7fffff,
    0xff7fffff,0x3f800000,0xbf800000};
  for(unsigned y=0;y<512;++y) for(unsigned x=0;x<2;++x) {
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
      memcpy(&rows[y][x],&rejected[i],4);
      assert(!YglRotationPairSafe(rows,512));
    }
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i) {
      memcpy(&rows[y][x],&accepted[i],4);
      assert(YglRotationPairSafe(rows,512));
    }
    rows[y][x]=0;
    /* Shader pair = row*2. Linear F32F32 texture row pitch 256*8
     * equals original RGBA8 upload row pitch 512*4. */
    unsigned pair=y*2;
    assert((pair>>8)*2048+(pair&255)*8+x*4 == y*16+x*4);
    /* Single-row descriptor uses the identical 8192-byte allocation. */
    assert(pair*8+x*4 == y*16+x*4);
    float u=((float)pair+0.5f)/1024.0f;
    assert((unsigned)(u*1024.0f)==pair);
    assert(pair*8+x*4+4 <= sizeof(rows));
    /* Affine vertex coordinates at raster centers, with generous interpolation
     * error around each center, must stay in the same nearest texel. */
    for(int e=-16;e<=16;++e) {
      float pixel_y=(float)y+0.5f+(float)e/4096.0f;
      float varying_u=(2.0f*pixel_y-0.5f)/1024.0f;
      assert((unsigned)(varying_u*1024.0f)==pair);
    }
  }
  puts("rotation coefficient pair admission and layout passed");
}
