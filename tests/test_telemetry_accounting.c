/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/telemetry.h"
#include "../src/vita/telemetry_policy.h"
#include "../src/vita/telemetry_sampling.h"
#include <assert.h>
#include <stdio.h>

static VitaTelemetryAccounting scope_accounting;
int VitaTelemetryMode = VT_DETAILED;
static uint64_t scope_clock;
static int sample_scopes;
static VitaTelemetrySampler scope_samples[VT_PHASE_COUNT];
void *VitaTelemetryEnter(VitaTelemetryPhase phase) {
  if (sample_scopes) {
    VitaTelemetrySampler *s = &scope_samples[phase];
    if (VitaTelemetrySampleEnter(s, (unsigned)phase + 1)) s->start = scope_clock;
    return s;
  }
  assert(VitaTelemetryAccountEnter(&scope_accounting, phase, scope_clock += 10));
  return NULL;
}
void VitaTelemetryLeaveSample(void *sample) {
  VitaTelemetrySampler *s = (VitaTelemetrySampler *)sample;
  if (VitaTelemetrySampleLeave(s)) VitaTelemetrySampleComplete(s, scope_clock + 5);
}
void VitaTelemetryLeave(VitaTelemetryPhase phase) {
  assert(!sample_scopes); // cookie must bypass lookup on the sampled path
  assert(VitaTelemetryAccountLeave(&scope_accounting, phase, scope_clock += 10));
}
static int early_return(int branch) {
  VT_SCOPE(VT_SCU);
  if (branch) { VT_SCOPE(VT_SCU_DSP); return 7; }
  return 9;
}

static void partition(const VitaTelemetryAccounting *a) {
  uint64_t total = 0;
  for (unsigned i = 0; i < VT_PHASE_COUNT; ++i) total += a->exclusive_us[i];
  assert(total == a->last - a->window_start);
}
int main(void) {
  for (unsigned i = 0; i < VT_PHASE_COUNT; ++i) {
    assert(!VitaTelemetryPhaseEnabled(VT_OFF, (VitaTelemetryPhase)i));
    assert(VitaTelemetryPhaseEnabled(VT_DETAILED, (VitaTelemetryPhase)i));
    assert(VitaTelemetryPhaseEnabled(VT_SAMPLED, (VitaTelemetryPhase)i) ==
           VitaTelemetryPhaseEnabled(VT_OVERVIEW, (VitaTelemetryPhase)i));
  }
  assert(!VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_SH2_MASTER));
  assert(VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_SOUND_SYNC));
  assert(VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_QUEUE_WAIT));
  assert(VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_GPU_WAIT));
  assert(VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_ATLAS_PUSH));
  assert(VitaTelemetryPhaseEnabled(VT_OVERVIEW, VT_ROTATION_UPLOAD));
  // The actual call nesting is atlas push -> rotation submission -> upload.
  // Separate names must partition the same interval, not double-count it.
  VitaTelemetryAccounting graphics, snapshot;
  VitaTelemetryAccountingInit(&graphics, 0);
  assert(VitaTelemetryAccountEnter(&graphics, VT_ATLAS_PUSH, 10));
  assert(VitaTelemetryAccountEnter(&graphics, VT_GPU_SUBMIT, 30));
  assert(VitaTelemetryAccountEnter(&graphics, VT_ROTATION_UPLOAD, 40));
  assert(VitaTelemetryAccountLeave(&graphics, VT_ROTATION_UPLOAD, 70));
  assert(VitaTelemetryAccountLeave(&graphics, VT_GPU_SUBMIT, 90));
  assert(VitaTelemetryAccountLeave(&graphics, VT_ATLAS_PUSH, 100));
  assert(VitaTelemetryAccountSnapshot(&graphics, 110, &snapshot));
  partition(&snapshot);
  assert(snapshot.exclusive_us[VT_ATLAS_PUSH] == 30);
  assert(snapshot.exclusive_us[VT_GPU_SUBMIT] == 30);
  assert(snapshot.exclusive_us[VT_ROTATION_UPLOAD] == 30);
  assert(snapshot.exclusive_us[VT_UNATTRIBUTED] == 20);
  VitaTelemetryAccountingInit(&scope_accounting, 0);
  assert(early_return(1) == 7 && early_return(0) == 9);
  assert(scope_accounting.depth == 1 && scope_accounting.errors == 0);
  assert(scope_accounting.exclusive_us[VT_SCU_DSP] == 10);
  assert(scope_accounting.exclusive_us[VT_SCU] == 30);
  sample_scopes = 1;
  assert(early_return(1) == 7 && early_return(0) == 9);
  assert(scope_samples[VT_SCU].entries == 2 && scope_samples[VT_SCU].depth == 0);
  assert(scope_samples[VT_SCU_DSP].entries == 1 && scope_samples[VT_SCU_DSP].depth == 0);
  sample_scopes = 0;
  VitaTelemetryAccounting main, sound, s;
  VitaTelemetryAccountingInit(&main, 100);
  VitaTelemetryAccountingInit(&sound, 100);
  assert(VitaTelemetryAccountEnter(&main, VT_SCHEDULER, 110));
  assert(VitaTelemetryAccountEnter(&main, VT_SH2_MASTER, 120));
  assert(VitaTelemetryAccountEnter(&main, VT_SH2_MEMORY, 130));
  assert(VitaTelemetryAccountLeave(&main, VT_SH2_MEMORY, 140));
  assert(VitaTelemetryAccountLeave(&main, VT_SH2_MASTER, 150));
  assert(VitaTelemetryAccountEnter(&main, VT_QUEUE_WAIT, 150));
  assert(VitaTelemetryAccountEnter(&sound, VT_SCSP_MIX, 125));
  assert(VitaTelemetryAccountSnapshot(&main, 175, &s));
  partition(&s);
  assert(s.exclusive_us[VT_SH2_MASTER] == 20);
  assert(s.exclusive_us[VT_SH2_MEMORY] == 10);
  assert(s.exclusive_us[VT_QUEUE_WAIT] == 25);
  assert(s.exclusive_us[VT_UNATTRIBUTED] == 10);
  assert(VitaTelemetryAccountLeave(&main, VT_QUEUE_WAIT, 190));
  assert(VitaTelemetryAccountLeave(&main, VT_SCHEDULER, 200));
  assert(VitaTelemetryAccountSnapshot(&main, 210, &s));
  partition(&s);
  assert(s.exclusive_us[VT_QUEUE_WAIT] == 15 && s.calls[VT_QUEUE_WAIT] == 0);
  assert(VitaTelemetryAccountLeave(&sound, VT_SCSP_MIX, 190));
  assert(VitaTelemetryAccountSnapshot(&sound, 210, &s));
  partition(&s);
  assert(s.exclusive_us[VT_SCSP_MIX] == 65);
  assert(!VitaTelemetryAccountLeave(&main, VT_SCHEDULER, 220));
  assert(!VitaTelemetryAccountEnter(&main, VT_INPUT, 209));
  for (unsigned i = 1; i < VT_DEPTH; ++i)
    assert(VitaTelemetryAccountEnter(&main, VT_SCHEDULER, 220));
  assert(!VitaTelemetryAccountEnter(&main, VT_SCHEDULER, 220));
  assert(VitaTelemetryAccountSnapshot(&main, 230, &s));
  assert(s.errors == 3);
  partition(&s);
  puts("telemetry accounting: nested exclusive time, overlapping owners, open-scope snapshots, invalid events passed");
}
