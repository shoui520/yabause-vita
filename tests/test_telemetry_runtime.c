/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/telemetry.h"
#include "../src/vita/telemetry_policy.h"
#include <psp2/kernel/threadmgr.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static _Atomic uint64_t clock_us;
static _Atomic int next_tid = 1, reports, sample_reports, failures;
static _Thread_local int tid;
uint64_t sceKernelGetProcessTimeWide(void) { return atomic_fetch_add(&clock_us, 10000); }
int sceKernelGetThreadId(void) {
  if (!tid) tid = atomic_fetch_add(&next_tid, 1);
  return tid;
}
int sceKernelGetThreadInfo(int id, SceKernelThreadInfo *info) {
  assert(id > 0);
  info->runClocks = atomic_load(&clock_us);
  info->currentPriority = 191;
  return 0;
}
void YuiMsg(const char *format, ...) {
  char text[32768];
  va_list ap; va_start(ap, format);
  vsnprintf(text, sizeof(text), format, ap); va_end(ap);
  if (strstr(text, "telemetry_window")) ++reports;
  if (strstr(text, "telemetry_sample thread=")) ++sample_reports;
  if (strstr(text, "failed") || strstr(text, "registry_full") || strstr(text, "overflow")) ++failures;
  if (strstr(text, "telemetry_thread ")) assert(strstr(text, "priority=191"));
  for (const char *p = text; (p = strstr(p, "errors=")); ++p)
    if (strtoull(p + 7, NULL, 10)) ++failures;
}
static void exercise(void) {
  for (unsigned i = 0; i < 10000; ++i) {
    VT_SCOPE(VT_SCU);
    { VT_SCOPE(VT_SCU_DSP); }
  }
  atomic_fetch_add(&clock_us, 3000000);
  VitaTelemetryReport();
}
static void *worker(void *unused) {
  (void)unused;
  VitaTelemetryThread("test_worker");
  exercise();
  return NULL;
}
int main(void) {
  assert(VitaTelemetryCheckThreadIsolation());
  // This is the real startup ordering that previously reached the trap:
  // coarse logging initializes owner state before reading the run mode.
  VitaTelemetryEnter(VT_LOGGING); VitaTelemetryLeave(VT_LOGGING);
  VitaTelemetryConfigure(VT_SAMPLED);
  exercise();
  VitaTelemetryThread("emulation");
  pthread_t child;
  assert(!pthread_create(&child, NULL, worker, NULL));
  exercise();
  assert(!pthread_join(child, NULL));
  assert(reports >= 3 && sample_reports >= 3 && failures == 0);
  puts("telemetry runtime: startup mode transition and concurrent owner reports passed");
}
