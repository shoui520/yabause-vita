/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/vidsoft.c"
#include "../src/video/opengl/rotation_pattern_cache.h"
#include <assert.h>
#include <stdio.h>
Vdp2Internal_struct Vdp2Internal;

static void check_name(YglRotationPatternKey *k, unsigned a, unsigned b) {
  uint8_t ram[4], descriptor[4];
  T1WriteWord(ram,0,a); T1WriteWord(ram,2,b);
  vdp2draw_struct info={0}; Vdp2 regs={0};
  /* Retain all 15 character bits; the final VRAM access wraps at 512 KiB. */
  regs.VRSIZE=0x8000;
  info.colornumber=k->format; info.patternwh=k->cells;
  info.patterndatasize=k->words; info.auxmode=k->aux;
  info.supplementdata=k->supplement;
  Vdp2PatternAddr(&info,&regs,ram);
  YglRotationPatternDecode(k,a,b,descriptor);
  assert(((unsigned)descriptor[0]|((unsigned)descriptor[1]<<8))*32 == info.charaddr);
  assert((unsigned)descriptor[2]*16 == info.paladdr);
  assert((descriptor[3]&3) == info.flipfunction);
  assert(((descriptor[3]>>2)&1) == info.specialfunction);
  assert(((descriptor[3]>>3)&1) == info.specialcolorfunction);
}

int main(void) {
  const unsigned formats[]={0,1,3,4}, supplements[]={0,3,0x15,0xe0,0x100,0x200,0x3ff};
  YglRotationPatternKey k={0};
  unsigned checks=0;
  for(unsigned f=0;f<4;++f) for(unsigned c=1;c<=2;++c)
  for(unsigned w=1;w<=2;++w) for(unsigned aux=0;aux<2;++aux) {
    k.format=formats[f]; k.cells=c; k.words=w; k.aux=aux;
    for(unsigned s=0;s<7;++s) {
      k.supplement=supplements[s];
      for(unsigned name=0;name<65536;++name) {
        check_name(&k,name,name^0xa55a); ++checks;
      }
    }
    for(unsigned s=0;s<1024;++s) {
      k.supplement=s;
      check_name(&k,0xffff,0xffff); check_name(&k,0,0); checks+=2;
    }
  }
  printf("pattern descriptor: %u software-reference comparisons passed\n",checks);
  uint8_t *ram=malloc(0x80000), *output=malloc(0x100000);
  assert(ram && output);
  for(unsigned i=0;i<0x80000;++i) ram[i]=(uint8_t)(i*37+(i>>7));
  for(unsigned sx=9;sx<=10;++sx) for(unsigned sy=9;sy<=10;++sy)
  for(unsigned cells=1;cells<=2;++cells) for(unsigned words=1;words<=2;++words) {
    k=(YglRotationPatternKey){0};
    k.width=4u<<sx; k.height=4u<<sy; k.shift_x=sx; k.shift_y=sy;
    k.pages_x=1u<<(sx-9); k.cells=cells; k.words=words; k.format=1;
    for(unsigned p=0;p<16;++p) k.planes[p]=(0x7fffe + p*0x2468)&0x7ffff;
    size_t bytes=YglRotationPatternBytes(&k);
    assert(bytes && bytes<=0x100000);
    assert(!YglRotationPatternBuild(&k,ram,output,bytes-1));
    assert(YglRotationPatternBuild(&k,ram,output,bytes));
    unsigned tw=k.width/(cells*8), th=k.height/(cells*8);
    for(unsigned y=0;y<th;++y) for(unsigned x=0;x<tw;++x) {
      unsigned h=x*cells*8,v=y*cells*8;
      unsigned plane=(v/(1u<<sy))*4+h/(1u<<sx);
      unsigned lx=h%(1u<<sx),ly=v%(1u<<sy);
      unsigned page=(ly/512)*k.pages_x+lx/512;
      unsigned row=(ly%512)/(cells*8), col=(lx%512)/(cells*8);
      unsigned side=64/cells;
      unsigned address=k.planes[plane]+(page*side*side+row*side+col)*words*2;
      unsigned a=((unsigned)ram[address%0x80000]<<8)|ram[(address+1)%0x80000];
      unsigned b=((unsigned)ram[(address+2)%0x80000]<<8)|ram[(address+3)%0x80000];
      uint8_t expected[4]; YglRotationPatternDecode(&k,a,b,expected);
      assert(!memcmp(expected,output+((size_t)y*tw+x)*4,4));
    }
  }
  k.shift_x=32; assert(!YglRotationPatternBytes(&k));
  k.shift_x=9; k.width=4096; assert(!YglRotationPatternBytes(&k));
  k.width=2048; k.pages_x=3; assert(!YglRotationPatternBytes(&k));
  free(output); free(ram);
  puts("pattern maps: all plane/page layouts, both name/cell sizes, wrapped VRAM, bounds passed");
}
