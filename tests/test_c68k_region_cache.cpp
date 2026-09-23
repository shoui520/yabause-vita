/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/a9_region_cache.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

int main() {
  using Handle = std::unique_ptr<unsigned>;
  std::vector<uint8_t> ram(m68ka9::SourceOwner::RamBytes);
  m68ka9::SourceOwner owner(ram.data());
  m68ka9::RegionCache<Handle, 8, 2, 2> cache(owner);
  unsigned built = 0, ran = 0;
  auto build = [&](const std::vector<uint16_t> &words) {
    ++built; return std::make_unique<unsigned>(words[0]);
  };
  auto run = [&](const Handle &handle) { ++ran; return *handle; };
  ram[0] = 42;
  assert(cache.Try(0, 4, run) == 0 && cache.Pending() == 0);
  assert(cache.Try(0, 4, run) == 0 && cache.Pending() == 1);
  for (unsigned i = 0; i < 100; ++i) assert(cache.Try(0, 4, run) == 0);
  assert(cache.Pending() == 1 && built == 0 && ran == 0);
  assert(cache.Drain(build, 0) == 0 && cache.Pending() == 1);
  assert(cache.Drain(build) == 1 && built == 1);
  assert(cache.Try(0, 4, run) == 42 && ran == 1);
  assert(owner.Write(1024, 2, [] {}));
  assert(cache.Try(0, 4, run) == 42);
  assert(owner.Write(0, 2, [&] { ram[0] = 7; }));
  assert(cache.Try(0, 4, run) == 0 && cache.Pending() == 1);
  assert(cache.Drain(build) == 1 && cache.Try(0, 4, run) == 7);

  // Compilation is outside source ownership: a writer can finish while the
  // builder is running. Its mutation must reject the just-built stale code.
  owner.InvalidateMapping();
  assert(cache.Try(0, 4, run) == 0);
  assert(cache.Drain([&](const auto &words) {
    std::thread writer([&] { assert(owner.Write(0, 2, [&] { ram[0] = 99; })); });
    writer.join();
    return build(words);
  }) == 1);
  assert(cache.stats.stale_compile == 1 && cache.Try(0, 4, run) == 0);
  assert(cache.Drain(build) == 1 && cache.Try(0, 4, run) == 99);

  // Unsupported code gets one negative entry, not a compile storm.
  owner.InvalidateMapping();
  assert(cache.Try(0, 4, run) == 0);
  assert(cache.Drain([](const auto &) { return Handle{}; }) == 1);
  for (unsigned i = 0; i < 100; ++i) assert(cache.Try(0, 4, run) == 0);
  assert(cache.Pending() == 0 && cache.stats.rejected == 100);
  owner.InvalidateMapping();
  assert(cache.Try(0, 4, run) == 0 && cache.Pending() == 1);
  try { cache.Drain([](const auto &) -> Handle { throw 42; }); assert(false); }
  catch (int value) { assert(value == 42); }
  assert(cache.Pending() == 0 && cache.Try(0, 4, run) == 0);
  assert(cache.Drain(build) == 1 && cache.Try(0, 4, run) == 99);

  cache.Clear();
  for (unsigned address : {0u, 2u, 4u}) {
    assert(cache.Try(address, 4, run) == 0);
    assert(cache.Try(address, 4, run) == 0);
  }
  assert(cache.Pending() == 2 && cache.stats.queue_full == 1);
  assert(cache.Drain(build) == 1 && cache.Pending() == 1);
  assert(cache.Try(4, 4, run) == 0 && cache.Pending() == 2);
  assert(cache.Drain(build, 10) == 2 && cache.Pending() == 0);
  for (auto range : {std::pair<unsigned,unsigned>{1, 4}, {0, 0}, {0, 514},
                     {0x7fffe, 4}, {0xfffffffeu, 4}, {0, 3}})
    assert(cache.Try(range.first, range.second, run) == 0);
  assert(cache.Pending() == 0);
  cache.Clear();
  ram[0] = 42; ram[16] = 7;
  for (unsigned address : {0u, 16u}) {
    cache.Try(address, 4, run); cache.Try(address, 4, run);
    assert(cache.Drain(build) == 1);
    assert(cache.Try(address, 4, run) == ram[address]);
  }
  // These keys collide: an old PC must never execute its replacement's code.
  assert(cache.Try(0, 4, run) == 0);
  owner.Rebind(nullptr);
  assert(cache.Try(0, 4, run) == 0);
  cache.Drain(build);
  cache.Clear();
  puts("68K region cache: deferred compilation, hot admission, source invalidation, negative caching, queue limits and teardown passed");
}
