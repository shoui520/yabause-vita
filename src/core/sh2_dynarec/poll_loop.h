/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <cstddef>
namespace sh2a9 {
struct PollLoop {
  // Descriptor: valid bit, load width log2, unchanged base register.
  static uint32_t Decode(const uint16_t *ops, size_t count) {
    if (count != 3 && count != 5) return 0;
    const unsigned kind = ops[0] & 0xf00f;
    if (kind < 0x6000 || kind > 0x6002) return 0;
    const unsigned n = (ops[0] >> 8) & 15, m = (ops[0] >> 4) & 15;
    if (n == m) return 0; // The next iteration must use the same address.
    if (count == 3) {
      if (ops[1] != (0x2008 | (n << 8) | (n << 4)) || ops[2] != 0x8bfc) return 0;
    } else {
      const unsigned mask = (ops[2] >> 4) & 15;
      if (kind != 0x6000 || mask == n ||
          ops[1] != (0x600c | (n << 8) | (n << 4)) ||
          ops[2] != (0x2009 | (n << 8) | (mask << 4)) ||
          ops[3] != (0x3000 | (n << 8) | (mask << 4)) || ops[4] != 0x8bfa) return 0;
    }
    return 0x100 | ((kind & 3) << 4) | m;
  }
  static bool StableAddress(uint32_t descriptor, uint32_t address) {
    if (!(descriptor & 0x100) || (descriptor & ~0x13fu) ||
        ((descriptor >> 4) & 3) == 3) return false;
    const unsigned width = 1u << ((descriptor >> 4) & 3);
    if (width == 1 && address == 0xfffffe11u) return true; // FTCSR, not FRC/ICR.
    const unsigned region = address >> 20;
    return (region == 2 || region == 0x202 || region == 0x60 || region == 0x260) &&
           !(address & (width - 1));
  }
  // Preserve the reference loop's whole-iteration overrun, including repeated
  // uncached-RAM wait costs. Never round directly to the scheduler deadline.
  struct Skip { uint32_t cycles, iterations; };
  static Skip ComputeSkip(uint32_t count, uint32_t memory_cycles,
                          uint32_t target, uint32_t iteration_cycles) {
    const uint64_t completed = uint64_t(count) + memory_cycles;
    const uint64_t step = uint64_t(iteration_cycles) + memory_cycles;
    if (!step || step > 0xffffffffu || completed >= target) return {};
    const uint32_t remaining_minus_one = target - uint32_t(completed) - 1;
    // Cortex-A9 has no integer divide instruction. Constant division on the
    // two common five/seven-cycle recipes becomes multiply/shift; retain a
    // 32-bit fallback for callback wait states, never a 64-bit divide helper.
    const uint32_t iterations = 1 + (step == 5 ? remaining_minus_one / 5 :
      step == 7 ? remaining_minus_one / 7 : remaining_minus_one / uint32_t(step));
    const uint64_t skip = uint64_t(iterations) * uint32_t(step);
    if (completed + skip > 0xffffffffu) return {};
    return {uint32_t(skip), iterations};
  }
  static uint32_t SkipCycles(uint32_t count, uint32_t memory_cycles,
                             uint32_t target, uint32_t iteration_cycles) {
    return ComputeSkip(count, memory_cycles, target, iteration_cycles).cycles;
  }
};
}
