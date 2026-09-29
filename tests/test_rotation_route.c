/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/vita/rotation_route.h"

/* Runs n frames of a scene whose period/wait depend on the route. */
static void run(VitaRotationRoute *r, unsigned n, const uint32_t period[2],
                const uint32_t wait[2], uint32_t eligible) {
  for (unsigned i = 0; i < n; ++i) {
    const int cpu = r->cpu;
    VitaRotationRouteFrame(r, period[cpu], wait[cpu], eligible);
  }
}

int main(void) {
  VitaRotationRoute r;
  /* GPU-bound scene where the CPU rows are faster: moves and stays. */
  VitaRotationRouteInit(&r);
  run(&r, 600, (uint32_t[]){26600, 16700}, (uint32_t[]){12000, 3000}, 1);
  assert(r.cpu == 1 && r.switches == 1 && !r.probing);
  /* Same GPU wait but the CPU rows are slower: goes back, backs off. */
  VitaRotationRouteInit(&r);
  run(&r, ROUTE_SETTLE + 2 * ROUTE_WINDOW, (uint32_t[]){26600, 30000}, (uint32_t[]){12000, 1000}, 1);
  assert(r.cpu == 0 && r.switches == 2 && r.holdoff == ROUTE_BACKOFF_MIN);
  const uint32_t before = r.switches;
  run(&r, ROUTE_BACKOFF_MIN - ROUTE_WINDOW, (uint32_t[]){26600, 30000}, (uint32_t[]){12000, 1000}, 1);
  assert(r.switches == before); /* held off */
  run(&r, 2000, (uint32_t[]){26600, 30000}, (uint32_t[]){12000, 1000}, 1);
  assert(r.cpu == 0 && r.backoff > ROUTE_BACKOFF_MIN);
  /* Full speed, CPU-bound, or no rotation screen: never leaves the GPU. */
  VitaRotationRouteInit(&r);
  run(&r, 3000, (uint32_t[]){16700, 10000}, (uint32_t[]){12000, 0}, 1);
  run(&r, 3000, (uint32_t[]){26600, 10000}, (uint32_t[]){1000, 0}, 1);
  run(&r, 3000, (uint32_t[]){26600, 10000}, (uint32_t[]){12000, 0}, 0);
  assert(r.cpu == 0 && r.switches == 0);
  /* On the CPU rows, becoming CPU-limited tries the GPU again. */
  VitaRotationRouteInit(&r);
  run(&r, 200, (uint32_t[]){26600, 16700}, (uint32_t[]){12000, 3000}, 1);
  assert(r.cpu == 1);
  run(&r, 200, (uint32_t[]){20000, 25000}, (uint32_t[]){8000, 500}, 1);
  assert(r.cpu == 0);
  puts("rotation_route: ok");
  return 0;
}
