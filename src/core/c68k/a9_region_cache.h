/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "a9_source_owner.h"
#include <utility>

namespace m68ka9 {
// One owner at a time, including Drain and destruction. Ownership can move to
// main ONLY through an acknowledged scheduler park/resume handshake: no Try
// may overlap Drain/Clear, and the handoff must establish happens-before.
// Other threads only mutate SourceOwner under its shared guard. Try performs
// no allocation, compilation, publication or artifact reclamation. Drain must
// run OUTSIDE C68k_Exec's guard. The publisher still owns executable-memory
// synchronization; this cache does not make cross-core I-cache publication safe.
// Artifact is a default-constructible, moveable owning handle with bool test.
template<class Artifact, unsigned Slots = 256, unsigned Queue = 16,
         unsigned HotThreshold = 8>
class RegionCache {
  static_assert(Slots && !(Slots & (Slots - 1)), "power-of-two cache slots");
  static_assert(Queue && HotThreshold, "nonempty queue and positive threshold");
  struct Key {
    unsigned address = ~0u, bytes = 0;
    bool operator==(Key b) const { return address == b.address && bytes == b.bytes; }
  };
  struct Entry { Key key; SourceOwner::Snapshot source; Artifact artifact{}; };
  struct Heat { Key key; unsigned count = 0; bool queued = false; };
public:
  struct Counters {
    uint64_t hits = 0, misses = 0, rejected = 0, queued = 0, queue_full = 0;
    uint64_t compiled = 0, stale_compile = 0;
  } stats;
  explicit RegionCache(SourceOwner &owner) : owner(owner) {}
  RegionCache(const RegionCache&) = delete;
  RegionCache& operator=(const RegionCache&) = delete;

  // Caller has resolved PC to ordinary physical sound RAM, bounded the span
  // to one contiguous fetch mapping, and established CPU/IRQ state ownership.
  // run returns consumed cycles; zero must leave CPU state unchanged.
  template<class Run> unsigned Try(unsigned address, unsigned bytes, Run&& run) {
    if (!Range(address, bytes)) return 0;
    const Key key{address, bytes};
    auto &entry = entries[Index(key)];
    unsigned consumed = 0;
    if (entry.key == key && owner.WithValid(entry.source, [&] {
      if (entry.artifact) { ++stats.hits; consumed = run(entry.artifact); }
      else ++stats.rejected; // Remember unsupported code until source changes.
    })) return consumed;
    ++stats.misses;
    auto &heat = heats[Index(key)];
    if (!(heat.key == key)) heat = Heat{key, 0, false};
    if (heat.queued) return 0;
    if (heat.count < HotThreshold) ++heat.count;
    if (heat.count < HotThreshold) return 0;
    if (queued == Queue) { ++stats.queue_full; return 0; }
    requests[(head + queued) % Queue] = key;
    ++queued; ++stats.queued; heat.queued = true;
    return 0;
  }

  // At most quota requests, including stale/duplicate requests, are processed.
  // build receives a stable copied source and may compile without blocking
  // writers. A concurrent write/remap rejects the artifact before installation.
  // The publisher must not overwrite storage owned by any live artifact while
  // preparing a candidate, even if that candidate is subsequently rejected.
  // An empty artifact negatively caches unsupported source. Build exceptions
  // propagate with the request consumed, leaving the old entry intact.
  template<class Build> unsigned Drain(Build&& build, unsigned quota = 1) {
    unsigned processed = 0;
    while (queued && processed < quota) {
      const Key key = requests[head];
      head = (head + 1) % Queue; --queued; ++processed;
      auto &heat = heats[Index(key)];
      if (heat.key == key) heat.queued = false;
      auto &entry = entries[Index(key)];
      if (entry.key == key && owner.WithValid(entry.source, [] {})) continue;
      auto source = owner.Capture(key.address, key.bytes);
      if (source.words.empty()) continue;
      Artifact artifact = build(source.words);
      ++stats.compiled;
      if (!owner.WithValid(source, [] {})) { ++stats.stale_compile; continue; }
      // No code runs concurrently on the owning worker. A write after the
      // validity check is still caught by Try's locked validation + execution.
      entry.artifact = std::move(artifact);
      entry.source = std::move(source);
      entry.key = key;
    }
    return processed;
  }
  unsigned Pending() const { return queued; }
  // Only outside execution and on the owning worker: reclaim published code.
  void Clear() {
    for (auto &entry : entries) entry = Entry{};
    for (auto &heat : heats) heat = Heat{};
    head = queued = 0;
  }
private:
  static bool Range(unsigned address, unsigned bytes) {
    return !(address & 1) && !(bytes & 1) && bytes &&
      bytes <= SourceOwner::MaxSourceBytes && address < SourceOwner::RamBytes &&
      bytes <= SourceOwner::RamBytes - address;
  }
  static unsigned Index(Key key) {
    return ((key.address >> 1) ^ (key.address >> 9) ^ key.bytes) & (Slots - 1);
  }
  SourceOwner &owner;
  std::array<Entry, Slots> entries{};
  std::array<Heat, Slots> heats{};
  std::array<Key, Queue> requests{};
  unsigned head = 0, queued = 0;
};
} // namespace m68ka9
