/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "c68k_runtime.h"
#include "sh2_code_memory.h"
#include "../core/c68k/a9_budgeted_region.h"
#include "../core/c68k/a9_region_cache.h"
#include "../core/c68k/a9_handoff.h"
#include "../core/c68k/native_dispatch.h"
#include "../core/c68k/native_source.h"
#include <psp2/kernel/processmgr.h>
#include <algorithm>
#include <memory>
#include <cstring>
extern "C" void YuiMsg(const char *, ...);
namespace {
constexpr unsigned Slots = 128, Page = 4096, PoolSlots = Slots + 1;
static_assert(vitacode::Layout::M68k >= PoolSlots * Page, "native 68K pool requires 516 KiB");
struct Pool {
  unsigned char *base = nullptr;
  std::array<bool, PoolSlots> used{};
};
struct Code {
  Pool *pool;
  unsigned slot, cycles;
  void *Address() const { return pool->base + slot * Page; }
  ~Code() { pool->used[slot] = false; }
};
using Handle = std::unique_ptr<Code>;
struct Runtime {
  Pool pool;
  m68ka9::RegionCache<Handle, Slots, 32, 8> cache{C68kNativeSource()};
  m68ka9::Handoff handoff;
  uint64_t calls = 0, executed = 0, cycles = 0, compile_us = 0;
  unsigned frames = 0;
  bool disabled = false; // Only written by main while worker is parked.
};
Runtime *runtime;
void Require(bool valid) {
  if (!valid) { YuiMsg("m68k_native_handoff_failed"); __builtin_trap(); }
}
Handle Build(Runtime &r, const std::vector<uint16_t> &source) {
  std::vector<uint16_t> ops;
  for (uint16_t op : source) {
    if (!m68ka9::RegisterRegion::Supports(op)) break;
    ops.push_back(op);
  }
  if (ops.size() < 2) return {};
  auto block = m68ka9::BudgetedRegion::Compile(ops);
  if (block.code.empty() || block.code.size() * 4 > Page) return {};
  unsigned slot = 0;
  while (slot < PoolSlots && r.pool.used[slot]) ++slot;
  if (slot == PoolSlots) return {};
  // Allocate ownership before publication; a candidate never overwrites any
  // live artifact, including the entry it is intended to replace.
  Handle code(new Code{&r.pool, slot, block.cycles});
  r.pool.used[slot] = true;
  {
    VitaM68kCodeWrite write(code->Address(), block.code.size() * 4);
    std::memcpy(code->Address(), block.code.data(), block.code.size() * 4);
  }
  return code;
}
}
extern "C" int VitaM68kNativeInit() {
  try {
    static Runtime storage;
    storage.pool.base = VitaM68kCodeArena();
    if (!storage.pool.base) return -1;
    runtime = &storage;
    YuiMsg("m68k_native_ready slots=%u source_words=32 publication=frame_handoff", Slots);
    return 0;
  } catch (const std::bad_alloc &) { return -1; }
}
extern "C" void VitaM68kNativeStart() {
  if (runtime && !runtime->disabled) Require(runtime->handoff.Start());
}
extern "C" void VitaM68kNativePark() {
  if (runtime && !runtime->disabled) Require(runtime->handoff.Park());
}
extern "C" void VitaM68kNativeResume() {
  if (runtime && !runtime->disabled) Require(runtime->handoff.Resume());
}
extern "C" void VitaM68kNativeStop() {
  if (runtime) runtime->handoff.Stop();
}
extern "C" void VitaM68kNativePublish() {
  if (!runtime || runtime->disabled) return;
  auto &r = *runtime;
  Require(r.handoff.BeginPublish());
  const auto begin = sceKernelGetProcessTimeWide();
  try { r.cache.Drain([&](const auto &source) { return Build(r, source); }, 8); }
  catch (const std::bad_alloc &) {
    r.disabled = true;
    YuiMsg("m68k_native_disabled reason=allocation");
  }
  r.compile_us += sceKernelGetProcessTimeWide() - begin;
  if (++r.frames % 60 == 0) {
    const auto &s = r.cache.stats;
    YuiMsg("m68k_native frames=%u calls=%llu executed=%llu cycles=%llu cache_hits=%llu misses=%llu rejected=%llu compiled=%llu stale=%llu pending=%u queue_full=%llu compile_us=%llu",
      r.frames, r.calls, r.executed, r.cycles, s.hits, s.misses, s.rejected,
      s.compiled, s.stale_compile, r.cache.Pending(), s.queue_full, r.compile_us);
  }
  Require(r.handoff.EndPublish());
}
extern "C" unsigned C68k_NativeTry(c68k_struc *cpu, pointer pc, s32 remaining) {
  if (!runtime || !runtime->handoff.Running()) return 0;
  auto &r = *runtime;
  ++r.calls;
  if (cpu != &C68K || !cpu->DirectReadRam || remaining < 8) return 0;
  const pointer base = reinterpret_cast<pointer>(cpu->DirectReadRam);
  if (pc < base || pc - base >= m68ka9::SourceOwner::RamBytes || (pc & 1)) return 0;
  const unsigned physical = unsigned(pc - base);
  const unsigned guest = unsigned(pc - cpu->BasePC);
  const unsigned span = std::min({64u, 0x80000u - physical, 0x10000u - (guest & 0xffff)});
  uint16_t op;
  std::memcpy(&op, reinterpret_cast<const void *>(pc), 2);
  if (!m68ka9::RegisterRegion::Supports(op)) return 0;
  return r.cache.Try(physical, span, [&](const Handle &code) -> unsigned {
    if (code->cycles > unsigned(remaining)) return 0;
    cpu->PC = pc;
    const unsigned consumed = reinterpret_cast<unsigned (*)(c68k_struc *, int)>(code->Address())(cpu, remaining);
    ++r.executed; r.cycles += consumed;
    return consumed;
  });
}
