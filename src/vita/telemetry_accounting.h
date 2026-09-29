/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_TELEMETRY_ACCOUNTING_H
#define VITA_TELEMETRY_ACCOUNTING_H
#include <stdint.h>
#include <string.h>

/* Exclusive wall-time, NOT CPU execution time. One owner per instance. Nested
 * children subtract from the parent's time by construction. Different threads
 * overlap and must never be summed into a supposed frame critical path. */
#define VITA_TELEMETRY_PHASES(X) \
 X(UNATTRIBUTED, "unattributed", "unknown") \
 X(FRONTEND, "frontend", "work") \
 X(INPUT, "input", "work") \
 X(SCHEDULER, "scheduler", "work") \
 X(SH2_MASTER, "sh2_master", "work") \
 X(SH2_SLAVE, "sh2_slave", "work") \
 X(SH2_COMPILE, "sh2_compile", "work") \
 X(SH2_DISPATCH, "sh2_dispatch", "work") \
 X(SH2_ONCHIP, "sh2_onchip", "work") \
 X(SH2_DMA, "sh2_dma", "work") \
 X(SH2_MEMORY, "sh2_memory", "work") \
 X(SH2_INVALIDATE, "sh2_invalidate", "work") \
 X(SH2_NATIVE, "sh2_native", "work") \
 X(SH2_FORWARD, "sh2_forward", "work") \
 X(SH2_SLICE_TAIL, "sh2_slice_tail", "work") \
 X(VDP2_CELL_LOOKUP, "vdp2_cell_lookup", "work") \
 X(VDP2_CELL_QUAD, "vdp2_cell_quad", "work") \
 X(VDP2_CELL_DECODE, "vdp2_cell_decode", "work") \
 X(VDP2_CELL_ADDR, "vdp2_cell_addr", "work") \
 X(M68K, "m68k", "work") \
 X(M68K_ORBIT, "m68k_orbit", "work") \
 X(SCSP_TIMER, "scsp_timer", "work") \
 X(SCU, "scu_control", "work") \
 X(SCU_DMA, "scu_dma", "work") \
 X(SCU_DSP, "scu_dsp", "work") \
 X(SMPC, "smpc", "work") \
 X(CD_CONTROL, "cd_control", "work") \
 X(DISC_READ, "disc_sector", "work") \
 X(DISC_IO, "disc_io", "io") \
 X(DISC_DECODE, "disc_decode", "work") \
 X(CDDA, "cdda", "work") \
 X(SCSP, "scsp_control", "work") \
 X(SOUND_SYNC, "sound_sync_spin", "spin") \
 X(SOUND_BUDGET_WAIT, "sound_budget_wait", "wait") \
 X(SCSP_MIX, "scsp_mix", "work") \
 X(SCSP_DSP, "scsp_dsp", "work") \
 X(AUDIO_CONVERT, "audio_convert", "work") \
 X(AUDIO_OUTPUT, "audio_output", "wait") \
 X(HBLANK, "hblank", "work") \
 X(VBLANK, "vblank", "work") \
 X(VDP1, "vdp1", "work") \
 X(VDP1_RASTER, "vdp1_raster", "work") \
 X(VDP2, "vdp2", "work") \
 X(VDP2_NORMAL, "vdp2_normal", "work") \
 X(VDP2_BITMAP, "vdp2_bitmap", "work") \
 X(VDP2_BITMAP_LS, "vdp2_bitmap_linescroll", "work") \
 X(VDP2_BITMAP_CI, "vdp2_bitmap_coordinc", "work") \
 X(VDP2_MAP_LINE, "vdp2_map_perline", "work") \
 X(VDP2_MAP, "vdp2_map", "work") \
 X(VDP2_ROTATION, "vdp2_rotation", "work") \
 X(TEXTURE_DECODE, "texture_decode", "work") \
 X(TEXTURE_UPLOAD, "texture_upload", "work") \
 X(ATLAS_PUSH, "atlas_push_cpu", "work") \
 X(ROTATION_UPLOAD, "rotation_upload", "work") \
 X(RENDER_ALLOC, "render_alloc", "work") \
 X(GPU_SUBMIT, "gpu_submit_cpu", "work") \
 X(GPU_WAIT, "gpu_wait", "wait") \
 X(GPU_NOTIFICATION_WAIT, "gpu_notification_wait", "wait") \
 X(DISPLAY_QUEUE_WAIT, "display_queue_wait", "wait") \
 X(PRESENT, "present", "work") \
 X(VBLANK_WAIT, "vblank_wait", "wait") \
 X(QUEUE_WAIT, "queue_wait", "wait") \
 X(MUTEX_WAIT, "mutex_wait", "wait") \
 X(THREAD_JOIN, "thread_join", "wait") \
 X(THREAD_SLEEP, "thread_sleep", "wait") \
 X(CAPTURE, "capture", "diagnostic") \
 X(LOGGING, "logging", "diagnostic") \
 X(TELEMETRY, "telemetry", "diagnostic")

#define VT_ENUM(id, name, kind) VT_##id,
typedef enum { VITA_TELEMETRY_PHASES(VT_ENUM) VT_PHASE_COUNT } VitaTelemetryPhase;
#undef VT_ENUM
#define VT_DEPTH 32
typedef struct {
  uint64_t exclusive_us[VT_PHASE_COUNT], calls[VT_PHASE_COUNT];
  uint64_t window_start, last, errors;
  unsigned depth;
  VitaTelemetryPhase stack[VT_DEPTH];
} VitaTelemetryAccounting;

static inline void VitaTelemetryAccountingInit(VitaTelemetryAccounting *a, uint64_t now) {
  memset(a, 0, sizeof(*a));
  a->window_start = a->last = now;
  a->depth = 1;
  a->stack[0] = VT_UNATTRIBUTED;
}
static inline int VitaTelemetryAccountUntil(VitaTelemetryAccounting *a, uint64_t now) {
  if (now < a->last || !a->depth) { ++a->errors; return 0; }
  a->exclusive_us[a->stack[a->depth - 1]] += now - a->last;
  a->last = now;
  return 1;
}
static inline int VitaTelemetryAccountEnter(VitaTelemetryAccounting *a,
                                           VitaTelemetryPhase phase, uint64_t now) {
  if ((unsigned)phase >= VT_PHASE_COUNT || a->depth == VT_DEPTH) { ++a->errors; return 0; }
  if (!VitaTelemetryAccountUntil(a, now)) return 0;
  a->stack[a->depth++] = phase;
  ++a->calls[phase];
  return 1;
}
static inline int VitaTelemetryAccountLeave(VitaTelemetryAccounting *a,
                                           VitaTelemetryPhase phase, uint64_t now) {
  if (a->depth <= 1 || a->stack[a->depth - 1] != phase) { ++a->errors; return 0; }
  if (!VitaTelemetryAccountUntil(a, now)) return 0;
  --a->depth;
  return 1;
}
/* Owner-only snapshot. Open scopes continue across windows; durations are split
 * at the boundary, calls count entries in this window rather than completions. */
static inline int VitaTelemetryAccountSnapshot(VitaTelemetryAccounting *a,
                                               uint64_t now, VitaTelemetryAccounting *out) {
  if (!VitaTelemetryAccountUntil(a, now)) return 0;
  *out = *a;
  memset(a->exclusive_us, 0, sizeof(a->exclusive_us));
  memset(a->calls, 0, sizeof(a->calls));
  a->window_start = now;
  a->errors = 0;
  return 1;
}
#endif
