/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/native_hot_samples.h"
#include <cassert>
#include <cstdio>
int main() {
  sh2a9::NativeHotSamples samples;
  samples.Add(0, false, 0, 1); // PC zero and zero-duration are valid samples.
  samples.Add(0, true, 10, 2);
  samples.Add(0, false, 20, 3);
  auto ranked = samples.Ranked();
  assert(ranked[0].samples == 2 && ranked[0].us == 20 && ranked[0].cycles == 4);
  assert(!ranked[0].slave && ranked[1].slave && ranked[1].samples == 1);
  assert(samples.samples == 3 && !samples.dropped);
  samples = {};
  for (unsigned i = 0; i < 128; ++i) samples.Add(i * 256, false, i, i + 1);
  samples.Add(0x12345678, false, 999, 999);
  samples.Add(127 * 256, false, 1000, 7); // Existing key survives saturation.
  ranked = samples.Ranked();
  assert(samples.samples == 130 && samples.dropped == 1);
  assert(ranked[0].pc == 127 * 256 && ranked[0].samples == 2);
  assert(ranked[0].us == 1127 && ranked[0].cycles == 135);
  uint64_t retained = 0;
  for (const auto &entry : ranked) retained += entry.samples;
  assert(retained + samples.dropped == samples.samples);
  samples = {};
  assert(!samples.samples && !samples.dropped && !samples.Ranked()[0].samples);
  samples.Add(2, false, 1, 1, true, 0);
  samples.Add(2, false, 1, 1, true, 0xffffffffu);
  samples.Add(2, false, 1, 1, false, 123);
  ranked = samples.Ranked();
  assert(ranked[0].load_samples == 2 && ranked[0].address_min == 0 &&
         ranked[0].address_max == 0xffffffffu && ranked[0].samples == 3);
  std::puts("Native hot samples: zero PC/time, CPU identity, collisions, saturation, ranking and reset passed");
}
