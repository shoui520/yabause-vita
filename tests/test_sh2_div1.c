/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void sh2_div1_a9(uint32_t *, unsigned, unsigned);
extern void sh2_div1_oracle(uint32_t *, unsigned, unsigned);
static unsigned cases;
static void check(uint32_t a, uint32_t b, uint32_t sr, unsigned m, unsigned n) {
  uint32_t actual[25], expected[25];
  for (unsigned i = 0; i < 25; ++i) actual[i] = 0x31415926u + i;
  actual[m] = b; actual[n] = a; actual[16] = sr;
  memcpy(expected, actual, sizeof(actual));
  sh2_div1_oracle(expected, m, n);
  sh2_div1_a9(actual, m * 4, n * 4);
  /* Templates' surrounding compiler code owns PC/cycle accounting. */
  actual[22] += 2; actual[23] += 1;
  assert(memcmp(actual, expected, sizeof(actual)) == 0);
  ++cases;
}
int main(void) {
  const uint32_t edges[] = {0,1,2,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n)
      for (unsigned f = 0; f < 8; ++f)
        for (unsigned a = 0; a < 7; ++a)
          for (unsigned b = 0; b < 7; ++b)
            check(edges[a], edges[b], 0xf2 | (f & 1) | ((f & 6) << 7), m, n);
  uint32_t r = 0x534832;
  for (unsigned i = 0; i < 1000000; ++i) {
    r = r * 1664525u + 1013904223u; uint32_t a = r;
    r = r * 1664525u + 1013904223u; uint32_t b = r;
    r = r * 1664525u + 1013904223u;
    check(a, b, r & 0x3f3, (r >> 16) & 15, (r >> 24) & 15);
  }
  printf("SH-2 DIV1 native/interpreter: %u cases passed\n", cases);
}
