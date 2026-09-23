/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ROTATION_COEFF_PAIR_H
#define YGL_ROTATION_COEFF_PAIR_H
#include <stdint.h>
#include <string.h>
/* Nearest F32 queries: admit only normal finite values and positive zero.
 * Preserve unusual encodings with the original byte-reconstruction shader. */
static inline int YglRotationPairSafe(const float rows[][4], unsigned height) {
  for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<2; ++x) {
    uint32_t bits; memcpy(&bits,&rows[y][x],sizeof(bits));
    uint32_t exponent=bits&0x7f800000u;
    if (exponent==0x7f800000u || (!exponent && bits)) return 0;
  }
  return 1;
}
#endif
