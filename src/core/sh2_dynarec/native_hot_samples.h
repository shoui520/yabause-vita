/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
namespace sh2a9 {
// Bounded diagnostic aggregation, never an execution or code-ownership cache.
// Saturation is counted explicitly. Periodic sampling is not an unbiased profile.
struct NativeHotSamples {
  struct Entry {
    uint32_t pc = 0;
    bool slave = false;
    uint64_t samples = 0, us = 0, cycles = 0;
    uint64_t load_samples = 0;
    uint32_t address_min = 0, address_max = 0;
  };
  std::array<Entry, 128> entries{};
  uint64_t samples = 0, dropped = 0;
  void Add(uint32_t pc, bool slave, uint64_t us, uint32_t cycles,
           bool starts_with_load = false, uint32_t address = 0) {
    ++samples;
    const unsigned first = ((pc >> 1) ^ (pc >> 12) ^ unsigned(slave)) & 127;
    for (unsigned probe = 0; probe < entries.size(); ++probe) {
      auto &entry = entries[(first + probe) & 127];
      if (!entry.samples || (entry.pc == pc && entry.slave == slave)) {
        entry.pc = pc; entry.slave = slave;
        ++entry.samples; entry.us += us; entry.cycles += cycles;
        if (starts_with_load) {
          if (!entry.load_samples) entry.address_min = entry.address_max = address;
          else {
            entry.address_min = std::min(entry.address_min, address);
            entry.address_max = std::max(entry.address_max, address);
          }
          ++entry.load_samples;
        }
        return;
      }
    }
    ++dropped;
  }
  std::array<Entry, 128> Ranked() const {
    auto result = entries;
    std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
      if (a.us != b.us) return a.us > b.us;
      if (a.samples != b.samples) return a.samples > b.samples;
      if (a.pc != b.pc) return a.pc < b.pc;
      return a.slave < b.slave;
    });
    return result;
  }
};
}
