/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SH2_DIV64_H
#define SH2_DIV64_H
#include "core.h"
/* Signed 64/32 quotient and remainder, bit-identical to C's / and % (d != 0).
 * The A9 has no integer divider and libgcc's 64-bit divide is a bit loop.
 * When |quotient| < 2^31 a double-precision estimate is within one of it
 * (two roundings, relative error below 2^-51), and one integer correction
 * against the exact remainder makes it exact. Anything else uses libgcc. */
static inline void SH2Div64(s64 n, s32 d, s64 *q, s32 *r)
{
#if defined(__arm__) && defined(__ARM_FP) && (__ARM_FP & 8)
   const u64 un = n < 0 ? 0 - (u64)n : (u64)n;
   const u32 ud = d < 0 ? 0 - (u32)d : (u32)d;
   if ((un >> 31) < ud) {
      const double est = ((double)(u32)(un >> 32) * 4294967296.0 + (double)(u32)un) / (double)ud;
      u32 uq = (u32)est;
      s64 rem = (s64)(un - (u64)uq * ud);
      if (rem < 0) { uq--; rem += ud; }
      else if (rem >= (s64)ud) { uq++; rem -= ud; }
      *q = (n < 0) != (d < 0) ? -(s64)uq : (s64)uq;
      *r = n < 0 ? -(s32)rem : (s32)rem;
      return;
   }
#endif
   *q = n / d;
   *r = (s32)(n % d);
}
#endif
