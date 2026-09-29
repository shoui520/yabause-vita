/* SH2Div64 against C's / and % on edge cases and random operands
 * (argument: random iterations; 50000000 passed on Cortex-A9 hardware). */
#include <stdio.h>
#include <stdlib.h>
#include "../src/core/sh2_div64.h"

static u64 rng = 0x9E3779B97F4A7C15ull;
static u64 next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int check(s64 n, s32 d) {
  if (!d || (n == INT64_MIN && d == -1)) return 0;
  s64 q; s32 r;
  SH2Div64(n, d, &q, &r);
  if (q != n / d || r != (s32)(n % d)) {
    printf("FAIL n=%lld d=%d q=%lld/%lld r=%d/%d\n", (long long)n, d, (long long)q, (long long)(n / d), r, (s32)(n % d));
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const long iterations = argc > 1 ? atol(argv[1]) : 2000000;
  int fail = 0;
  static const s64 ns[] = {0, 1, -1, 2, -2, 0x7FFFFFFF, -0x80000000ll, 0x80000000ll, 0xFFFFFFFFll, 0x100000000ll,
                           0x3FFFFFFFC0000000ll, -0x3FFFFFFFC0000000ll, 0x3FFFFFFF80000001ll, INT64_MAX, INT64_MIN};
  static const s32 ds[] = {1, -1, 2, -2, 3, 7, 0x7FFFFFFF, -0x7FFFFFFF - 1, 0x10000, -0x10000, 0x7FFFFFFE, 65535};
  for (unsigned i = 0; i < sizeof ns / sizeof *ns; ++i)
    for (unsigned j = 0; j < sizeof ds / sizeof *ds; ++j)
      for (int k = -3; k <= 3; ++k) fail |= check(ns[i] + k, ds[j]);
  for (long i = 0; i < iterations && !fail; ++i) {
    const u64 a = next(), b = next();
    s32 d = (s32)b;
    switch (a & 3) {                                /* bias toward in-range quotients */
      case 0: d >>= (b >> 32) & 31; break;
      case 1: d = (s32)(b >> 40) | 1; break;
      default: break;
    }
    if (!d) continue;
    s64 n = (s64)a >> ((a >> 58) & 31);
    fail |= check(n, d);
    /* exact multiples and neighbours: the estimate's boundary cases */
    const s64 m = (s64)(s32)(a >> 33) * d;
    fail |= check(m, d) | check(m + 1, d) | check(m - 1, d);
  }
  puts(fail ? "FAIL" : "PASS");
  return fail;
}
