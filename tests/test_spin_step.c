/* SPDX-License-Identifier: GPL-2.0-or-later
 * SpinStepTo equals the block-by-block stepping loop it replaces, for random
 * recorded cycles (including zero-cycle positions and wrapped counts). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/core/sh2_dynarec/spin_step.h"

static void reference(const uint32_t *S, unsigned j, unsigned n, unsigned *q, uint32_t *count, uint32_t *m,
                      uint32_t target) {
  while (*count < target) {
    *count += S[*q + 1] - S[*q];
    if (++*q == n) { *q = j; ++*m; }
  }
}

int main(void) {
  srand(12345);
  uint32_t S[97];
  unsigned long cases = 0;
  for (int it = 0; it < 200000; ++it) {
    const unsigned n = 1 + (unsigned)rand() % 96, j = (unsigned)rand() % n;
    S[0] = (uint32_t)rand() * 2654435761u;             // arbitrary base: counts may wrap
    for (unsigned k = 1; k <= n; ++k) S[k] = S[k - 1] + (uint32_t)(rand() % 4 ? rand() % 9 : 0);
    if (S[n] == S[j]) S[n] += 1 + (uint32_t)rand() % 5; // a period takes time
    const uint32_t cycle = S[n] - S[j];
    const unsigned q0 = j + (unsigned)rand() % (n - j);
    const uint32_t count0 = (uint32_t)rand() % 1000, target = count0 + 1 + (uint32_t)rand() % cycle;
    unsigned qa = q0, qb = q0;
    uint32_t ca = count0, cb = count0, ma = 7, mb = 7;
    reference(S, j, n, &qa, &ca, &ma, target);
    SpinStepTo(S, j, n, &qb, &cb, &mb, target);
    if (qa != qb || ca != cb || ma != mb) {
      fprintf(stderr, "mismatch n=%u j=%u q0=%u: ref q=%u c=%u m=%u, fast q=%u c=%u m=%u\n", n, j, q0, qa, ca, ma, qb,
              cb, mb);
      return 1;
    }
    ++cases;
  }
  printf("spin_step: %lu cases identical\n", cases);
  return 0;
}
