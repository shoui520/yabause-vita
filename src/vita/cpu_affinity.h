/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_CPU_AFFINITY_H
#define VITA_CPU_AFFINITY_H
#include "../core/threads.h"
// Fixed placement for emulation workers. Other service threads keep their
// requested policy; pinning does not introduce new asynchronous execution.
static inline int VitaAffinityForRole(int role, int requested) {
  if (role == YAB_THREAD_SCSP || role == YAB_THREAD_OPENAL) return 2;
#ifdef VITA_ROTATION_SPLIT
  /* Rows are shared with the render thread (core 2) while it waits; the
   * worker preempts the spinning sound thread on core 1 instead. */
  if (role == YAB_THREAD_VIDSOFT_LAYER_RBG0) return 2;
#endif
  if (role == YAB_THREAD_VDP ||
      (role >= YAB_THREAD_VIDSOFT_LAYER_NBG3 && role < YAB_NUM_THREADS)) return 4;
  return requested;
}
// Yabause uses logical bits 0..2. Sony Kernel Reference specifies USER_0..2;
// kernel/cpu.h encodes these at bits 16..18. Zero means system default.
static inline int VitaAffinityEncode(int logical) {
  return logical < 0 || (logical & ~7) ? -1 : logical << 16;
}
static inline int VitaAffinityDecode(int native) {
  if (native < 0) return native; // Preserve the kernel error, not a fake mask.
  return native & ~0x70000 ? -1 : native >> 16;
}
#endif
