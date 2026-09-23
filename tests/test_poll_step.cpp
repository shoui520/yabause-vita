/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/poll_step.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <cstring>
#include <sys/mman.h>
extern "C" void sh2_poll_step_oracle(uint32_t *, const uint16_t *, unsigned,
                                    uint32_t, uint32_t, unsigned, uint32_t *);
static uint32_t *callback_state, callback_value, callback_address;
static unsigned callback_calls, callback_width, callback_mutate;
static uint32_t Read(uint32_t address, unsigned width) {
  callback_address = address; callback_width = width; ++callback_calls;
  if (callback_mutate) { callback_state[15] ^= 0x76543210u; callback_state[16] ^= 0x101u; }
  return callback_value;
}
static uint32_t Byte(uint32_t a) { return Read(a, 1) & 255; }
static uint32_t Word(uint32_t a) { return Read(a, 2) & 65535; }
static uint32_t Long(uint32_t a) { return Read(a, 4); }
int main() {
  auto *memory = static_cast<uint32_t *>(mmap(nullptr, 4096,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
  std::mt19937 random(0x504f4c4c);
  unsigned cases = 0;
  for (unsigned width = 0; width < 3; ++width)
  for (unsigned n = 0; n < 16; ++n)
  for (unsigned m = 0; m < 16; ++m)
  for (unsigned masked = 0; masked < 2; ++masked)
  for (unsigned mask = 0; mask < (masked ? 16u : 1u); ++mask) {
    const uint16_t plain[] = {uint16_t(0x6000 | n << 8 | m << 4 | width),
      uint16_t(0x2008 | n << 8 | n << 4), 0x8bfc};
    const uint16_t extended[] = {plain[0], uint16_t(0x600c | n << 8 | n << 4),
      uint16_t(0x2009 | n << 8 | mask << 4), uint16_t(0x3000 | n << 8 | mask << 4), 0x8bfa};
    const auto *ops = masked ? extended : plain;
    const unsigned words = masked ? 5 : 3;
    const uint32_t desc = sh2a9::PollStep::Decode(ops, words);
    if (n == m || (masked && (width != 0 || mask == n))) { assert(!desc); continue; }
    assert(desc);
    const auto body = sh2a9::PollStep::Emit(ops, words);
    assert(!body.empty());
    // Test state has PC/count at17/18 instead of tagSH2's22/23. Only the
    // wrapper loads/stores these; the fused body keeps them in r8/r9.
    std::vector<uint32_t> function{0xe92d47f0u, 0xe1a07000u, 0xe5978044u, 0xe5979048u};
    function.insert(function.end(), body.begin(), body.end());
    function.insert(function.end(), {0xe5878044u, 0xe5879048u, 0xe8bd87f0u});
    assert(function.size() * 4 < 4096);
    std::memcpy(memory, function.data(), function.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
                           reinterpret_cast<char *>(memory) + function.size() * 4);
    for (unsigned trial = 0; trial < 32; ++trial) {
      std::array<uint32_t, 28> actual;
      for (auto &v : actual) v = random();
      // Include both branch outcomes, signed loads, full masks, PC/cycle wrap.
      if (trial < 4) {
        actual[17] = trial & 1 ? 0xfffffffcu : 0;
        actual[18] = trial & 2 ? 0xfffffffeu : 0;
      }
      const uint32_t value = trial < 4 ? (trial & 1 ? 0xffffffffu : 0u) : random();
      const uint32_t wait = trial % 7;
      actual[25] = reinterpret_cast<uint32_t>(&Byte);
      actual[26] = reinterpret_cast<uint32_t>(&Word);
      actual[27] = reinterpret_cast<uint32_t>(&Long);
      auto expected = actual;
      auto native = actual;
      uint32_t receipt[3] = {};
      sh2_poll_step_oracle(expected.data(), ops, words, value, wait, trial & 1, receipt);
      unsigned calls = 0;
      sh2a9::PollStep::Run(desc, actual.data(), actual[16], actual[17], actual[18],
        [&](uint32_t address, unsigned bytes) {
          ++calls;
          assert(address == receipt[0] && bytes == receipt[1]);
          if (trial & 1) { actual[15] ^= 0x76543210u; actual[16] ^= 0x101u; }
          return value;
        });
      actual[18] += wait; // Existing caller accounts for callback memory cycles.
      assert(actual == expected && calls == 1 && receipt[2] == 1);
      callback_state = native.data(); callback_value = value;
      callback_mutate = trial & 1; callback_calls = 0;
      reinterpret_cast<void (*)(uint32_t *)>(memory)(native.data());
      native[18] += wait;
      assert(native == expected && callback_calls == 1);
      assert(callback_address == receipt[0] && callback_width == receipt[1]);
      ++cases;
    }
    // A delayed branch, a changed displacement or extra operation is rejected.
    auto bad = std::array<uint16_t, 5>{};
    for (unsigned i = 0; i < words; ++i) bad[i] = ops[i];
    bad[words - 1] ^= 0x0400; // BF -> BF/S
    assert(!sh2a9::PollStep::Decode(bad.data(), words));
  }
  munmap(memory, 4096);
  std::printf("Poll step: %u interpreter comparisons for C++ AND emitted A32, aliases/branches/waits/wraparound\n", cases);
}
