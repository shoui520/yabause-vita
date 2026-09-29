/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_ROTATION_ROUTE_H
#define VITA_ROTATION_ROUTE_H
#include <stdint.h>
/* Chooses who draws GPU-admissible rotation screens: the SGX shader (0) or
 * the CPU rows (1). Both produce the same pixels; only the cost moves.
 * The GPU path is kept unless frames are slow while presentation waits on
 * the GPU; the other path is then tried for one window and kept only if the
 * frame period did not get worse. Failed tries back off. */
enum { ROUTE_WINDOW = 30, ROUTE_SETTLE = 6, ROUTE_BACKOFF_MIN = 120, ROUTE_BACKOFF_MAX = 1800 };
enum { ROUTE_SLOW_US = 17200 }; /* period above this: below ~58 fps */

typedef struct {
  int cpu;            /* current route */
  int probing;        /* 1 while trying the other route */
  uint32_t settle;    /* frames to discard after a switch */
  uint32_t frames;    /* frames in the current window */
  uint64_t wall_us, wait_us;
  uint32_t eligible;  /* admissible rotation screens in the window */
  uint32_t base_period;
  uint32_t holdoff, backoff;
  uint32_t switches;
} VitaRotationRoute;

static inline void VitaRotationRouteInit(VitaRotationRoute *r) {
  *r = (VitaRotationRoute){0};
  r->backoff = ROUTE_BACKOFF_MIN;
}

static inline void VitaRotationRouteSwitch(VitaRotationRoute *r) {
  r->cpu = !r->cpu;
  r->settle = ROUTE_SETTLE;
  ++r->switches;
}

/* One emulated frame: its wall time, the presentation wait inside it and
 * the admissible rotation screens it drew. Returns the route to use next. */
static inline int VitaRotationRouteFrame(VitaRotationRoute *r, uint32_t wall_us,
                                         uint32_t wait_us, uint32_t eligible) {
  if (r->settle) { --r->settle; return r->cpu; }
  r->wall_us += wall_us; r->wait_us += wait_us; r->eligible += eligible;
  if (++r->frames < ROUTE_WINDOW) return r->cpu;
  const uint32_t period = (uint32_t)(r->wall_us / r->frames);
  const uint64_t wall = r->wall_us, wait = r->wait_us;
  const uint32_t n = r->frames, seen = r->eligible;
  r->frames = 0; r->wall_us = r->wait_us = 0; r->eligible = 0;
  if (r->probing) {
    r->probing = 0;
    if ((uint64_t)period * 100 > (uint64_t)r->base_period * 105) {
      VitaRotationRouteSwitch(r); /* worse: go back */
      r->holdoff = r->backoff;
      if (r->backoff < ROUTE_BACKOFF_MAX) r->backoff *= 2;
    } else {
      r->backoff = ROUTE_BACKOFF_MIN;
    }
    return r->cpu;
  }
  if (r->holdoff) { r->holdoff = r->holdoff > n ? r->holdoff - n : 0; return r->cpu; }
  if (!seen || period <= ROUTE_SLOW_US) return r->cpu;
  /* GPU route: presentation waiting says the SGX is behind. CPU route: little
   * waiting says the ARM side is the limit, so the GPU may do better. */
  const int try_other = r->cpu ? wait * 10 < wall : wait * 4 >= wall;
  if (try_other) {
    r->base_period = period;
    r->probing = 1;
    VitaRotationRouteSwitch(r);
  }
  return r->cpu;
}
#endif
