/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/telemetry.h"
#include <assert.h>
#include <stdio.h>
int VitaTelemetryMode;
static unsigned enters, leaves, samples;
static int cookie;
void *VitaTelemetryEnter(VitaTelemetryPhase p) {
  ++enters;
  return VitaTelemetryMode == VT_SAMPLED && !VitaTelemetryPhaseEnabled(VT_OVERVIEW, p)
    ? &cookie : NULL;
}
void VitaTelemetryLeave(VitaTelemetryPhase p) { (void)p; ++leaves; }
void VitaTelemetryLeaveSample(void *s) { assert(s == &cookie); ++samples; }
static void early_exit(VitaTelemetryPhase phase) { VT_SCOPE(phase); return; }
int main(void) {
  unsigned cases = 0;
  for (int mode = VT_OFF; mode <= VT_SAMPLED; ++mode)
    for (unsigned p = 0; p < VT_PHASE_COUNT; ++p) {
      VitaTelemetryMode = mode;
      VitaTelemetryPhase phase = (VitaTelemetryPhase)p;
      enters = leaves = samples = 0;
      { VT_SCOPE(phase); early_exit(phase); }
      const unsigned active = mode == VT_SAMPLED || VitaTelemetryPhaseEnabled(mode, phase);
      const unsigned sampled = mode == VT_SAMPLED && !VitaTelemetryPhaseEnabled(VT_OVERVIEW, phase);
      assert(enters == 2 * active);
      assert(samples == 2 * sampled);
      assert(leaves == 2 * (active && !sampled));
      ++cases;
    }
  printf("telemetry scope gate: %u mode/phase cases, nesting and early exit passed\n", cases);
}
