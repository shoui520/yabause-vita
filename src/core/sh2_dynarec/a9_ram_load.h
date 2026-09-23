/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <cassert>
#include <vector>

namespace sh2a9 {
// Canonical-state lowering of MOV.B/W/L @Rm,Rn and @Rm+,Rn. Runtime guards deliberately
// exclude cache-through aliases, MMIO and unaligned accesses. The generic
// callback preserves their existing semantics/cycle accounting. Work RAM uses
// Yabause T2 halfword layout: byte xor 1, native halfword, longword ROR 16.
// Clobbers r0-r3/r10 and normal callback scratch registers, like old templates.
// Does not update PC/cycles; the caller emits the normal instruction separator.
inline std::vector<uint32_t> RamLoad(unsigned n, unsigned m, unsigned width,
                                    uint32_t low, uint32_t high, bool postincrement = false) {
  assert(n < 16 && m < 16 && (width == 1 || width == 2 || width == 4));
  std::vector<uint32_t> c;
  auto constant = [&](unsigned r, uint32_t value) {
    c.push_back(0xe3000000u | (r << 12) | ((value & 0xf000) << 4) | (value & 0xfff));
    c.push_back(0xe3400000u | (r << 12) | ((value >> 12) & 0xf0000) | ((value >> 16) & 0xfff));
  };
  auto branch = [&](unsigned at, unsigned target, uint32_t condition) {
    c[at] = condition | 0x0a000000u | ((int32_t(target - at - 2)) & 0x00ffffffu);
  };
  c.push_back(0xe5970000u | (m * 4)); // ldr r0,[r7,#Rm]
  c.push_back(0xe1a01a20u); // mov r1,r0,lsr #20
  c.push_back(0xe3510060u); // high cached RAM?
  const unsigned high_branch = c.size(); c.push_back(0);
  c.push_back(0xe3510002u); // low cached RAM?
  const unsigned slow_branch = c.size(); c.push_back(0);
  constant(2, low);
  const unsigned base_branch = c.size(); c.push_back(0);
  branch(high_branch, c.size(), 0x00000000u); // EQ
  constant(2, high);
  branch(base_branch, c.size(), 0xe0000000u); // AL
  c.push_back(0xe3c034ffu); // bic r3,r0,#0xff000000
  c.push_back(0xe3c3360fu); // bic r3,r3,#0x00f00000
  unsigned alignment_branch = 0;
  if (width != 1) {
    c.push_back(0xe3130000u | (width - 1));
    alignment_branch = c.size(); c.push_back(0);
  }
  if (width == 1) {
    c.push_back(0xe2233001u); // eor r3,r3,#1
    c.push_back(0xe19200d3u); // ldrsb r0,[r2,r3]
  } else if (width == 2) c.push_back(0xe19200f3u); // ldrsh
  else {
    c.push_back(0xe7920003u); // ldr
    c.push_back(0xe1a00860u); // ror r0,r0,#16
  }
  const unsigned done_branch = c.size(); c.push_back(0);
  branch(slow_branch, c.size(), 0x10000000u); // NE
  if (alignment_branch) branch(alignment_branch, c.size(), 0x10000000u);
  c.push_back(0xe597a000u | (width == 1 ? 100 : width == 2 ? 104 : 108));
  c.push_back(0xe12fff3au); // blx r10
  if (width == 1) c.push_back(0xe6af0070u); // sxtb
  if (width == 2) c.push_back(0xe6bf0070u); // sxth
  branch(done_branch, c.size(), 0xe0000000u);
  c.push_back(0xe5870000u | (n * 4));
  if (postincrement && n != m) {
    // Reload after callbacks, matching the legacy MOVBP/WP/LP templates.
    c.push_back(0xe5971000u | (m * 4));
    c.push_back(0xe2811000u | width);
    c.push_back(0xe5871000u | (m * 4));
  }
  return c;
}
}
