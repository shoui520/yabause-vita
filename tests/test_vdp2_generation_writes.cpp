/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vdp2.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

int main() {
  Vdp2Ram=static_cast<u8 *>(calloc(1,0x80000));
  assert(Vdp2Ram);
  Vdp2RamGeneration=0; Vdp2RamGenerationEpoch=0;
  for(unsigned bank=0;bank<4;++bank) {
    unsigned a=bank*0x20000+0x104;
    unsigned before=Vdp2RamGeneration;
    Vdp2RamWriteByte(a,0x83); assert(Vdp2RamReadByte(a)==0x83);
    assert(Vdp2RamGeneration==before+1);
    Vdp2RamWriteByte(a+0x80000,0x83); assert(Vdp2RamGeneration==before+1);
    Vdp2RamWriteWord(a,0x9a37); assert(Vdp2RamReadWord(a)==0x9a37);
    assert(Vdp2RamGeneration==before+2);
    Vdp2RamWriteWord(a+0x80000,0x9a37); assert(Vdp2RamGeneration==before+2);
    Vdp2RamWriteLong(a,0x12345678); assert(Vdp2RamReadLong(a)==0x12345678);
    assert(Vdp2RamGeneration==before+3);
    Vdp2RamWriteLong(a+0x80000,0x12345678); assert(Vdp2RamGeneration==before+3);
    Vdp2RamWriteLong(a,0); assert(Vdp2RamGeneration==before+4);
    Vdp2RamWriteLong(a,0x12345678); assert(Vdp2RamGeneration==before+5);
  }
  Vdp2RamGeneration=UINT32_MAX; Vdp2RamGenerationEpoch=17;
  Vdp2RamWriteByte(0x7ffff,0xab);
  assert(Vdp2RamGeneration==0 && Vdp2RamGenerationEpoch==18);
  Vdp2RamWriteByte(0xfffff,0xab);
  assert(Vdp2RamGeneration==0 && Vdp2RamGenerationEpoch==18);
  Vdp2RamGeneration=UINT32_MAX; Vdp2RamGenerationEpoch=UINT32_MAX-1;
  Vdp2RamWriteWord(0x7fffc,0xabcd);
  assert(Vdp2RamGeneration==0 && Vdp2RamGenerationEpoch==UINT32_MAX);
  Vdp2RamGeneration=UINT32_MAX;
  Vdp2RamWriteLong(0x7fffc,0xdeadbeef);
  assert(Vdp2RamGeneration==0 && Vdp2RamGenerationEpoch==UINT32_MAX);
  free(Vdp2Ram); Vdp2Ram=nullptr;
  puts("production VDP2 writes: widths, banks, mirrors, identical stores and epoch exhaustion passed");
}
