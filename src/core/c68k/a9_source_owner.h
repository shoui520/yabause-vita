/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace m68ka9 {
// Source ownership for the experimental native tier.
// Every physical sound-RAM writer must call Write BEFORE mutation. Legacy
// after-write notifications cannot satisfy this protocol. Addresses are
// normalized physical offsets, after the Saturn RAM mirror/mode checks.
// Mutex synchronization supplies ordering (ARM A9 MPCore TRM 1.7.1); it does
// not publish executable instructions into I-cache (TRM 2.1).
class SourceOwner {
public:
  static constexpr unsigned RamBytes = 0x80000, PageBytes = 256;
  static constexpr unsigned MaxSourceBytes = 512;
  struct Snapshot {
    unsigned address = 0;
    std::vector<uint16_t> words;
  private:
    friend class SourceOwner;
    const SourceOwner *owner = nullptr;
    uint64_t mapping = 0;
    unsigned first_page = 0, pages = 0;
    std::array<uint64_t, 3> versions{};
  };

  explicit SourceOwner(uint8_t *physical_ram, std::recursive_mutex *shared = nullptr)
      : mutex(shared ? *shared : local_mutex), ram(physical_ram) {}
  SourceOwner(const SourceOwner&) = delete;
  SourceOwner& operator=(const SourceOwner&) = delete;

  Snapshot Capture(unsigned address, unsigned bytes) {
    Snapshot result;
    if (!Range(address, bytes) || (address & 1) || (bytes & 1) ||
        bytes > MaxSourceBytes) return result;
    // Reserve before locking; compilation and allocation do not hold writers.
    result.words.resize(bytes / 2);
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!ram) { result.words.clear(); return result; }
    result.owner = this; result.mapping = mapping;
    result.address = address; result.first_page = address / PageBytes;
    result.pages = (address + bytes - 1) / PageBytes - result.first_page + 1;
    for (unsigned i = 0; i < result.pages; ++i)
      result.versions[i] = versions[result.first_page + i];
    // Saturn T2 memory: byte-swapped guest words on little-endian A9.
    for (unsigned i = 0; i < bytes; i += 2)
      result.words[i / 2] = uint16_t(ram[address + i]) |
                           (uint16_t(ram[address + i + 1]) << 8);
    return result;
  }

  // The callable is restricted to a bounded native arithmetic unit with NO
  // memory callbacks/recursive ownership operations. It runs under ownership,
  // not merely after an unlocked validity check. False means no execution.
  template<class Run> bool WithValid(const Snapshot& source, Run&& run) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!ram || source.owner != this || source.mapping != mapping ||
        source.words.empty()) return false;
    for (unsigned i = 0; i < source.pages; ++i)
      if (source.versions[i] != versions[source.first_page + i]) return false;
    run();
    return true;
  }

  // Mutation must touch ONLY the declared range and must not recurse. Invalid
  // physical ranges reject before invoking it. Invalidate before invoking so
  // even a throwing mutation cannot leave an apparently valid old snapshot.
  template<class Mutation> bool Write(unsigned address, unsigned bytes, Mutation&& mutation) {
    if (!Range(address, bytes)) return false;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!ram) return false;
    for (unsigned page = address / PageBytes; page <= (address + bytes - 1) / PageBytes; ++page)
      ++versions[page];
    mutation();
    return true;
  }

  // Caller owns allocation lifetime. Rebinding (including the same pointer)
  // invalidates all compiled snapshots; old memory can be freed after return.
  void Rebind(uint8_t *physical_ram) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    ++mapping;
    ram = physical_ram;
  }

  // Fetch-bank remapping can change the meaning of a compiled guest PC even
  // without changing any physical RAM byte or the allocation itself.
  void InvalidateMapping() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    ++mapping;
  }

private:
  static bool Range(unsigned address, unsigned bytes) {
    return bytes && address < RamBytes && bytes <= RamBytes - address;
  }
  std::recursive_mutex local_mutex;
  std::recursive_mutex &mutex;
  uint8_t *ram;
  uint64_t mapping = 1;
  std::array<uint64_t, RamBytes / PageBytes> versions{};
};
} // namespace m68ka9
