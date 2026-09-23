/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/a9_source_owner.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
  std::vector<uint8_t> ram(m68ka9::SourceOwner::RamBytes);
  for (unsigned i = 0; i < ram.size(); ++i) ram[i] = i * 13u;
  m68ka9::SourceOwner owner(ram.data());
  unsigned executed = 0;
  auto valid = [&](const m68ka9::SourceOwner::Snapshot &s) {
    return owner.WithValid(s, [&] { ++executed; });
  };
  auto source = owner.Capture(254, 512); // Three pages, not only two.
  assert(source.words.size() == 256);
  assert(source.words[0] == uint16_t(ram[254] | ram[255] << 8));
  assert(valid(source));
  assert(owner.Write(1024, 1, [&] { ram[1024] ^= 1; }));
  assert(valid(source)); // No whole-cache invalidation for unrelated pages.
  assert(owner.Write(765, 1, [&] { ram[765] ^= 1; }));
  assert(!valid(source));
  source = owner.Capture(254, 512);
  assert(owner.Write(255, 2, [&] { ram[255] ^= 1; ram[256] ^= 1; }));
  assert(!valid(source));
  auto last = owner.Capture(ram.size() - 2, 2);
  assert(valid(last));
  bool mutated = false;
  for (auto span : {std::pair<unsigned, unsigned>{0, 0}, {unsigned(ram.size()), 1},
                    {unsigned(ram.size() - 1), 2}, {1, 0xffffffffu}})
    assert(!owner.Write(span.first, span.second, [&] { mutated = true; }));
  assert(!mutated && valid(last));
  assert(owner.Capture(1, 2).words.empty());
  assert(owner.Capture(0, 513).words.empty());
  assert(owner.Capture(ram.size() - 2, 4).words.empty());
  assert(owner.Capture(0, 0).words.empty());
  owner.Rebind(ram.data());
  assert(!valid(last));
  source = owner.Capture(0, 2);
  m68ka9::SourceOwner other(ram.data());
  assert(!other.WithValid(source, [&] { assert(false); }));

  // Deterministic ownership check: writer attempts while native execution
  // owns the source. It cannot mutate until that callback relinquishes it.
  std::atomic<bool> attempted{false}, completed{false};
  std::thread writer;
  const uint8_t original = ram[0];
  assert(owner.WithValid(source, [&] {
    writer = std::thread([&] {
      attempted.store(true, std::memory_order_release);
      assert(owner.Write(0, 1, [&] { ram[0] ^= 1; }));
      completed.store(true, std::memory_order_release);
    });
    while (!attempted.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(!completed.load(std::memory_order_acquire));
    assert(ram[0] == original);
  }));
  writer.join();
  assert(completed.load() && !valid(source));
  // Compile outside the lock, then reject if source changes before entry.
  source = owner.Capture(0, 2);
  assert(owner.Write(0, ram.size(), [&] { ram[0] ^= 1; }));
  assert(!valid(source));
  owner.Rebind(nullptr);
  assert(owner.Capture(0, 2).words.empty());
  assert(!owner.Write(0, 1, [&] { assert(false); }));
  puts("68K source ownership: ranges, cross-page writes, rebind, stale compilation and concurrent exclusion passed");
}
