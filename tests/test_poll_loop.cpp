/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/poll_loop.h"
#include <cassert>
#include <cstdio>
using sh2a9::PollLoop;
int main() {
  unsigned patterns = 0, cycles = 0;
  for (unsigned n = 0; n < 16; ++n)
    for (unsigned m = 0; m < 16; ++m)
      for (unsigned width = 0; width < 3; ++width) {
        uint16_t ops[] = {uint16_t(0x6000 | n << 8 | m << 4 | width),
          uint16_t(0x2008 | n << 8 | n << 4), 0x8bfc};
        auto desc = PollLoop::Decode(ops, 3);
        assert(bool(desc) == (n != m));
        if (desc) {
          assert((desc & 15) == m);
          assert(PollLoop::StableAddress(desc, 0x06000000));
          assert(PollLoop::StableAddress(desc, 0x260ffffc));
          assert(PollLoop::StableAddress(desc, 0x00200000));
          assert(PollLoop::StableAddress(desc, 0x202ffffc));
          assert(PollLoop::StableAddress(desc, 0xfffffe11) == (width == 0));
          assert(!PollLoop::StableAddress(desc, 0xfffffe12));
          assert(!PollLoop::StableAddress(desc, 0x05a00000));
          assert(!PollLoop::StableAddress(desc, 0x46000000));
          assert(PollLoop::StableAddress(desc, 0x06000001) == (width == 0));
        }
        ops[2] = 0x8bfb; // Different back edge cannot use this recipe.
        assert(!PollLoop::Decode(ops, 3));
        ++patterns;
      }
  for (unsigned n = 0; n < 16; ++n)
    for (unsigned m = 0; m < 16; ++m)
      for (unsigned mask = 0; mask < 16; ++mask) {
        uint16_t ops[] = {uint16_t(0x6000 | n << 8 | m << 4),
          uint16_t(0x600c | n << 8 | n << 4),
          uint16_t(0x2009 | n << 8 | mask << 4),
          uint16_t(0x3000 | n << 8 | mask << 4), 0x8bfa};
        assert(bool(PollLoop::Decode(ops, 5)) == (n != m && n != mask));
        ops[1] ^= 1; // EXTU.W is not the byte recipe.
        assert(!PollLoop::Decode(ops, 5));
        ++patterns;
      }
  assert(!PollLoop::StableAddress(0x130, 0x06000000));
  assert(!PollLoop::StableAddress(0x300, 0x06000000));
  assert(!PollLoop::Decode(nullptr, 0));
  for (unsigned count = 0; count < 40; ++count)
    for (unsigned memory = 0; memory < 8; ++memory)
      for (unsigned iteration = 1; iteration < 16; ++iteration)
        for (unsigned target = 0; target < 150; ++target) {
          uint64_t reference = count + memory;
          while (reference < target) reference += iteration + memory;
          assert(count + memory + PollLoop::SkipCycles(count, memory, target, iteration) == reference);
          const auto skip = PollLoop::ComputeSkip(count, memory, target, iteration);
          assert(uint64_t(skip.iterations) * (iteration + memory) == skip.cycles);
          ++cycles;
        }
  assert(!PollLoop::SkipCycles(0, 0, 100, 0));
  assert(!PollLoop::SkipCycles(0xfffffffe, 0, 0xffffffff, 2));
  assert(PollLoop::SkipCycles(0xfffffffe, 0, 0xffffffff, 1) == 1);
  std::printf("Poll loops: %u decode cases, %u deadline cases passed\n", patterns, cycles);
}
