/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ROTATION_GPU_H
#define YGL_ROTATION_GPU_H
/* CPU producer methods make no GL calls; the atlas flush owns submission. */
int YglVitaRotationBegin(RBGDrawInfo *, vdp2rotationparameter_struct *, const unsigned char *);
void YglVitaRotationRow(int, int, const vdp2rotationparameter_struct *);
void YglVitaRotationFlush(YglTextureManager *);
void YglVitaRotationReset(void);
void YglVitaRotationShutdown(void);
int YglVitaRotationWritten(void);
int YglVitaRotationUsesAtlas(void);
int YglVitaRotationPrivateRect(unsigned,unsigned,unsigned,unsigned);
#ifdef VITA_ROTATION_TARGET
/* Composition-side request: render the admitted job whose atlas rectangle is
 * exactly (x,y,w,h) into its own w x h texture instead of the atlas. Returns
 * a nonzero source id for YglVitaRotationSource, or 0 (atlas path). */
int YglVitaRotationTarget(unsigned x, unsigned y, unsigned w, unsigned h);
#endif
#endif
