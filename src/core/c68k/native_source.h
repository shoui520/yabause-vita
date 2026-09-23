/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C68K_NATIVE_SOURCE_H
#define C68K_NATIVE_SOURCE_H
#if defined(VITA_M68K_NATIVE_GUARDS) && !defined(C68K_GEN)
#ifdef __cplusplus
#include "a9_source_owner.h"
m68ka9::SourceOwner &C68kNativeSource();
extern "C" {
#endif
void C68kNativeSourceBind(void *ram);
void C68kNativeSourceRemap(void);
// Post-write notification is safe ONLY when the writer holds the shared
// native guard continuously from before mutation through this call. Offsets
// are physical sound-RAM offsets, not guest aliases or SH-2 bus addresses.
void C68kNativeSourceChanged(unsigned address, unsigned bytes);
#ifdef __cplusplus
}
#endif
#else
static inline void C68kNativeSourceBind(void *ram) { (void)ram; }
static inline void C68kNativeSourceRemap(void) {}
static inline void C68kNativeSourceChanged(unsigned address, unsigned bytes) {
  (void)address; (void)bytes;
}
#endif
#endif
