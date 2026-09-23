/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
typedef struct { int coefenab; float deltaKAx; int valid, lineaddr; } vdp2rotationparameter_struct;
typedef struct vdp2draw_struct vdp2draw_struct;
typedef vdp2rotationparameter_struct *(*Vdp2GetRParam_func)(vdp2draw_struct *, int, int);
struct vdp2draw_struct { Vdp2GetRParam_func GetRParam; };
static vdp2rotationparameter_struct paraA, paraB;
static unsigned calls;
static int vdp2rGetKValue(vdp2rotationparameter_struct *p, float x) {
  assert(x == 0.0f); ++calls; p->lineaddr = 37; return p->valid;
}
static vdp2rotationparameter_struct *vdp2RGetParamMode01NoK(vdp2draw_struct *i, int x, int y) {
  (void)i; (void)x; (void)y; ++calls; return &paraB;
}
static vdp2rotationparameter_struct *vdp2RGetParamMode01WithK(vdp2draw_struct *i, int x, int y) {
  (void)i; (void)y; return vdp2rGetKValue(&paraB, (float)x) ? &paraB : NULL;
}
static vdp2rotationparameter_struct *window(vdp2draw_struct *i, int x, int y) {
  (void)i; (void)y; ++calls; return x < 100 ? &paraA : &paraB;
}
#include "../src/video/opengl/rotation_row.inc"
int main(void) {
  const float deltas[] = {0.0f, -0.0f, 0.25f, -0.25f, NAN};
  for (int mode=0; mode<8; ++mode)
    for (int enabled=0; enabled<2; ++enabled)
      for (int valid=0; valid<2; ++valid)
        for (unsigned d=0; d<sizeof(deltas)/sizeof(*deltas); ++d) {
          paraA = paraB = (vdp2rotationparameter_struct){enabled,deltas[d],valid,0};
          vdp2draw_struct info = {enabled ? vdp2RGetParamMode01WithK : vdp2RGetParamMode01NoK};
          vdp2rotationparameter_struct *result = NULL;
          calls = 0;
          int resolved = Vdp2RotationResolveRow(&info, mode, 19, &result);
          int expected = mode != 2 && mode != 3 && (!enabled || deltas[d] == 0.0f);
          assert(resolved == expected);
          if (!resolved) { assert(calls == 0); continue; }
          assert(result == (enabled && !valid ? NULL : mode == 0 ? &paraA : &paraB));
          assert(calls == (unsigned)(enabled || mode >= 4));
          if (enabled) assert((mode == 0 ? paraA.lineaddr : paraB.lineaddr) == 37);
        }
  vdp2draw_struct info = {window};
  vdp2rotationparameter_struct *result = NULL;
  calls = 0;
  for (int mode=2; mode<8; ++mode)
    assert(!Vdp2RotationResolveRow(&info,mode,0,&result));
  assert(calls == 0);
  puts("rotation row selection: fixed/varying coefficients, transparency, RBG1 and window fallback passed");
}
