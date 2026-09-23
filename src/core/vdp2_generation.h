/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VDP2_GENERATION_H
#define VDP2_GENERATION_H
#include <stdint.h>
/* UINT32_MAX epoch is permanently untrusted, not a wrapping cache identity. */
static inline void Vdp2GenerationAdvance(uint32_t *low,uint32_t *epoch) {
  if (++*low == 0 && *epoch != UINT32_MAX) ++*epoch;
}
#endif
