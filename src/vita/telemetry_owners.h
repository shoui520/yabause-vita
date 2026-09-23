/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_TELEMETRY_OWNERS_H
#define VITA_TELEMETRY_OWNERS_H
#include <stdatomic.h>
#include "telemetry_sampling.h"
#include "telemetry_accounting.h"
#define VT_SAMPLE_OWNERS 32
/* Slots are never recycled during a process. Only identity publication is
 * shared; a matching thread alone may touch its counters. Static storage avoids
 * dangling TLS pointers when an owner exits. Cleanup revokes its identity. */
typedef struct {
  _Atomic int tid[VT_SAMPLE_OWNERS];
  VitaTelemetrySampler phase[VT_SAMPLE_OWNERS][VT_PHASE_COUNT];
} VitaTelemetryOwners;
static inline int VitaTelemetryOwnerFind(VitaTelemetryOwners *o, int tid) {
  if (tid <= 0) return -1;
  for (unsigned i = 0; i < VT_SAMPLE_OWNERS; ++i)
    if (atomic_load_explicit(&o->tid[i], memory_order_acquire) == tid) return (int)i;
  return -1;
}
static inline void VitaTelemetryOwnerPublish(VitaTelemetryOwners *o, unsigned slot, int tid) {
  atomic_store_explicit(&o->tid[slot], tid, memory_order_release);
}
#endif
