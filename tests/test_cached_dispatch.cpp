/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/cached_dispatch.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  constexpr unsigned entries = 0x100000 / 2;
  std::vector<int *> rom(entries), low(entries), high(entries);
  int r=1, l=2, h=3, replacement=4;
  unsigned cases=0;
  for (unsigned index : {0u,1u,0x1234u,entries-1}) {
    rom[index]=&r; low[index]=&l; high[index]=&h;
    // Exhaust all upper twelve address bits against the existing dispatch
    // masks, including uncached aliases and the separately keyed C0 region.
    for (unsigned upper=0; upper<4096; ++upper)
      for (unsigned odd=0; odd<2; ++odd)
        for (bool helpers : {false,true}) {
          uint32_t pc=(upper<<20)|(index<<1)|odd;
          int *expected=nullptr;
          if ((pc>>24)!=0xc0) {
            unsigned mapping=upper&255;
            if (mapping==0 && !helpers) expected=&r;
            if (mapping==2) expected=&l;
            if (mapping==0x60) expected=&h;
          }
          assert(sh2a9::CachedBlock(pc, helpers, rom.data(), low.data(), high.data())==expected);
          ++cases;
        }
    // Invalidation/recycling must be visible on the very next lookup.
    low[index]=nullptr;
    assert(!sh2a9::CachedBlock(0x00200000|(index<<1),false,rom.data(),low.data(),high.data()));
    low[index]=&replacement;
    assert(sh2a9::CachedBlock(0x00200000|(index<<1),false,rom.data(),low.data(),high.data())==&replacement);
    rom[index]=low[index]=high[index]=nullptr;
  }
  std::printf("SH-2 cached dispatch: %u address/helper cases plus invalidation/replacement passed\n",cases);
}
