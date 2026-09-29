/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
typedef struct { int coefenab; float deltaKAx; int valid, lineaddr; } vdp2rotationparameter_struct;
typedef struct { int WinShowLine, WinHStart, WinHEnd; } vdp2WindowInfo;
typedef struct vdp2draw_struct vdp2draw_struct;
typedef vdp2rotationparameter_struct *(*Vdp2GetRParam_func)(vdp2draw_struct *, int, int);
struct vdp2draw_struct { Vdp2GetRParam_func GetRParam; vdp2WindowInfo *pWinInfo; int hres_shift; };
static vdp2rotationparameter_struct paraA, paraB;
static struct { int WCTLD; } regs, *fixVdp2Regs = &regs;
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
/* Mode 3: inside the window is A, outside B; the NoK variant indexes the
 * window with the unshifted line (as the emulator's callback does). */
static vdp2rotationparameter_struct *mode3(vdp2draw_struct *i, int x, int line) {
  ++calls;
  if (!(fixVdp2Regs->WCTLD & 0xA)) return &paraA;
  const vdp2WindowInfo *w = &i->pWinInfo[line];
  return w->WinShowLine && !(x < w->WinHStart || x >= w->WinHEnd) ? &paraA : &paraB;
}
static vdp2rotationparameter_struct *vdp2RGetParamMode03NoK(vdp2draw_struct *i, int x, int y) {
  return mode3(i, x, y);
}
static vdp2rotationparameter_struct *vdp2RGetParamMode03WithK(vdp2draw_struct *i, int x, int y) {
  return mode3(i, x, y << i->hres_shift);
}
#define vdp2RGetParamMode03WithKA vdp2RGetParamMode03WithK
#define vdp2RGetParamMode03WithKB vdp2RGetParamMode03WithK
#include "../src/video/opengl/rotation_row.inc"
int main(void) {
  const float deltas[] = {0.0f, -0.0f, 0.25f, -0.25f, NAN};
  for (int mode=0; mode<8; ++mode)
    for (int enabled=0; enabled<2; ++enabled)
      for (int valid=0; valid<2; ++valid)
        for (unsigned d=0; d<sizeof(deltas)/sizeof(*deltas); ++d) {
          paraA = paraB = (vdp2rotationparameter_struct){enabled,deltas[d],valid,0};
          vdp2draw_struct info = {enabled ? vdp2RGetParamMode01WithK : vdp2RGetParamMode01NoK, NULL, 0};
          vdp2rotationparameter_struct *result = NULL;
          calls = 0;
          int resolved = Vdp2RotationResolveRow(&info, mode, 19, 352, &result);
          int expected = mode != 2 && mode != 3 && (!enabled || deltas[d] == 0.0f);
          assert(resolved == expected);
          if (!resolved) { assert(calls == 0); continue; }
          assert(result == (enabled && !valid ? NULL : mode == 0 ? &paraA : &paraB));
          assert(calls == (unsigned)(enabled || mode >= 4));
          if (enabled) assert((mode == 0 ? paraA.lineaddr : paraB.lineaddr) == 37);
        }
  vdp2draw_struct info = {window, NULL, 0};
  vdp2rotationparameter_struct *result = NULL;
  calls = 0;
  for (int mode=2; mode<8; ++mode)
    assert(!Vdp2RotationResolveRow(&info,mode,0,352,&result));
  assert(calls == 0);

  /* Mode 3 resolves a row only when every pixel takes the same side. */
  static vdp2WindowInfo win[64];
  const struct { int show, start, end, resolved; } rows[] = {
    {0, 10, 20, 1}, {1, 0, 352, 1}, {1, -5, 400, 1}, {1, 0, 351, 0}, {1, 1, 352, 0},
    {1, 352, 400, 1}, {1, -9, 0, 1}, {1, 30, 30, 1}, {1, 40, 20, 1}, {1, 100, 200, 0}};
  for (int fn = 0; fn < 2; ++fn)
    for (int shift = 0; shift < 2; ++shift)
      for (unsigned r = 0; r < sizeof(rows) / sizeof(*rows); ++r)
        for (int wctld = 0; wctld < 2; ++wctld) {
          paraA = paraB = (vdp2rotationparameter_struct){0, 0.0f, 1, 0};
          regs.WCTLD = wctld ? 0x2 : 0;
          for (unsigned k = 0; k < 64; ++k) win[k] = (vdp2WindowInfo){1, 100, 200};
          const int y = 5, line = fn == 0 ? y : y << shift;
          win[line] = (vdp2WindowInfo){rows[r].show, rows[r].start, rows[r].end};
          vdp2draw_struct m3 = {fn == 0 ? vdp2RGetParamMode03NoK : vdp2RGetParamMode03WithK, win, shift};
          result = NULL;
          int resolved = Vdp2RotationResolveRow(&m3, 3, y, 352, &result);
          assert(resolved == (!wctld || rows[r].resolved));
          if (!resolved) continue;
          for (int x = 0; x < 352; ++x) assert(m3.GetRParam(&m3, x, y) == result);
          paraA.deltaKAx = 0.5f;
          assert(!Vdp2RotationResolveRow(&m3, 3, y, 352, &result));
        }
  puts("rotation row selection: fixed/varying coefficients, transparency, RBG1, window fallback and mode 3 window rows passed");
}
