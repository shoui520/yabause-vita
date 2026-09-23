/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/c68k/native_guard.h"
#include "../core/c68k/native_source.h"
#ifdef VITA_M68K_NATIVE_GUARDS
std::recursive_mutex &C68kNativeMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}
extern "C" void C68kNativeGuardEnter(void) { C68kNativeMutex().lock(); }
extern "C" void C68kNativeGuardLeave(void) { C68kNativeMutex().unlock(); }
m68ka9::SourceOwner &C68kNativeSource() {
  static m68ka9::SourceOwner source(nullptr, &C68kNativeMutex());
  return source;
}
extern "C" void C68kNativeSourceBind(void *ram) {
  C68kNativeSource().Rebind(static_cast<uint8_t *>(ram));
}
extern "C" void C68kNativeSourceRemap(void) {
  C68kNativeSource().InvalidateMapping();
}
extern "C" void C68kNativeSourceChanged(unsigned address, unsigned bytes) {
  // The physical writers already hold the same mutex before touching RAM.
  // Invalid ranges conservatively invalidate all sources rather than allowing
  // a malformed/wrapping notification to retain a stale compiled block.
  if (!C68kNativeSource().Write(address, bytes, [] {}))
    C68kNativeSource().InvalidateMapping();
}
#endif
