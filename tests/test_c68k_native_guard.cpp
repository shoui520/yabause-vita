/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/native_guard.h"
#include "../src/core/c68k/a9_source_owner.h"
#include "../src/core/c68k/native_source.h"
#include <cassert>
#include <cstdio>
#include <thread>
extern "C" int c68k_guard_fixture(int, void (*)(void));
extern "C" void c68k_source_fixture_write(void *, unsigned, unsigned);
static unsigned callbacks;
static void recursive_callback() { C68K_NATIVE_GUARD; ++callbacks; }
static void check_released() {
  bool acquired = false;
  std::thread other([&] {
    acquired = C68kNativeMutex().try_lock();
    if (acquired) C68kNativeMutex().unlock();
  });
  other.join();
  assert(acquired);
}
int main() {
  assert(c68k_guard_fixture(1, recursive_callback) == 11);
  check_released();
  assert(c68k_guard_fixture(0, recursive_callback) == 22 && callbacks == 1);
  check_released();
  try { C68K_NATIVE_GUARD; throw 42; } catch (int n) { assert(n == 42); }
  check_released();
  std::vector<uint8_t> ram(m68ka9::SourceOwner::RamBytes);
  m68ka9::SourceOwner owner(ram.data(), &C68kNativeMutex());
  {
    C68K_NATIVE_GUARD;
    assert(owner.Write(0, 2, [&] { ram[0] = 42; }));
    auto source = owner.Capture(0, 2);
    assert(owner.WithValid(source, [&] { assert(source.words[0] == 42); }));
    bool acquired = true;
    std::thread other([&] {
      acquired = C68kNativeMutex().try_lock();
      if (acquired) C68kNativeMutex().unlock();
    });
    other.join();
    assert(!acquired);
  }
  check_released();
  // A shared non-atomic value is safe only while both participants own the
  // same guard. This also exercises repeated cross-thread handoff.
  unsigned count = 0;
  auto increment = [&] {
    for (unsigned i = 0; i < 20000; ++i) { C68K_NATIVE_GUARD; ++count; }
  };
  std::thread worker(increment);
  increment(); worker.join();
  assert(count == 40000);
  // Exercise the actual C -> runtime bridge used by sound-RAM writers.
  C68kNativeSourceBind(ram.data());
  auto &runtime = C68kNativeSource();
  auto snapshot = runtime.Capture(254, 4);
  auto valid = [&] { return runtime.WithValid(snapshot, [] {}); };
  assert(valid());
  c68k_source_fixture_write(ram.data(), 1024, 2);
  assert(valid()); // Unrelated page does not evict this block.
  c68k_source_fixture_write(ram.data(), 255, 2);
  assert(ram[255] == 17 && ram[256] == 17 && !valid());
  snapshot = runtime.Capture(254, 4);
  C68kNativeSourceRemap();
  assert(!valid());
  snapshot = runtime.Capture(254, 4);
  C68kNativeSourceBind(ram.data()); // Same allocation, different lifetime.
  assert(!valid());
  snapshot = runtime.Capture(254, 4);
  { C68K_NATIVE_GUARD; C68kNativeSourceChanged(0x7ffff, 2); }
  assert(!valid()); // Malformed/wrapping range cannot retain stale code.
  snapshot = runtime.Capture(254, 4);
  C68kNativeSourceBind(nullptr);
  assert(!valid() && runtime.Capture(0, 2).words.empty());
  puts("68K native guard: C early-return, recursive IO, C++ unwind, shared source ownership and contention passed");
  puts("68K runtime source bridge: page writes, remap, rebind and teardown passed");
}
