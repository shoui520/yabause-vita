/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VITA_DIAG_TIMERS (diagnostics only): wall-clock microseconds and call
 * counts for a few coarse functions, reported once per telemetry window.
 * Each measurement costs two sceKernelGetProcessTimeWide calls (~1.3 us). */
#pragma once
#ifdef VITA_DIAG_TIMERS
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DT_SPRITE, DT_BITMAP, DT_ATLAS, DT_VDP1, DT_SH2M, DT_SH2S, DT_HBLANK, DT_SCU, DT_SMPC_CD,
       DT_VDP2_DRAW, DT_GPU, DT_SCU_DMA, DT_SCU_DSP, DT_COUNT };
extern uint64_t vita_diag_dsp_insns;
extern uint64_t vita_diag_us[DT_COUNT], vita_diag_calls[DT_COUNT];
extern uint64_t vita_diag_sprite_us[8], vita_diag_sprite_texels[8], vita_diag_sprite_calls[8];
uint64_t sceKernelGetProcessTimeWide(void);
void VitaDiagTimersReport(void);
#ifdef __cplusplus
}
#endif
#define DIAG_T0(v) uint64_t v = sceKernelGetProcessTimeWide()
#define DIAG_T1(v, slot) (vita_diag_us[slot] += sceKernelGetProcessTimeWide() - (v), ++vita_diag_calls[slot])
#else
#define DIAG_T0(v) ((void)0)
#define DIAG_T1(v, slot) ((void)0)
#endif
