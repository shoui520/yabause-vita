/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>

namespace sh2a9 {
// Read the authoritative table every time. No second cache or retained target
// survives a guest memory write, invalidation, recompilation or slot recycling.
template<class Block>
inline Block *CachedBlock(uint32_t pc, bool rom_helpers,
                          Block *const *rom, Block *const *low, Block *const *high) {
  if ((pc & 0xff000000u) == 0xc0000000u) return nullptr;
  const uint32_t index = (pc & 0xfffffu) >> 1;
  switch (pc & 0x0ff00000u) {
    case 0x00000000: return rom_helpers ? nullptr : rom[index];
    case 0x00200000: return low[index];
    case 0x06000000: return high[index];
    default: return nullptr;
  }
}
}
