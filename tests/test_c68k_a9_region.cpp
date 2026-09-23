/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/a9_register_region.h"
#include "../src/core/c68k/a9_budgeted_region.h"
#include "../src/core/c68k/a9_source_owner.h"
#include "../src/core/c68k/a9_region_cache.h"
#include "../src/core/c68k/c68k.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <memory>
#include <sys/mman.h>

int main() {
  static_assert(offsetof(c68k_struc, flag_C) == 64);
  static_assert(offsetof(c68k_struc, flag_X) == 80);
  constexpr size_t capacity = 262144;
  auto *memory = static_cast<uint32_t *>(mmap(nullptr, capacity,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
  alignas(16) uint16_t program[32768] = {};
  c68k_struc reference;
  C68k_Init(&reference, nullptr);
  C68k_Set_Fetch(&reference, 0, sizeof(program) - 1, (pointer)program);
  std::vector<uint16_t> supported;
  for (unsigned op = 0; op < 65536; ++op)
    if (m68ka9::RegisterRegion::Supports(op)) supported.push_back(op);
  std::mt19937 random(0x68a9496);
  unsigned cases = 0, instructions = 0;
  auto check = [&](const std::vector<uint16_t>& ops, unsigned edge, bool boundaries) {
    constexpr uint32_t edges[] = {0, 0xffffffffu, 0x80000000u, 0x7fffffffu};
    for (unsigned r = 0; r < 8; ++r) {
      reference.D[r] = edge ? edges[(r + edge) & 3] : random();
      reference.A[r] = edge ? edges[(r + edge + 1) & 3] : random();
    }
    C68k_Set_SR(&reference, 0x2000 | (random() & 31));
    C68k_Set_PC(&reference, 0);
    reference.Status = 0; reference.IRQLine = 0;
    c68k_struc actual = reference, initial = actual;
    m68ka9::RegisterRegion region;
    unsigned reference_cycles = 0;
    for (auto op : ops) {
      assert(region.Emit(op));
      if (boundaries && (random() & 7) == 0) region.Flush();
      program[0] = op;
      C68k_Set_PC(&reference, 0);
      const s32 elapsed = C68k_Exec(&reference, 1); // Exactly one instruction.
      if (elapsed <= 0 || unsigned(elapsed) != m68ka9::RegisterRegion::Cycles(op))
        fprintf(stderr, "Cycle mismatch op=%04x C68K=%d region=%u\n",
                op, elapsed, m68ka9::RegisterRegion::Cycles(op));
      assert(elapsed > 0 && unsigned(elapsed) == m68ka9::RegisterRegion::Cycles(op));
      reference_cycles += elapsed;
    }
    const size_t before = region.code.size();
    assert(!region.Emit(0x4e75)); // RTS requires a separate observable boundary.
    assert(region.code.size() == before && region.instructions == ops.size());
    assert(region.cycles == reference_cycles);
    region.Flush();
    std::vector<uint32_t> function{0xe92d5ff0}; // PUSH r4-r12,lr: 40-byte frame
    function.insert(function.end(), region.code.begin(), region.code.end());
    function.push_back(0xe8bd9ff0); // POP r4-r12,pc
    assert(function.size() * 4 <= capacity);
    memcpy(memory, function.data(), function.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
                           reinterpret_cast<char *>(memory) + function.size() * 4);
    reinterpret_cast<void (*)(c68k_struc *)>(memory)(&actual);
    if (memcmp(actual.D, reference.D, sizeof(actual.D)) ||
        memcmp(actual.A, reference.A, sizeof(actual.A)) ||
        C68k_Get_SR(&actual) != C68k_Get_SR(&reference)) {
      fprintf(stderr, "FAIL case=%u first=%04x len=%zu actual_sr=%x expected_sr=%x\n",
              cases, ops[0], ops.size(), C68k_Get_SR(&actual), C68k_Get_SR(&reference));
      for (unsigned i = 0; i < 8; ++i)
        fprintf(stderr, "D%u %08x/%08x A%u %08x/%08x\n", i, actual.D[i], reference.D[i],
                i, actual.A[i], reference.A[i]);
      assert(false);
    }
    // No accidental modification of mask/supervisor state, callbacks or cycles.
    const size_t tail = offsetof(c68k_struc, flag_I);
    assert(!memcmp(reinterpret_cast<char *>(&actual) + tail,
                   reinterpret_cast<char *>(&initial) + tail, sizeof(actual) - tail));
    ++cases; instructions += ops.size();
  };
  for (auto op : supported) for (unsigned edge = 0; edge <= 4; ++edge)
    check({op}, edge, false);
  // X must survive CMP/logical/address work, including after dirty eviction.
  const std::vector<std::vector<uint16_t>> x_sequences = {
    {0xd081, 0xb082, 0x8083, 0x2040},
    {0x9081, 0x2040, 0x7000, 0xb082},
    {0xd081, 0x8083, 0xb082, 0xb083},
    {0x4480, 0x4680, 0x4281, 0xb081},
    {0xd081, 0x9082, 0xd083, 0x9084, 0x4a80}
  };
  for (const auto &ops : x_sequences) for (unsigned edge = 0; edge <= 4; ++edge)
    check(ops, edge, false);
  // A run of X-overwriting arithmetic needs one X extraction at the exit,
  // not one per guest instruction. Assert emitted work, not QEMU wall time.
  m68ka9::RegisterRegion chain;
  for (unsigned i = 0; i < 32; ++i) assert(chain.Emit(0xd081));
  chain.Flush();
  auto count_word = [](const m68ka9::RegisterRegion &region, uint32_t wanted,
                       uint32_t mask = 0xffffffffu) {
    unsigned count = 0;
    for (uint32_t word : region.code) count += (word & mask) == wanted;
    return count;
  };
  assert(count_word(chain, 0xe20bbc01u) == 1);
  assert(count_word(chain, 0xe10f9000u) == 1); // Only final arithmetic NZCV.
  assert(count_word(chain, 0xe1a0a000u, 0xfffffff0u) == 1); // Final result only.
  m68ka9::RegisterRegion preserved_x;
  assert(preserved_x.Emit(0xd081)); // ADD sets X.
  assert(preserved_x.Emit(0xb082)); // CMP preserves ADD's X, replaces NZCV.
  preserved_x.Flush();
  assert(count_word(preserved_x, 0xe10f9000u) == 2);
  assert(count_word(preserved_x, 0xe20bbc01u) == 1);
  // Once a later ADD overwrites X too, both earlier captures are dead.
  m68ka9::RegisterRegion overwritten;
  for (auto op : {0xd081, 0xb082, 0xd083}) assert(overwritten.Emit(op));
  overwritten.Flush();
  assert(count_word(overwritten, 0xe10f9000u) == 1);
  assert(count_word(overwritten, 0xe20bbc01u) == 1);
  chain.Emit(0xd081); chain.Flush();
  assert(count_word(chain, 0xe10f9000u) == 2); // Never prune across a flush.
  // All MOVEQ signed immediates need one ARM constant instruction, no MOVT.
  for (unsigned byte = 0; byte < 256; ++byte) {
    m68ka9::RegisterRegion constant;
    assert(constant.Emit(uint16_t(0x7000 | byte)));
    constant.Flush();
    assert(!constant.code.empty());
    assert((constant.code[0] & 0xffff0fffu) == ((byte < 128 ? 0xe3a00000u : 0xe3e00000u) |
           (byte < 128 ? byte : 255 - byte)));
  }
  for (unsigned run = 0; run < 2000; ++run) {
    std::vector<uint16_t> ops;
    for (unsigned i = 0; i < 256; ++i) ops.push_back(supported[random() % supported.size()]);
    check(ops, 0, run & 1);
  }
  unsigned budget_cases = 0;
  auto check_budget = [&](const std::vector<uint16_t>& ops) {
    auto block = m68ka9::BudgetedRegion::Compile(ops);
    assert(block.bytes == ops.size() * 2 && !block.code.empty());
    assert(block.code.size() * 4 <= capacity);
    memcpy(memory, block.code.data(), block.code.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
                           reinterpret_cast<char *>(memory) + block.code.size() * 4);
    for (unsigned r = 0; r < 8; ++r) {
      reference.D[r] = random(); reference.A[r] = random();
    }
    C68k_Set_SR(&reference, 0x2000 | (random() & 31));
    C68k_Set_PC(&reference, 0);
    reference.Status = 0; reference.IRQLine = 0;
    reference.CycleSup = 0; reference.CycleToDo = block.cycles;
    reference.CycleIO = -123;
    for (size_t i = 0; i < ops.size(); ++i) program[i] = ops[i];
    c68k_struc initial = reference;
    assert(unsigned(C68k_Exec(&reference, block.cycles)) == block.cycles);
    const int budgets[] = {-2147483647 - 1, -1, 0, 1, int(block.cycles) - 1,
                           int(block.cycles), int(block.cycles) + 1, 2147483647};
    for (int budget : budgets) {
      c68k_struc actual = initial;
      unsigned consumed = reinterpret_cast<unsigned (*)(c68k_struc *, int)>(memory)(&actual, budget);
      if (budget < int(block.cycles)) {
        assert(consumed == 0 && !memcmp(&actual, &initial, sizeof(actual)));
      } else {
        assert(consumed == block.cycles);
        assert(!memcmp(actual.D, reference.D, sizeof(actual.D)));
        assert(!memcmp(actual.A, reference.A, sizeof(actual.A)));
        assert(C68k_Get_SR(&actual) == C68k_Get_SR(&reference));
        // Only CycleIO differs when caller grants a larger execution window.
        c68k_struc expected = reference;
        expected.CycleIO += budget - int(block.cycles);
        const size_t tail = offsetof(c68k_struc, flag_I);
        assert(!memcmp(reinterpret_cast<char *>(&actual) + tail,
                       reinterpret_cast<char *>(&expected) + tail, sizeof(actual) - tail));
      }
      ++budget_cases;
    }
  };
  for (auto op : supported) check_budget({op});
  for (unsigned run = 0; run < 1000; ++run) {
    std::vector<uint16_t> ops;
    for (unsigned i = 0; i < 256; ++i) ops.push_back(supported[random() % supported.size()]);
    check_budget(ops);
  }
  assert(m68ka9::BudgetedRegion::Compile({}).code.empty());
  assert(m68ka9::BudgetedRegion::Compile({0x7001, 0x4e75}).code.empty());
  assert(m68ka9::BudgetedRegion::Compile(std::vector<uint16_t>(257, 0x4e71)).code.empty());
  // Connect ownership to REAL executable output: changed source must reject
  // even when an old, otherwise valid native function remains in the arena.
  std::vector<uint8_t> source_ram(m68ka9::SourceOwner::RamBytes);
  m68ka9::SourceOwner owner(source_ram.data());
  assert(owner.Write(0, 4, [&] {
    source_ram[0] = 42; source_ram[1] = 0x70;
    source_ram[2] = 0x71; source_ram[3] = 0x4e;
  }));
  auto snapshot = owner.Capture(0, 4);
  C68k_Set_Fetch(&reference, 0, source_ram.size() - 1, (pointer)source_ram.data());
  C68k_Set_PC(&reference, 0);
  auto publish = [&](const m68ka9::SourceOwner::Snapshot& source) {
    auto block = m68ka9::BudgetedRegion::Compile(source.words);
    assert(!block.code.empty() && block.code.size() * 4 <= capacity);
    memcpy(memory, block.code.data(), block.code.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
                           reinterpret_cast<char *>(memory) + block.code.size() * 4);
  };
  publish(snapshot);
  auto invoke = [&] {
    assert(reinterpret_cast<unsigned (*)(c68k_struc *, int)>(memory)(&reference, 8) == 8);
  };
  reference.D[0] = 0;
  assert(owner.WithValid(snapshot, invoke) && reference.D[0] == 42);
  assert(owner.Write(0, 1, [&] { source_ram[0] = 7; }));
  C68k_Set_PC(&reference, 0);
  reference.D[0] = 99;
  assert(!owner.WithValid(snapshot, invoke) && reference.D[0] == 99);
  snapshot = owner.Capture(0, 4);
  publish(snapshot);
  assert(owner.WithValid(snapshot, invoke) && reference.D[0] == 7);
  owner.Rebind(source_ram.data());
  reference.D[0] = 99;
  assert(!owner.WithValid(snapshot, invoke) && reference.D[0] == 99);
  // Cached artifacts each own their executable mapping. Publication of a new
  // candidate must never overwrite a still-live old artifact before admission.
  struct NativeImage {
    void *address;
    size_t bytes;
    explicit NativeImage(const std::vector<uint32_t> &code) : bytes(code.size() * 4) {
      address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      assert(address != MAP_FAILED);
      memcpy(address, code.data(), bytes);
      __builtin___clear_cache(static_cast<char *>(address), static_cast<char *>(address) + bytes);
    }
    ~NativeImage() { assert(munmap(address, bytes) == 0); }
  };
  using NativeHandle = std::unique_ptr<NativeImage>;
  m68ka9::RegionCache<NativeHandle, 4, 2, 1> cache(owner);
  unsigned cache_compiles = 0;
  auto compile_cached = [&](const std::vector<uint16_t> &words) -> NativeHandle {
    ++cache_compiles;
    auto block = m68ka9::BudgetedRegion::Compile(words);
    if (block.code.empty()) return {};
    return std::make_unique<NativeImage>(block.code);
  };
  auto cached_run = [&](const NativeHandle &image) {
    return reinterpret_cast<unsigned (*)(c68k_struc *, int)>(image->address)(&reference, 8);
  };
  C68k_Set_PC(&reference, 0);
  auto before_cache = reference;
  assert(cache.Try(0, 4, cached_run) == 0 && cache_compiles == 0);
  assert(!memcmp(&reference, &before_cache, sizeof(reference)));
  assert(cache.Drain(compile_cached) == 1);
  assert(cache.Try(0, 4, cached_run) == 8 && reference.D[0] == 7);
  C68k_Set_PC(&reference, 0);
  before_cache = reference;
  assert(cache.Try(0, 4, [&](const NativeHandle &image) {
    return reinterpret_cast<unsigned (*)(c68k_struc *, int)>(image->address)(&reference, 7);
  }) == 0);
  assert(!memcmp(&reference, &before_cache, sizeof(reference)));
  assert(owner.Write(0, 1, [&] { source_ram[0] = 42; }));
  reference.D[0] = 99;
  assert(cache.Try(0, 4, cached_run) == 0 && reference.D[0] == 99);
  assert(cache.Drain(compile_cached) == 1);
  assert(cache.Try(0, 4, cached_run) == 8 && reference.D[0] == 42);
  assert(cache_compiles == 2);
  cache.Clear();
  assert(munmap(memory, capacity) == 0);
  printf("68K A32 regions: %zu encodings, %u cases, %u instructions matched C68K\n",
         supported.size(), cases, instructions);
  printf("68K budgeted regions: %u admission/PC/cycle-state cases passed\n", budget_cases);
  puts("68K source-owned native execution: stale code rejected; updated code executed");
  puts("68K cached A32 execution: cold fallback, deferred publication, budget rejection and invalidation passed");
}
