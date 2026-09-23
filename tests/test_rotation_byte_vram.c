/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
  /* Exhaust every wrapped VRAM address, including all row/texel boundaries.
   * Old RGBA8 row pitch 512*4 equals U8 row pitch 2048. */
  for (uint32_t address=0;address<0x100000;++address) {
    uint32_t wrapped=address&0x7ffff;
    uint32_t texel=wrapped>>2, lane=address&3;
    uint32_t old_offset=(texel>>9)*2048+(texel&511)*4+lane;
    uint32_t new_offset=(wrapped>>11)*2048+(wrapped&2047);
    assert(old_offset==new_offset && new_offset<0x80000);
  }
  for(unsigned byte=0;byte<256;++byte) for(unsigned odd=0;odd<2;++odd) {
    int promoted=byte<128 ? (int)byte : (int)byte-256;
    assert((promoted&255)==(int)byte);
    unsigned old_code=(byte>>(odd ? 0 : 4))&15;
    unsigned direct_code=odd ? byte%16 : byte/16;
    assert(old_code==direct_code);
  }
  puts("byte VRAM: exhaustive wrapped addressing and 4-bit ordering passed");
}
