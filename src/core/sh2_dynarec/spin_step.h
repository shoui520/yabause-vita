/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/* Advance a proven spin cycle to the end of a slice. S holds the recorded
 * cumulative cycle counts of positions j..n (position n is position j one
 * period later; S[j..n] never decreases across that range). From position q
 * (j <= q < n) with slice count *count < target and target - *count at most
 * one period (S[n] - S[j]), this gives the result of
 *
 *   while (count < target) { count += S[q + 1] - S[q]; if (++q == n) { q = j; ++m; } }
 *
 * with a binary search over the prefix sums instead of one step per block.
 * Only differences of S are used, so the counts may wrap. */
static inline void SpinStepTo(const uint32_t *S, unsigned j, unsigned n, unsigned *q, uint32_t *count,
                              uint32_t *m, uint32_t target) {
  unsigned p = *q;
  uint32_t need = target - *count;
  if (need > S[n] - S[p]) {                 // passes position n: wraps to j once
    *count += S[n] - S[p];
    need -= S[n] - S[p];
    p = j;
    ++*m;
  }
  unsigned lo = p + 1, hi = n;              // S[n] - S[p] >= need: the answer is in [lo, hi]
  while (lo < hi) {
    const unsigned mid = (lo + hi) >> 1;
    if (S[mid] - S[p] >= need) hi = mid; else lo = mid + 1;
  }
  *count += S[lo] - S[p];
  if (lo == n) { lo = j; ++*m; }
  *q = lo;
}
