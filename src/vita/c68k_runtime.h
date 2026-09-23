/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_C68K_RUNTIME_H
#define VITA_C68K_RUNTIME_H
#ifdef VITA_M68K_NATIVE_EXECUTION
#ifdef __cplusplus
extern "C" {
#endif
int VitaM68kNativeInit(void);
void VitaM68kNativeStart(void);
void VitaM68kNativePark(void);
void VitaM68kNativePublish(void);
void VitaM68kNativeResume(void);
void VitaM68kNativeStop(void);
#ifdef __cplusplus
}
#endif
#else
static inline int VitaM68kNativeInit(void) { return 0; }
static inline void VitaM68kNativeStart(void) {}
static inline void VitaM68kNativePark(void) {}
static inline void VitaM68kNativePublish(void) {}
static inline void VitaM68kNativeResume(void) {}
static inline void VitaM68kNativeStop(void) {}
#endif
#endif
