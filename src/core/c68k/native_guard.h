/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C68K_NATIVE_GUARD_H
#define C68K_NATIVE_GUARD_H
#if defined(VITA_M68K_NATIVE_GUARDS) && !defined(C68K_GEN)
#ifdef __cplusplus
#include <mutex>
std::recursive_mutex &C68kNativeMutex();
extern "C" {
#endif
void C68kNativeGuardEnter(void);
void C68kNativeGuardLeave(void);
#ifdef __cplusplus
}
#endif
typedef struct { int held; } C68kNativeScope;
static inline C68kNativeScope C68kNativeScopeEnter(void) {
  C68kNativeScope scope = {1};
  C68kNativeGuardEnter();
  return scope;
}
static inline void C68kNativeScopeLeave(C68kNativeScope *scope) {
  if (scope->held) C68kNativeGuardLeave();
}
// GCC cleanup applies to early C returns and C++ exception unwinding. One
// declaration per lexical scope; nested scopes are allowed for callback IO.
#define C68K_NATIVE_GUARD \
  C68kNativeScope c68k_native_scope_ __attribute__((cleanup(C68kNativeScopeLeave))) = C68kNativeScopeEnter()
#else
#define C68K_NATIVE_GUARD ((void)0)
#endif
#endif
