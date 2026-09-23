/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "telemetry.h"
#include "telemetry_policy.h"
#include "telemetry_sampling.h"
#include "telemetry_owners.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>

static void append_report(char *buffer, size_t capacity, size_t *used,
                          const char *format, ...) {
  if (*used >= capacity) return;
  va_list ap; va_start(ap, format);
  int length = vsnprintf(buffer + *used, capacity - *used, format, ap);
  va_end(ap);
  if (length < 0 || (size_t)length + 1 >= capacity - *used) {
    *used = capacity;
    return;
  }
  *used += length;
  buffer[(*used)++] = '\n'; buffer[*used] = 0;
}

extern void YuiMsg(const char *, ...);
int VitaTelemetryMode = VT_OVERVIEW;
int VitaTelemetrySamplingEnabled(void) { return VitaTelemetryMode == VT_SAMPLED; }
static VitaTelemetryOwners sample_owners;
static pthread_key_t sample_owner_key;
static int sample_owner_key_ready;
static void release_sample_owner(void *cookie) {
  unsigned slot = (unsigned)(uintptr_t)cookie - 1;
  if (slot < VT_SAMPLE_OWNERS) VitaTelemetryOwnerPublish(&sample_owners, slot, 0);
}
void VitaTelemetryConfigure(int value) {
  VitaTelemetryMode = value >= VT_OFF && value <= VT_SAMPLED ? value : VT_OVERVIEW;
  // Configuration occurs before any worker starts. The destructor revokes
  // thread identity, including library-created owners and pthread_exit paths.
  if (VitaTelemetryMode == VT_SAMPLED && !sample_owner_key_ready) {
    if (pthread_key_create(&sample_owner_key, release_sample_owner)) {
      YuiMsg("telemetry_sample_owner_key_failed");
      __builtin_trap();
    }
    sample_owner_key_ready = 1;
  }
}
static _Thread_local VitaTelemetryAccounting accounting;
static _Thread_local int initialized, reporting;
static _Thread_local char owner[32];
static _Thread_local unsigned sequence;
static _Thread_local uint64_t previous_run_clocks;
static _Thread_local int have_run_clocks;
static _Thread_local uint64_t previous_cpu_sample_us;
static _Thread_local int registry_slot = -1;
/* Only immutable identity is shared. The observer never reads another
 * thread's accounting stack/counters (a blocked owner cannot publish them). */
#define VT_MAX_THREADS 32
_Static_assert(VT_MAX_THREADS == VT_SAMPLE_OWNERS, "owner registries must match");
typedef struct { int tid; char role[32]; } RegisteredThread;
static RegisteredThread registered[VT_MAX_THREADS];
static unsigned registered_count;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int observer_tid = -1;
static void observe_threads(void) {
  static uint64_t previous_us[VT_MAX_THREADS], previous_run[VT_MAX_THREADS];
  static int valid[VT_MAX_THREADS];
  RegisteredThread copy[VT_MAX_THREADS];
  /* Only the emulation observer calls this function. Avoid spending a
   * worker's entire default 32 KiB stack on formatted diagnostics. */
  static char report[16384]; size_t used = 0;
  pthread_mutex_lock(&registry_lock);
  unsigned count = registered_count;
  memcpy(copy, registered, count * sizeof(*copy));
  pthread_mutex_unlock(&registry_lock);
  for (unsigned i = 0; i < count; ++i) {
    SceKernelThreadInfo info = {0}; info.size = sizeof(info);
    int rc = sceKernelGetThreadInfo(copy[i].tid, &info);
    uint64_t now = sceKernelGetProcessTimeWide(), run = 0;
    if (rc >= 0) memcpy(&run, &info.runClocks, sizeof(run));
    int usable = rc >= 0 && valid[i] && run >= previous_run[i];
    append_report(report, sizeof(report), &used, "telemetry_thread thread=%d role=%s sample_us=%llu cpu_window_us=%llu cpu_run_us=%llu cpu_valid=%d status=%u wait_type=%u wait_id=%d priority=%d thread_info_rc=%d scope=registered_threads_only",
      copy[i].tid, copy[i].role, now, usable ? now - previous_us[i] : 0,
      usable ? run - previous_run[i] : 0, usable, info.status,
      info.waitType, info.waitId, info.currentPriority, rc);
    previous_us[i] = now; previous_run[i] = run; valid[i] = rc >= 0;
  }
  if (used && used < sizeof(report)) YuiMsg("%s", report);
  else if (used) YuiMsg("telemetry_report_overflow");
}
static _Thread_local int isolation_probe;
static void *check_isolation(void *unused) {
  (void)unused;
  const int fresh = isolation_probe == 0;
  isolation_probe = 29;
  return (void *)(uintptr_t)fresh;
}
int VitaTelemetryCheckThreadIsolation(void) {
  pthread_t worker;
  isolation_probe = 17;
  if (pthread_create(&worker, NULL, check_isolation, NULL)) return 0;
  void *result = NULL;
  if (pthread_join(worker, &result)) return 0;
  return result == (void *)(uintptr_t)1 && isolation_probe == 17;
}
#define VT_NAME(id, name, kind) name,
static const char *names[] = { VITA_TELEMETRY_PHASES(VT_NAME) };
#undef VT_NAME
#define VT_KIND(id, name, kind) kind,
static const char *kinds[] = { VITA_TELEMETRY_PHASES(VT_KIND) };
#undef VT_KIND

void VitaTelemetryThread(const char *role) {
  if (!VitaTelemetryMode) return;
  if (!initialized) {
    VitaTelemetryAccountingInit(&accounting, sceKernelGetProcessTimeWide());
    initialized = 1;
  }
  snprintf(owner, sizeof(owner), "%s", role);
  pthread_mutex_lock(&registry_lock);
  if (registry_slot < 0 && registered_count < VT_MAX_THREADS)
    registry_slot = (int)registered_count++;
  if (registry_slot >= 0) {
    registered[registry_slot].tid = sceKernelGetThreadId();
    snprintf(registered[registry_slot].role, sizeof(owner), "%s", role);
    if (VitaTelemetryMode == VT_SAMPLED) {
      if (pthread_setspecific(sample_owner_key, (void *)(uintptr_t)(registry_slot + 1))) {
        pthread_mutex_unlock(&registry_lock);
        YuiMsg("telemetry_sample_owner_key_failed");
        __builtin_trap();
      }
      VitaTelemetryOwnerPublish(&sample_owners, registry_slot, registered[registry_slot].tid);
    }
  }
  if (!strcmp(role, "emulation")) observer_tid = sceKernelGetThreadId();
  pthread_mutex_unlock(&registry_lock);
  if (registry_slot < 0) YuiMsg("telemetry_registry_full coverage=partial");
}
static void ensure_owner(void) {
  if (!initialized) VitaTelemetryThread("unregistered");
}
static VitaTelemetrySampler *owner_samples(void) {
  int tid = sceKernelGetThreadId();
  int slot = VitaTelemetryOwnerFind(&sample_owners, tid);
  if (slot < 0) {
    ensure_owner();
    // Startup logging can register this thread in overview mode before the
    // run request selects sampled mode. Bind that existing slot lazily too.
    if (registry_slot >= 0) {
      if (pthread_setspecific(sample_owner_key, (void *)(uintptr_t)(registry_slot + 1))) {
        YuiMsg("telemetry_sample_owner_key_failed");
        __builtin_trap();
      }
      VitaTelemetryOwnerPublish(&sample_owners, registry_slot, tid);
    }
    slot = VitaTelemetryOwnerFind(&sample_owners, tid);
    if (slot < 0) {
      YuiMsg("telemetry_registry_full coverage=partial");
      __builtin_trap(); // never silently share or drop an owner's counters
    }
  }
  return sample_owners.phase[slot];
}
void *VitaTelemetryEnter(VitaTelemetryPhase phase) {
  if (!VitaTelemetryPhaseEnabled(VitaTelemetryMode, phase)) {
    if (VitaTelemetryMode == VT_SAMPLED && (unsigned)phase < VT_PHASE_COUNT) {
      // No emulated TLS in the established-owner hot path. Exit retains the
      // owner-local counter cookie; publication never shares mutable counters.
      VitaTelemetrySampler *s = &owner_samples()[phase];
      if (VitaTelemetrySampleEnter(s, 0x9e3779b9u ^ ((unsigned)phase + 1)))
        s->start = sceKernelGetProcessTimeWide();
      return s;
    }
    return NULL;
  }
  ensure_owner();
  VitaTelemetryAccountEnter(&accounting, phase, sceKernelGetProcessTimeWide());
  return NULL;
}
void VitaTelemetryLeaveSample(void *sample) {
  VitaTelemetrySampler *s = sample;
  if (VitaTelemetrySampleLeave(s))
    VitaTelemetrySampleComplete(s, sceKernelGetProcessTimeWide());
}
void VitaTelemetryLeave(VitaTelemetryPhase phase) {
  if (!VitaTelemetryPhaseEnabled(VitaTelemetryMode, phase)) {
    if (VitaTelemetryMode == VT_SAMPLED && (unsigned)phase < VT_PHASE_COUNT) {
      VitaTelemetryLeaveSample(&owner_samples()[phase]);
    }
    return;
  }
  ensure_owner();
  const uint64_t now = sceKernelGetProcessTimeWide();
  VitaTelemetryAccountLeave(&accounting, phase, now);
}
void VitaTelemetryReport(void) {
  if (!VitaTelemetryMode) return;
  ensure_owner();
  if (reporting) return;
  const uint64_t now = sceKernelGetProcessTimeWide();
  if (now - accounting.window_start < 2000000) return;
  reporting = 1;
  // Reporting remains a live child scope in the NEXT interval, not hidden
  // time charged to whichever subsystem happened to trigger this report.
  VitaTelemetryAccountEnter(&accounting, VT_TELEMETRY, now);
  VitaTelemetryAccounting snapshot;
  VitaTelemetryAccountSnapshot(&accounting, now, &snapshot);
  const int tid = sceKernelGetThreadId();
  SceKernelThreadInfo info = {0};
  info.size = sizeof(info);
  const int rc = sceKernelGetThreadInfo(tid, &info);
  const uint64_t cpu_sample_us = sceKernelGetProcessTimeWide();
  uint64_t clocks = 0;
  if (rc >= 0) memcpy(&clocks, &info.runClocks, sizeof(clocks));
  const int valid_delta = rc >= 0 && have_run_clocks && clocks >= previous_run_clocks;
  const uint64_t delta = valid_delta ? clocks - previous_run_clocks : 0;
  const uint64_t cpu_window_us = valid_delta ? cpu_sample_us - previous_cpu_sample_us : 0;
  previous_cpu_sample_us = cpu_sample_us;
  previous_run_clocks = clocks;
  have_run_clocks = rc >= 0;
  uint64_t sum = 0;
  for (unsigned i = 0; i < VT_PHASE_COUNT; ++i) sum += snapshot.exclusive_us[i];
  ++sequence;
  static _Thread_local char report[16384]; size_t used = 0;
  /* Sony Kernel Reference: runClocks is execution time in microseconds,
   * not hardware cycles. Its sample interval is separate from wall scopes. */
  append_report(report, sizeof(report), &used, "telemetry_window thread=%d role=%s seq=%u begin_us=%llu end_us=%llu wall_us=%llu accounted_us=%llu errors=%llu coverage=partial gpu_execution=unmeasured cpu_run_us=%llu cpu_valid=%d cpu_sample_end_us=%llu cpu_window_us=%llu thread_info_rc=%d mode=%s omitted_children=in_parent",
    tid, owner, sequence, snapshot.window_start, snapshot.last,
    snapshot.last - snapshot.window_start, sum, snapshot.errors, delta, valid_delta,
    cpu_sample_us, cpu_window_us, rc, VitaTelemetryMode == VT_DETAILED ? "detailed" :
      VitaTelemetryMode == VT_SAMPLED ? "sampled" : "overview");
  for (unsigned i = 0; i < VT_PHASE_COUNT; ++i)
    if (snapshot.exclusive_us[i] || snapshot.calls[i])
      append_report(report, sizeof(report), &used, "telemetry_phase thread=%d seq=%u name=%s kind=%s exclusive_wall_us=%llu entries=%llu",
        tid, sequence, names[i], kinds[i], snapshot.exclusive_us[i], snapshot.calls[i]);
  if (VitaTelemetryMode == VT_SAMPLED) {
    VitaTelemetrySampler *samples = owner_samples();
    for (unsigned i = 0; i < VT_PHASE_COUNT; ++i) {
      VitaTelemetrySampler *s = &samples[i];
      if (s->entries || s->samples || s->depth || s->errors)
        append_report(report, sizeof(report), &used,
          "telemetry_sample thread=%d seq=%u name=%s entries=%llu roots=%llu samples=%llu inclusive_wall_us=%llu max_wall_us=%llu errors=%llu open_depth=%u probability_denominator=1024 attribution=completion_window additive=0",
          tid, sequence, names[i], s->entries, s->roots, s->samples,
          s->inclusive_us, s->max_us, s->errors, s->depth);
      VitaTelemetrySampleResetWindow(s);
    }
  }
  append_report(report, sizeof(report), &used, "telemetry_end thread=%d seq=%u", tid, sequence);
  if (used < sizeof(report)) YuiMsg("%s", report);
  else YuiMsg("telemetry_report_overflow");
  if (tid == observer_tid) observe_threads();
  VitaTelemetryAccountLeave(&accounting, VT_TELEMETRY, sceKernelGetProcessTimeWide());
  reporting = 0;
}
