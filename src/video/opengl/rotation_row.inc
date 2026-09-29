/* SPDX-License-Identifier: GPL-2.0-or-later
 * VDP2 manual 6.3/6.4: fixed-parameter modes with no horizontal coefficient
 * increment can resolve once per row. The window-switched mode (RPMD 3) can
 * too on rows its window covers entirely or not at all.
 * A successful resolution may be NULL (the entire line is transparent).
 */
static int Vdp2RotationResolveRow(vdp2draw_struct *info, int mode, int y, int hres,
                                 vdp2rotationparameter_struct **result)
{
  vdp2rotationparameter_struct *p;
  if (mode == 0 || mode == 1) {
    p = mode == 0 ? &paraA : &paraB;
    if (p->coefenab && p->deltaKAx != 0.0f) return 0;
    *result = p->coefenab && !vdp2rGetKValue(p, 0.0f) ? NULL : p;
    return 1;
  }
  /* Preserve RBG1's callback and its existing CRAM addressing contract. */
  if (mode >= 4 && mode <= 7 &&
      (info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode01NoK ||
       (info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode01WithK &&
        paraB.deltaKAx == 0.0f))) {
    *result = info->GetRParam(info, 0, y);
    return 1;
  }
  /* The mode 3 callbacks depend on h only through the window comparison and
   * deltaKAx * h. Mode03NoK indexes the window with the unshifted line. */
  if (mode == 3 && paraA.deltaKAx == 0.0f && paraB.deltaKAx == 0.0f &&
      (info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode03NoK ||
       info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode03WithKA ||
       info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode03WithKB ||
       info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode03WithK)) {
    if (fixVdp2Regs->WCTLD & 0xA) {
      if (!info->pWinInfo) return 0;
      const int line = info->GetRParam == (Vdp2GetRParam_func)vdp2RGetParamMode03NoK ?
        y : y << info->hres_shift;
      const vdp2WindowInfo *w = &info->pWinInfo[line];
      const int inside_all = w->WinHStart <= 0 && w->WinHEnd >= hres;
      const int outside_all = w->WinHEnd <= w->WinHStart || w->WinHStart >= hres || w->WinHEnd <= 0;
      if (w->WinShowLine && !inside_all && !outside_all) return 0;
    }
    *result = info->GetRParam(info, 0, y);
    return 1;
  }
  return 0;
}
