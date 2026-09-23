/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_TELEMETRY_SAMPLING_H
#define VITA_TELEMETRY_SAMPLING_H
#include <stdint.h>
/* One phase, one owner. Samples are inclusive outermost calls and overlap
 * other phases. Never add them to exclusive accounting or call them CPU time.
 * A fixed-seed PRNG avoids a fixed every-N-call cadence, but does not establish
 * unbiased workload coverage. Report raw samples, not extrapolated totals. */
typedef struct {
  uint64_t entries, roots, samples, inclusive_us, max_us, errors;
  uint64_t start;
  uint32_t random, depth, selected;
} VitaTelemetrySampler;
static inline int VitaTelemetrySampleEnter(VitaTelemetrySampler *s, uint32_t seed) {
  ++s->entries;
  if (s->depth++) return 0;
  ++s->roots;
  uint32_t x = s->random ? s->random : seed;
  if (!x) x = 1;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  s->random = x;
  s->selected = (x & 1023u) == 0;
  return s->selected;
}
/* Returns whether the caller must take a clock reading. */
static inline int VitaTelemetrySampleLeave(VitaTelemetrySampler *s) {
  if (!s->depth) { ++s->errors; return 0; }
  return --s->depth == 0 && s->selected;
}
static inline void VitaTelemetrySampleComplete(VitaTelemetrySampler *s, uint64_t now) {
  s->selected = 0;
  if (now < s->start) { ++s->errors; return; }
  uint64_t elapsed = now - s->start;
  ++s->samples;
  s->inclusive_us += elapsed;
  if (elapsed > s->max_us) s->max_us = elapsed;
}
/* Retain open samples and PRNG state across reports. Full durations belong to
 * the completion window; entries/roots belong to the start window. */
static inline void VitaTelemetrySampleResetWindow(VitaTelemetrySampler *s) {
  s->entries = s->roots = s->samples = s->inclusive_us = s->max_us = s->errors = 0;
}
#endif
