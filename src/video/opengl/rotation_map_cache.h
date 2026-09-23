/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ROTATION_MAP_CACHE_H
#define YGL_ROTATION_MAP_CACHE_H
#include <stdint.h>
#include <string.h>
/* No row/transform fields: they affect sampling, not decoded map contents.
 * CRAM is also absent: indexed pixels remain encoded palette references. */
typedef struct {
  uint32_t generation;
  float bitmap[4], pixel[3], map[4], pattern[4], palette[4], special[4], planes[16];
} VitaRotationMapKey;
typedef struct {
  VitaRotationMapKey resident, candidate;
  unsigned valid, candidate_valid, repeats;
} VitaRotationMapPolicy;
/* Only call for admitted tiled maps. Equal plane bases have identical local
 * decoding. Find a rectangular period of the 4x4 plane-address table, keeping
 * screen-over tests in logical coordinates in the sampling shader. */
static inline unsigned YglRotationMapExtent(const VitaRotationMapKey *k, unsigned axis) {
  unsigned period;
  for (period=1; period<4; period*=2) {
    unsigned same=1;
    for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x) {
      unsigned source = axis ? (y%period)*4+x : y*4+x%period;
      if (k->planes[y*4+x] != k->planes[source]) same=0;
    }
    if (same) break;
  }
  unsigned extent = period * (1u << (unsigned)k->map[axis]);
  unsigned logical = (unsigned)k->bitmap[axis+1];
  return extent < logical ? extent : logical;
}
static inline int YglRotationMapAdmit(const VitaRotationMapKey *k) {
  if (k->pattern[3] != 0 || (k->bitmap[3] != 0 && k->bitmap[3] != 2)) return 0;
  if ((k->map[0] != 9 && k->map[0] != 10) ||
      (k->map[1] != 9 && k->map[1] != 10)) return 0;
  for (unsigned i=1; i<=2; ++i) {
    float f = k->bitmap[i];
    if (!(f >= 512 && f <= 2048)) return 0;
    unsigned n = (unsigned)f;
    if ((float)n != f || (n & (n-1))) return 0;
  }
  return 1;
}
/* 1: hit; 2: stable miss worth attempting; 0: baseline. Commit separately,
 * only after the complete GPU decode has actually been queued. */
static inline int YglRotationMapObserve(VitaRotationMapPolicy *p,
                                        const VitaRotationMapKey *k) {
  if (!YglRotationMapAdmit(k)) return 0;
  if (p->valid && !memcmp(&p->resident,k,sizeof(*k))) {
    p->candidate_valid = 0; p->repeats = 0;
    return 1;
  }
  if (!p->candidate_valid || memcmp(&p->candidate,k,sizeof(*k))) {
    p->candidate = *k; p->candidate_valid = 1; p->repeats = 1;
    return 0;
  }
  if (p->repeats < 8) ++p->repeats;
  return p->repeats == 8 ? 2 : 0;
}
static inline void YglRotationMapCommit(VitaRotationMapPolicy *p,
                                       const VitaRotationMapKey *k) {
  p->resident = *k; p->valid = 1;
}
#endif
