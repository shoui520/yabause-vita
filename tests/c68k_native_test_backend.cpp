/* SPDX-License-Identifier: GPL-2.0-or-later */
// Test publication/dispatch backend, not the production code-cache policy.
// Compile on each hit deliberately exercises handoff without introducing a
// cache whose invalidation could mask executor-state failures. No perf claims.
#include "../src/core/c68k/a9_budgeted_region.h"
#include "../src/core/c68k/native_dispatch.h"
#include <cassert>
#include <cstring>
#include <sys/mman.h>

static unsigned hits, calls;
extern "C" void C68kNativeTestReset(void) { hits = calls = 0; }
extern "C" unsigned C68kNativeTestHits(void) { return hits; }
extern "C" unsigned C68kNativeTestCalls(void) { return calls; }
extern "C" unsigned C68k_NativeTry(c68k_struc *cpu, pointer pc, s32 remaining) {
  ++calls;
  if (remaining <= 0) return 0;
  // Fixtures use a single directly mapped 512 KiB RAM and run single-threaded.
  const pointer base = cpu->Fetch[0];
  if (pc < base || pc - base >= 0x80000 || ((pc - base) & 1)) return 0;
  const unsigned offset = unsigned(pc - base);
  const unsigned limit = (0x10000 - (offset & 0xffff)) / 2;
  std::vector<uint16_t> ops;
  for (unsigned i = 0; i < limit && i < 32; ++i) {
    uint16_t op;
    memcpy(&op, reinterpret_cast<const void *>(pc + i * 2), 2);
    if (!m68ka9::RegisterRegion::Supports(op)) break;
    ops.push_back(op);
  }
  if (ops.size() < 2) return 0;
  auto block = m68ka9::BudgetedRegion::Compile(ops);
  if (block.cycles > unsigned(remaining)) return 0;
  constexpr size_t capacity = 65536;
  static void *memory = mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(memory != MAP_FAILED && block.code.size() * 4 <= capacity);
  memcpy(memory, block.code.data(), block.code.size() * 4);
  __builtin___clear_cache(static_cast<char *>(memory),
                         static_cast<char *>(memory) + block.code.size() * 4);
  cpu->PC = pc;
  const unsigned result = reinterpret_cast<unsigned (*)(c68k_struc *, int)>(memory)(cpu, remaining);
  assert(result == block.cycles);
  ++hits;
  return result;
}
