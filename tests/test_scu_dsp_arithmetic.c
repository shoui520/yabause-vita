/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/scu_dsp_arithmetic.h"
#include <assert.h>
#include <stdio.h>

static unsigned cases;
/* Keep named, non-inlined entries for inspecting target code generation. */
__attribute__((noinline)) int32_t TestAdd32(uint32_t a, uint32_t b) {
  return ScuDspAdd32(a,b);
}
__attribute__((noinline)) int32_t TestSub32(uint32_t a, uint32_t b) {
  return ScuDspSub32(a,b);
}
static void check(uint32_t a, uint32_t b) {
  uint64_t sum = (uint64_t)a + b;
  int64_t difference = (int64_t)a - b;
  uint32_t add_bits = (uint32_t)sum;
  uint32_t sub_bits = (uint32_t)difference;
  int64_t add = (int64_t)add_bits - ((int64_t)(add_bits >> 31) << 32);
  int64_t sub = (int64_t)sub_bits - ((int64_t)(sub_bits >> 31) << 32);
  assert(TestAdd32(a,b) == add);
  assert(TestSub32(a,b) == sub);
  assert(ScuDspCarry32(a,b) == (sum >> 32));
  assert(ScuDspBorrow32(a,b) == (difference < 0));
  ++cases;
}
int main(void) {
  const uint32_t edge[] = {0,1,2,0x7ffffffe,0x7fffffff,0x80000000,
                           0x80000001,0xfffffffe,0xffffffff};
  for (unsigned i=0;i<sizeof(edge)/sizeof(*edge);++i)
    for (unsigned j=0;j<sizeof(edge)/sizeof(*edge);++j) check(edge[i],edge[j]);
  uint32_t rng=0x534355;
  for (unsigned i=0;i<1000000;++i) {
    rng = rng * 1664525u + 1013904223u;
    uint32_t a=rng;
    rng = rng * 1664525u + 1013904223u;
    check(a,rng);
  }
  printf("SCU DSP arithmetic: %u cases passed\n",cases);
}
