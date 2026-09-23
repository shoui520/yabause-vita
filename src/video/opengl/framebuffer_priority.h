/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
/* Match framebuffer_common.cg's inclusive interval test exactly. Use the
 * composition snapshot, not VDP1 draw-time registers. Callers must exclude
 * per-line priorities. NaNs conservatively keep the draw, as the shader does. */
static inline int YglFramebufferPriorityVisible(const float *priority,
                                               float from, float to) {
  for (unsigned i=0;i<8;++i) {
    float depth=priority[i*4];
    if (!(depth<from || depth>to)) return 1;
  }
  return 0;
}
