/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_TELEMETRY_POLICY_H
#define VITA_TELEMETRY_POLICY_H
#include "telemetry_accounting.h"
enum { VT_OFF = 0, VT_OVERVIEW = 1, VT_DETAILED = 2, VT_SAMPLED = 3 };
/* Overview retains whole-thread CPU samples, coarse scopes and blocking waits.
 * Omitted children remain charged to their enclosing scope; they are NOT zero
 * cost. Detailed mode is required for the hot subsystem breakdown. This is
 * deterministic filtering, not sampling or a statistical CPU profiler.
 * VT_SAMPLED uses this same exclusive partition and adds independent raw
 * inclusive call samples for the excluded phases in telemetry.c. */
static inline int VitaTelemetryPhaseEnabled(int mode, VitaTelemetryPhase phase) {
  if (mode == VT_OFF) return 0;
  if (mode == VT_DETAILED) return 1;
  switch (phase) {
    case VT_SH2_MASTER: case VT_SH2_SLAVE: case VT_SH2_ONCHIP:
    case VT_SH2_DMA: case VT_SH2_MEMORY: case VT_SH2_DISPATCH:
    case VT_SH2_INVALIDATE: case VT_SCU: case VT_SCU_DMA: case VT_SCU_DSP:
    case VT_SMPC: case VT_CD_CONTROL: case VT_HBLANK:
    case VT_M68K: case VT_SCSP_MIX: case VT_SCSP_DSP:
    case VT_MUTEX_WAIT: case VT_TEXTURE_DECODE:
      return 0;
    default: return 1;
  }
}
#endif
