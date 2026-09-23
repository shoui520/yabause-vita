/* SPDX-License-Identifier: GPL-2.0-or-later
 * VDP2 manual 6.3/6.4: fixed-parameter modes with no horizontal coefficient
 * increment can resolve once per row. Window/parameter-switch modes cannot.
 * A successful resolution may be NULL (the entire line is transparent).
 */
static int Vdp2RotationResolveRow(vdp2draw_struct *info, int mode, int y,
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
  return 0;
}
