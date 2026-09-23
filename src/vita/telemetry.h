/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_TELEMETRY_H
#define VITA_TELEMETRY_H
#include "telemetry_accounting.h"
#include "telemetry_policy.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional owner-local sample cookie; manual Enter/Leave callers may ignore it. */
void *VitaTelemetryEnter(VitaTelemetryPhase phase);
void VitaTelemetryLeave(VitaTelemetryPhase phase);
void VitaTelemetryLeaveSample(void *sample);
void VitaTelemetryReport(void);
void VitaTelemetryThread(const char *role);
/* Set once, before starting emulation/worker threads. Values in
 * telemetry_policy.h: OFF, OVERVIEW (folds hot children into parents), DETAILED,
 * SAMPLED (overview plus separate inclusive samples of hot children). */
void VitaTelemetryConfigure(int mode);
/* Read-only to callers; Configure writes this before workers start. Constant
 * hot phases can reject omitted scopes without two out-of-line calls. */
extern int VitaTelemetryMode;
static inline int VitaTelemetryScopeEnabled(VitaTelemetryPhase phase) {
  return VitaTelemetryMode == VT_SAMPLED ||
    VitaTelemetryPhaseEnabled(VitaTelemetryMode, phase);
}
int VitaTelemetrySamplingEnabled(void);
int VitaTelemetryCheckThreadIsolation(void);
#ifdef __cplusplus
}
class VitaTelemetryScope {
  VitaTelemetryPhase phase;
  bool active;
  void *sample;
public:
  explicit VitaTelemetryScope(VitaTelemetryPhase p) : phase(p), active(VitaTelemetryScopeEnabled(p)),
    sample(active ? VitaTelemetryEnter(p) : nullptr) {}
  ~VitaTelemetryScope() { if (!active) return; if (sample) VitaTelemetryLeaveSample(sample); else VitaTelemetryLeave(phase); }
  VitaTelemetryScope(const VitaTelemetryScope&) = delete;
};
#else
typedef struct { VitaTelemetryPhase phase; int active; void *sample; } VitaTelemetryScope;
static inline VitaTelemetryScope VitaTelemetryScopeStart(VitaTelemetryPhase p) {
  int active = VitaTelemetryScopeEnabled(p);
  return (VitaTelemetryScope){p, active, active ? VitaTelemetryEnter(p) : NULL};
}
static inline void VitaTelemetryScopeEnd(VitaTelemetryScope *s) {
  if (!s->active) return;
  if (s->sample) VitaTelemetryLeaveSample(s->sample); else VitaTelemetryLeave(s->phase);
}
#endif
#define VT_JOIN_INNER(a,b) a##b
#define VT_JOIN(a,b) VT_JOIN_INNER(a,b)
#ifdef __cplusplus
#define VT_SCOPE(p) VitaTelemetryScope VT_JOIN(vt_scope_,__LINE__)(p)
#else
#define VT_SCOPE(p) VitaTelemetryScope VT_JOIN(vt_scope_,__LINE__) \
  __attribute__((cleanup(VitaTelemetryScopeEnd))) = VitaTelemetryScopeStart(p)
#endif
#endif
