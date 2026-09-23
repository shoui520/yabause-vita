/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/native_guard.h"
#include "../src/core/c68k/native_source.h"

void c68k_source_fixture_write(void *ram, unsigned address, unsigned bytes) {
  C68K_NATIVE_GUARD;
  unsigned char *memory = ram;
  for (unsigned i = 0; i < bytes; ++i) memory[address + i] = 17;
  C68kNativeSourceChanged(address, bytes);
}
int c68k_guard_fixture(int early, void (*callback)(void)) {
  C68K_NATIVE_GUARD;
  if (early) return 11;
  callback();
  return 22;
}
