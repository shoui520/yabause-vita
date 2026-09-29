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
#ifdef VITA_STACK_PROFILE
/* Diagnostic build only: every scope on the emulation thread pushes its phase
 * id (no clock read); a sampler thread on another core histograms the top of
 * the stack. The owner is identified by TPIDRURO (user read-only thread ID
 * register, ARMv7-A ARM B4.1.150), readable without a system call. */
extern volatile unsigned char vt_stack[64];
extern volatile unsigned vt_stack_depth;
extern uintptr_t vt_stack_owner;
/* Second sampled thread (the sound worker), same scheme. */
extern volatile unsigned char vt_stack2[64];
extern volatile unsigned vt_stack2_depth;
extern uintptr_t vt_stack2_owner;
void VitaStackAdoptSecondThread(void);
/* Master SH-2 block sampling: *vt_pc_src holds the running Block*, whose
 * start PC is the u32 at byte offset vt_pc_off. */
extern const volatile uintptr_t *vt_pc_src;
extern unsigned vt_pc_off;
static inline uintptr_t VitaThreadPointer(void) {
  uintptr_t value; __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value)); return value;
}
static inline void VitaStackPush(unsigned phase) {
  const uintptr_t tp = VitaThreadPointer();
  if (tp == vt_stack_owner) {
    unsigned d = vt_stack_depth; if (d < 64) vt_stack[d] = (unsigned char)phase; vt_stack_depth = d + 1;
  } else if (tp == vt_stack2_owner) {
    unsigned d = vt_stack2_depth; if (d < 64) vt_stack2[d] = (unsigned char)phase; vt_stack2_depth = d + 1;
  }
}
static inline void VitaStackPop(void) {
  const uintptr_t tp = VitaThreadPointer();
  if (tp == vt_stack_owner) { if (vt_stack_depth) vt_stack_depth = vt_stack_depth - 1; }
  else if (tp == vt_stack2_owner) { if (vt_stack2_depth) vt_stack2_depth = vt_stack2_depth - 1; }
}
#endif
#if defined(A9_PMU_REGIONS) && !defined(VITA_STACK_PROFILE)
/* ARMv7 Linux profiling build: the phase pushes attribute PMU counts. */
void A9PmuPush(unsigned phase);
void A9PmuPop(void);
static inline void VitaStackPush(unsigned phase) { A9PmuPush(phase); }
static inline void VitaStackPop(void) { A9PmuPop(); }
#endif
#ifdef VITA_STACK_PROFILE
#define VT_STACK_BEGIN(p) VitaStackPush(p)
#define VT_STACK_END() VitaStackPop()
#else
#define VT_STACK_BEGIN(p) ((void)0)
#define VT_STACK_END() ((void)0)
#endif
static inline int VitaTelemetryScopeEnabled(VitaTelemetryPhase phase) {
#ifdef VITA_STACK_PROFILE
  (void)phase; return VitaTelemetryMode != VT_OFF;
#else
  return VitaTelemetryMode == VT_SAMPLED ||
    VitaTelemetryPhaseEnabled(VitaTelemetryMode, phase);
#endif
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
