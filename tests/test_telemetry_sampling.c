/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/telemetry_sampling.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  VitaTelemetrySampler a = {0}, b = {0};
  unsigned chosen = 0;
  for (unsigned i = 0; i < 1000000; ++i) {
    int take = VitaTelemetrySampleEnter(&a, 17);
    assert(take == VitaTelemetrySampleEnter(&b, 17));
    if (take) { a.start = b.start = 100; ++chosen; }
    assert(!VitaTelemetrySampleEnter(&a, 17)); // recursion is not double timed
    assert(!VitaTelemetrySampleLeave(&a));
    assert(VitaTelemetrySampleLeave(&a) == take);
    assert(VitaTelemetrySampleLeave(&b) == take);
    if (take) { VitaTelemetrySampleComplete(&a, 109); VitaTelemetrySampleComplete(&b, 109); }
  }
  assert(chosen > 800 && chosen < 1200);
  assert(a.entries == 2000000 && a.roots == 1000000);
  assert(a.samples == chosen && a.inclusive_us == chosen * 9u && a.max_us == 9);
  assert(a.inclusive_us == b.inclusive_us && a.depth == 0 && a.errors == 0);
  while (!VitaTelemetrySampleEnter(&a, 17)) assert(!VitaTelemetrySampleLeave(&a));
  a.start = 500;
  uint32_t random = a.random;
  VitaTelemetrySampleResetWindow(&a);
  assert(a.random == random && a.depth == 1 && a.selected);
  assert(VitaTelemetrySampleLeave(&a));
  VitaTelemetrySampleComplete(&a, 530);
  assert(a.entries == 0 && a.samples == 1 && a.inclusive_us == 30);
  assert(!VitaTelemetrySampleLeave(&a) && a.errors == 1);
  a.start = 900; VitaTelemetrySampleComplete(&a, 800);
  assert(a.errors == 2 && a.samples == 1);
  puts("telemetry sampling: million-call, recursion, window and error checks passed");
}
