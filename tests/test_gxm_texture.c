/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/gxm/texture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void word(uint8_t *ram, unsigned a, unsigned v) {
   ram[a & 0x7ffff] = v >> 8; ram[(a + 1) & 0x7ffff] = v;
}

int main(void) {
   uint8_t *ram = calloc(1, 0x80000);
   assert(ram);
   uint32_t result[16], palette[2048];
   for (unsigned i = 0; i < 2048; ++i) palette[i] = 0x80000000u | i;
   VitaVdp1Texture t = {.width=8, .height=1, .color=0x1200};
   // 4-bit: nonadjacent end codes, transparent zero, ignore the row tail.
   ram[0]=0x1f; ram[1]=0x20; ram[2]=0xf3; ram[3]=0x45;
   assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
   assert(result[0]==0xff001201 && result[1]==0 && result[2]==0xff001202);
   for (unsigned i=3; i<8; ++i) assert(result[i]==0);
   t.reverse_x=1;
   assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
   assert(result[0]==0 && result[1]==0 && result[2]==0xff001202);
   assert(result[4]==0 && result[5]==0xff001203 && result[7]==0xff001205);
   t.reverse_x=0; t.mode=0xc0;
   assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
   assert(result[1]==0xff00120f && result[3]==0xff001200);
   // Lookup values remain raw framebuffer codes, including a valid zero.
   t.mode=0xc8; t.color=0x10;
   word(ram, 0x80 + 2, 0x8123); word(ram, 0x80, 0);
   assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
   assert(result[0]==0xff008123 && result[3]==0xff000000);
   // 64/128-color end codes are FF, not the masked palette value.
   for (unsigned format=2; format<5; ++format) {
      t.mode=format<<3; t.color=0x1000;
      memset(ram, 1, 8); ram[1]=0xff; ram[3]=0xff;
      assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
      assert(result[0]==0xff001001 && result[1]==0 && result[2]==0xff001001);
      for (unsigned i=3; i<8; ++i) assert(result[i]==0);
      ram[1]=format==2 ? 0x3f : 0x7f;
      assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
      assert(result[1] != 0 && result[4] != 0);
   }
   // Valid RGB codes have MSB set (table 6.2). Retain the reference renderer's
   // transparent handling of prohibited MSB-clear codes when SPD is off.
   t.mode=5<<3; t.source=0x7ffff;
   for (unsigned i=0; i<8; ++i) word(ram,t.source+2*i,i==2?0x7fff:i<2?i:0x8000|i);
   assert(!VitaDecodeVdp1Texture(ram, &t, result, 8));
   assert(result[0]==0 && result[1]==0 && result[2]==0 && result[3]==0xff008003);
   VitaVdp2Cell cell = {.format=2, .source=0x7ffe0, .transparency=1};
   VitaVdp2Texel texels[64];
   // Exercise all five unused high bits independently of the 11-bit index.
   for (unsigned high=0; high<32; ++high) {
      for (unsigned i=0; i<64; ++i) word(ram,cell.source+2*i,(high<<11)|i);
      assert(!VitaDecodeVdp2Cell(ram,palette,&cell,texels));
      assert(!texels[0].visible);
      for (unsigned i=1; i<64; ++i) {
         assert(texels[i].visible && texels[i].rgba==(0xff000000u|i));
         assert(texels[i].dot==((high<<11)|i) && texels[i].color_msb==1);
      }
   }
   cell.format=3; word(ram,cell.source,0x801f);
   word(ram,cell.source+2,0x7c00);
   assert(!VitaDecodeVdp2Cell(ram,NULL,&cell,texels));
   assert(texels[0].rgba==0xff0000f8 && texels[0].visible && !texels[1].visible);
   cell.format=4; word(ram,cell.source,0x8012); word(ram,cell.source+2,0x3456);
   assert(!VitaDecodeVdp2Cell(ram,NULL,&cell,texels));
   assert(texels[0].rgba==0xff123456 && texels[0].color_msb);
   cell.format=5; assert(VitaDecodeVdp2Cell(ram,palette,&cell,texels)<0);
   t.width=505; assert(VitaDecodeVdp1Texture(ram,&t,result,8)<0);
   free(ram);
   puts("VDP1/VDP2 native texture decoding: passed");
}
