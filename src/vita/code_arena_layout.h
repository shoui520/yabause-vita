/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#ifndef VITA_M68K_CODE_RESERVE_KIB
#define VITA_M68K_CODE_RESERVE_KIB 0
#endif
#ifndef VITA_SH2_BLOCK_STAGGER
#define VITA_SH2_BLOCK_STAGGER 0
#endif
namespace vitacode {
enum class Region { Sh2, M68k };
struct Layout {
  static constexpr std::size_t Total = 16 * 1024 * 1024;
  static constexpr std::size_t Block = 4096;
  static_assert(VITA_M68K_CODE_RESERVE_KIB >= 0 && VITA_M68K_CODE_RESERVE_KIB <= 15360,
                "68K reserve must leave at least 1 MiB for SH-2");
  static_assert(VITA_M68K_CODE_RESERVE_KIB % 4 == 0, "reserve must be 4 KiB aligned");
  static constexpr std::size_t M68k = VITA_M68K_CODE_RESERVE_KIB * 1024u;
  static constexpr std::size_t Sh2 = Total - M68k;
  /* Slot stride. Block-sized strides start every block on a 4 KiB boundary:
   * on the A9's 32 KiB 4-way L1 (8 KiB per way) all block entries then share
   * two cache sets. A few extra lines per slot spread them over all sets. */
  static_assert(VITA_SH2_BLOCK_STAGGER % 32 == 0, "stagger must be whole cache lines");
  static constexpr std::size_t Stride = Block + VITA_SH2_BLOCK_STAGGER;
  /* Shared runtime code (the sh2_dispatch copy and the block-link check) at
   * the end of the SH-2 region, within branch range of every block. */
  static constexpr std::size_t Stubs = 512;
  static constexpr std::size_t StubOffset = Sh2 - Stubs;
  static constexpr unsigned Sh2Blocks = StubOffset / Stride;
  static constexpr unsigned NextSh2Block(unsigned current) {
    if constexpr ((Sh2Blocks & (Sh2Blocks - 1)) == 0)
      return (current + 1) & (Sh2Blocks - 1);
    else
      return current + 1 == Sh2Blocks ? 0 : current + 1;
  }
  static constexpr bool Contains(Region region, std::size_t offset, std::size_t size) {
    const std::size_t start = region == Region::Sh2 ? 0 : Sh2;
    const std::size_t length = region == Region::Sh2 ? Sh2 : M68k;
    return size && offset >= start && offset - start < length &&
           size <= length - (offset - start);
  }
};
} // namespace vitacode
