/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C68K_NATIVE_DISPATCH_H
#define C68K_NATIVE_DISPATCH_H
#include "c68k.h"
#ifdef __cplusplus
extern "C" {
#endif
// Experimental executor integration point. Zero rejects with CPU unchanged.
// Success consumes <= remaining, materializes registers/flags, advances CPU->PC
// from the supplied PRIVATE executor PC, and preserves the last instruction's
// CycleIO publication. IRQ/state exclusion, source ownership, executable memory
// publication and lifetime MUST be supplied by the runtime implementation.
// This hook is not enabled in the Vita product build yet.
unsigned C68k_NativeTry(c68k_struc *cpu, pointer pc, s32 remaining);
#ifdef __cplusplus
}
#endif
#endif
