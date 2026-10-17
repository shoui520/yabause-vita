/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_CPU_AFFINITY_H
#define VITA_CPU_AFFINITY_H
#include "../core/threads.h"
// Fixed placement for emulation workers. Other service threads keep their
// requested policy; pinning does not introduce new asynchronous execution.
// Core 0 takes most of the kernel's interrupts (a thread there loses about
// 3% of its time to them), so the emulation thread, which bounds the frame
// rate, runs on core 1 and the sound side on core 0. Logical masks.
enum { VITA_CORE_EMULATION = 2, VITA_CORE_SOUND = 1 };
static inline int VitaAffinityForRole(int role, int requested) {
  if (role == YAB_THREAD_SCSP || role == YAB_THREAD_OPENAL) return VITA_CORE_SOUND;
#ifdef VITA_ROTATION_SPLIT
  /* Rows are shared with the render thread (core 2) while it waits; the
   * worker preempts the spinning sound thread on its core instead. */
  if (role == YAB_THREAD_VIDSOFT_LAYER_RBG0) return VITA_CORE_SOUND;
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
