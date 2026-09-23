/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "poll_loop.h"
#include <vector>

namespace sh2a9 {
// A single ordinary instruction sequence, not a wait-skipping policy. Decode
// is independent of PC/title and accepts only the exact non-delayed recipes.
// SH-1/SH-2 Programming Manual: MOV, EXTU, AND, CMP/EQ, TST and BF.
struct PollStep {
  static uint32_t Decode(const uint16_t *ops, size_t count) {
    const uint32_t poll = PollLoop::Decode(ops, count);
    if (!poll) return 0;
    return poll | (((ops[0] >> 8) & 15u) << 16) |
      (count == 5 ? (1u << 24) | (((ops[2] >> 4) & 15u) << 20) : 0);
  }

  // A32 whole-block body: existing prologue supplies r7=state, r8=PC,
  // r9=cycles and aligned stack; existing epilogue publishes PC/cycles.
  // ARM DDI0406B: LDR, BLX(register), SXTB/SXTH/UXTB, AND, CMP, BIC,
  // ORR and conditional ADD. All operations after CMP preserve its flags.
  static std::vector<uint32_t> Emit(const uint16_t *ops, size_t count) {
    const uint32_t descriptor = Decode(ops, count);
    if (!descriptor) return {};
    const unsigned n = (descriptor >> 16) & 15, m = descriptor & 15;
    const unsigned width = 1u << ((descriptor >> 4) & 3);
    const bool masked = count == 5;
    std::vector<uint32_t> code{
      0xe5970000u | (m * 4),
      0xe597a000u | (width == 1 ? 100u : width == 2 ? 104u : 108u),
      0xe12fff3au}; // BLX r10: retain the normal memory callback
    if (masked) code.push_back(0xe6ef0070u); // UXTB r0,r0
    else if (width == 1) code.push_back(0xe6af0070u); // SXTB
    else if (width == 2) code.push_back(0xe6bf0070u); // SXTH
    if (masked) {
      code.push_back(0xe5971000u | (((descriptor >> 20) & 15) * 4));
      code.push_back(0xe0000001u); // AND r0,r0,r1
      code.push_back(0xe1500001u); // CMP r0,r1
    } else code.push_back(0xe3500000u); // CMP r0,#0
    code.push_back(0xe5870000u | (n * 4));
    code.push_back(0xe597a040u);
    code.push_back(0xe3caa001u);
    code.push_back(0x038aa001u); // ORREQ: T=1 only on equality
    code.push_back(0xe587a040u);
    code.push_back(0x02888000u | unsigned(count * 2)); // ADDEQ: fallthrough
    code.push_back(0xe2899000u | unsigned(count));
    code.push_back(0x12899002u); // ADDNE: BF taken costs two extra cycles
    return code;
  }

  template<class Read>
  static void Run(uint32_t descriptor, uint32_t *r, uint32_t &sr,
                  uint32_t &pc, uint32_t &cycles, Read read) {
    const unsigned n = (descriptor >> 16) & 15, m = descriptor & 15;
    const unsigned width = 1u << ((descriptor >> 4) & 3);
    const bool masked = (descriptor & (1u << 24)) != 0;
    // Match native r8/r9 lifetime across the original first-instruction callback.
    const uint32_t entry_pc = pc, entry_cycles = cycles;
    uint32_t value = read(r[m], width);
    if (width == 1) value = (value & 127u) - (value & 128u);
    else if (width == 2) value = (value & 32767u) - (value & 32768u);
    r[n] = value;
    bool t;
    if (masked) {
      r[n] &= 255u;
      // Read mask AFTER the callback and load assignment. Source/mask aliases
      // are legal; Decode rejects only destination aliases that change recipe.
      const uint32_t mask = r[(descriptor >> 20) & 15];
      r[n] &= mask;
      t = r[n] == mask;
    } else t = r[n] == 0;
    sr = (sr & ~1u) | uint32_t(t);
    const unsigned words = masked ? 5 : 3;
    pc = t ? entry_pc + words * 2 : entry_pc;
    cycles = entry_cycles + words + (t ? 0 : 2);
    // The caller retains memory-cycle accounting and FinishBlock, including
    // the original interrupt and BLOCK_LOOP handling. No deadline rounding.
  }
};
}
