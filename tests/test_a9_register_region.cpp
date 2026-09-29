/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/a9_register_region.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <sys/mman.h>

static std::vector<uint8_t> low_ram(1u << 20), high_ram(1u << 20);
extern "C" void sh2_counted_loop_oracle(unsigned variant, unsigned reg, uint32_t *state);
extern "C" void sh2_div1_oracle(uint32_t *state, unsigned m, unsigned n);
extern "C" void sh2_rotcl_oracle(uint32_t *state, unsigned n);
extern "C" void sh2_carry_oracle(uint32_t *state, unsigned instruction);

static void Reference(uint16_t op, uint32_t *r, uint32_t pc = 0) {
  unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
  if ((op >> 8) == 0xc9) { r[0] &= op & 255; return; }
  if ((op >> 8) == 0xca) { r[0] ^= op & 255; return; }
  if ((op >> 8) == 0xcb) { r[0] |= op & 255; return; }
  if ((op >> 12) == 9 || (op >> 12) == 13) {
    const unsigned width = (op >> 12) == 9 ? 2 : 4;
    const uint32_t address = ((pc + 4) & (width == 4 ? ~3u : ~0u)) + (op & 255) * width;
    const auto &ram = (address >> 20) == 2 ? low_ram : high_ram;
    uint32_t value = 0;
    for (unsigned i=0;i<width;++i) value=(value<<8)|ram[((address&0xfffff)+i)^1];
    r[n] = width == 2 ? uint32_t(int32_t(int16_t(value))) : value;
    return;
  }
  if ((op & 0xf00f) == 0x0007) { r[20] = uint32_t(uint64_t(r[n]) * r[m]); return; }
  if ((op & 0xf0ff) == 0x000a) { r[n] = r[19]; return; }
  if ((op & 0xf0ff) == 0x001a) { r[n] = r[20]; return; }
  // Other MACH/MACL operations, as their baseline templates compute them.
  if (op == 0x0028) { r[19] = r[20] = 0; return; }
  if ((op & 0xf0ff) == 0x400a) { r[19] = r[n]; return; }
  if ((op & 0xf0ff) == 0x401a) { r[20] = r[n]; return; }
  if ((op & 0xf00f) == 0x200d) { r[n] = (r[m] << 16) | (r[n] >> 16); return; }
  if ((op & 0xf00f) == 0x300d || (op & 0xf00f) == 0x3005) {
    const uint64_t p = (op & 8) ? uint64_t(int64_t(int32_t(r[n])) * int32_t(r[m]))
                                : uint64_t(r[n]) * r[m];
    r[19] = uint32_t(p >> 32); r[20] = uint32_t(p);
    return;
  }
  if ((op & 0xf00f) == 0x200f) { r[20] = uint32_t(int32_t(int16_t(r[n])) * int16_t(r[m])); return; }
  if ((op & 0xf00f) == 0x200e) { r[20] = (r[n] & 0xffff) * (r[m] & 0xffff); return; }
  if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) {
    const auto pc = r[22], cycles = r[23];
    sh2_carry_oracle(r, op);
    r[22] = pc; r[23] = cycles;
    return;
  }
  if (sh2a9::RegisterRegion::IsDivisionStep(op)) {
    const auto pc = r[22], cycles = r[23];
    if ((op & 0xf0ff) == 0x4024) sh2_rotcl_oracle(r, n);
    else sh2_div1_oracle(r, m, n);
    r[22] = pc; r[23] = cycles;
    return;
  }
  auto t = [&](bool value) { r[16] = (r[16] & ~1u) | unsigned(value); };
  if (op == 8 || op == 0x18) { t(op == 0x18); return; }
  if ((op & 0xf0ff) == 0x0029) { r[n] = r[16] & 1; return; }
  if ((op & 0xf0ff) == 0x4010) { --r[n]; t(r[n] == 0); return; }
  if ((op & 0xf0ff) == 0x4011) { t(int32_t(r[n]) >= 0); return; }
  if ((op & 0xf0ff) == 0x4015) { t(int32_t(r[n]) > 0); return; }
  if ((op >> 8) == 0x88) { t(r[0] == uint32_t(int32_t(int8_t(op)))); return; }
  if ((op >> 8) == 0xc8) { t((r[0] & (op & 255)) == 0); return; }
  switch (op & 0xf00f) {
  case 0x3000: t(r[n] == r[m]); return;
  case 0x3002: t(r[n] >= r[m]); return;
  case 0x3003: t(int32_t(r[n]) >= int32_t(r[m])); return;
  case 0x3006: t(r[n] > r[m]); return;
  case 0x3007: t(int32_t(r[n]) > int32_t(r[m])); return;
  case 0x2008: t((r[n] & r[m]) == 0); return;
  }
  if ((op & 0xf00f) >= 0x6000 && (op & 0xf00f) <= 0x6002) {
    const unsigned width = 1u << (op & 3);
    const auto &ram = (r[m] >> 20) == 2 ? low_ram : high_ram;
    const unsigned offset = r[m] & 0xfffff;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value = (value << 8) | ram[(offset + i) ^ 1];
    if (width == 1) value = int8_t(value);
    if (width == 2) value = int16_t(value);
    r[n] = value;
    return;
  }
  if (op == 9) return;
  if ((op >> 12) == 0xe) { r[n] = int8_t(op); return; }
  if ((op >> 12) == 7) { r[n] += int8_t(op); return; }
  if ((op & 0xf0ff) == 0x4000 || (op & 0xf0ff) == 0x4020) { t(r[n] >> 31); r[n] <<= 1; return; }
  if ((op & 0xf0ff) == 0x4001) { t(r[n] & 1); r[n] >>= 1; return; }
  if ((op & 0xf0ff) == 0x4021) { t(r[n] & 1); r[n] = uint32_t(int32_t(r[n]) >> 1); return; }
  if ((op & 0xf000) == 0x4000) {
    unsigned shift = (op & 0x30) == 0 ? 2 : (op & 0x30) == 0x10 ? 8 : 16;
    r[n] = (op & 1) ? r[n] >> shift : r[n] << shift;
    return;
  }
  switch (op & 0xf00f) {
  case 0x300c: r[n] += r[m]; break;
  case 0x3008: r[n] -= r[m]; break;
  case 0x2009: r[n] &= r[m]; break;
  case 0x200a: r[n] ^= r[m]; break;
  case 0x200b: r[n] |= r[m]; break;
  case 0x6003: r[n] = r[m]; break;
  case 0x6007: r[n] = ~r[m]; break;
  case 0x600b: r[n] = 0u - r[m]; break;
  case 0x6009: r[n] = (r[m] << 16) | (r[m] >> 16); break;
  case 0x600c: r[n] = r[m] & 255; break;
  case 0x600d: r[n] = r[m] & 65535; break;
  case 0x600e: r[n] = static_cast<int8_t>(r[m]); break;
  case 0x600f: r[n] = static_cast<int16_t>(r[m]); break;
  default: assert(false);
  }
}

int main() {
  constexpr size_t capacity = 65536;
  auto *memory = static_cast<uint32_t *>(mmap(nullptr, capacity,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
  std::mt19937 random(0x534832);
  for (auto &v : low_ram) v = random();
  for (auto &v : high_ram) v = random();
  std::vector<uint16_t> supported;
  for (unsigned op = 0; op < 65536; ++op)
    if (sh2a9::RegisterRegion::Supports(op)) supported.push_back(op);
  unsigned cases = 0;
  auto test = [&](const std::vector<uint16_t>& ops, bool boundaries, unsigned edge = 0, uint32_t pc = 0,
                  const std::function<void(uint32_t *)> &setup = {}) {
    // Extra words include SR: predicates/ROTCL change T, DIV1 changes Q/T.
    uint32_t actual[32], expected[32];
    for (auto &r : actual) r = random();
    if (edge) {
      constexpr uint32_t values[] = {0, 0xffffffffu, 0x80008080u, 0x7fff7f7fu};
      for (unsigned i = 0; i < 16; ++i) actual[i] = values[(i + edge) & 3];
    }
    if (pc) actual[22]=pc;
    if (setup) setup(actual);
    std::memcpy(expected, actual, sizeof(actual));
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low_ram.data()),
                               reinterpret_cast<uint32_t>(high_ram.data()));
    uint32_t guest_pc=pc;
    for (auto op : ops) {
      const auto before = region.code.size();
      const auto maximum = region.MaxEmissionWords(op);
      assert(region.Emit(op, guest_pc));
      assert(region.code.size() - before <= maximum);
      Reference(op, expected, guest_pc);
      if (pc) guest_pc+=2;
      if (boundaries && (random() & 7) == 0) region.Flush();
    }
    const auto size = region.code.size();
    assert(!region.Emit(0x000b)); // RTS must be handled by control-flow lowering.
    assert(region.code.size() == size && region.instructions == ops.size());
    region.Finish();
    expected[22] += unsigned(ops.size()) * 2;
    expected[23] += unsigned(ops.size());
    for (auto op : ops) expected[23] += sh2a9::RegisterRegion::Cycles(op) - 1;
    std::vector<uint32_t> function = {0xe92d4ff8u, 0xe1a07000u,
      0xe5978058u, 0xe597905cu}; // save r4-r10/lr; r7=state; load PC/cycles
    function.insert(function.end(), region.code.begin(), region.code.end());
    function.push_back(0xe5878058u);
    function.push_back(0xe587905cu);
    function.push_back(0xe8bd8ff8u); // restore r4-r10/pc, ARM/Thumb interworking
    assert(function.size() * 4 <= capacity);
    std::memcpy(memory, function.data(), function.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
      reinterpret_cast<char *>(memory) + function.size() * 4);
    reinterpret_cast<void (*)(uint32_t *)>(memory)(actual);
    assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
    bool has_load = false;
    for (auto op : ops)
      has_load |= ((op & 0xf00f) >= 0x6000 && (op & 0xf00f) <= 0x6002) || sh2a9::RegisterRegion::IsPcLoad(op);
    if (has_load) {
      // Re-execute the SAME generated body with changed RAM. Only the address
      // is a constant: folding the fetched data would be a correctness bug.
      for (unsigned offset : {0u, 4u, 0xffffcu}) {
        low_ram[offset] ^= 0xff;
        high_ram[offset] ^= 0xff;
      }
      if (pc) {
        actual[22]=expected[22]=pc;
        guest_pc=pc;
        for (auto op : ops) {
          if(sh2a9::RegisterRegion::IsPcLoad(op)) {
            const auto address=sh2a9::RegisterRegion::PcLoadAddress(op,guest_pc);
            auto &ram=(address>>20)==2 ? low_ram : high_ram;
            ram[(address&0xfffff)^1]^=0xff;
          }
          guest_pc+=2;
        }
      }
      guest_pc=pc;
      for (auto op : ops) { Reference(op, expected, guest_pc); if(pc) guest_pc+=2; }
      expected[22] += unsigned(ops.size()) * 2;
      expected[23] += unsigned(ops.size());
      for (auto op : ops) expected[23] += sh2a9::RegisterRegion::Cycles(op) - 1;
      reinterpret_cast<void (*)(uint32_t *)>(memory)(actual);
      assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
    }
    ++cases;
  };
  for (auto op : supported)
      for (unsigned edge = 0; edge < 5; ++edge) test({op}, false, edge);
  {
    sh2a9::RegisterRegion logic;
    for(unsigned i=0;i<32;++i) assert(logic.Emit(uint16_t(0xc900|i)));
    logic.Finish();
    unsigned loads=0,stores=0;
    for(uint32_t word : logic.code) {
      loads += (word & 0xffff0fffu) == 0xe5970000u;
      stores += (word & 0xffff0fffu) == 0xe5870000u;
    }
    assert(loads==1 && stores==1); // R0 remains resident across the chain.
  }
  for (unsigned imm=0;imm<256;++imm) {
    // Known R0, runtime R0, dirty T, eviction, and flush boundaries.
    test({0xe080,uint16_t(0xc900|imm),uint16_t(0xca00|imm),
          uint16_t(0xcb00|imm),0x0029},false);
    test({0x0018,uint16_t(0xc900|imm),0x7001,uint16_t(0xcb00|imm),
          0x4008,uint16_t(0xca00|imm),0x0029},true);
  }
  for(uint32_t bank : {0x00200000u,0x06000000u})
    for(unsigned alignment : {0u,2u}) for(unsigned kind : {9u,13u})
      for(unsigned n=0;n<16;++n) for(unsigned disp : {0u,1u,127u,255u}) {
        const uint16_t op=uint16_t((kind<<12)|(n<<8)|disp);
        test({op},false,0,bank+alignment);
        std::vector<uint16_t> mixed{0x18};
        for(unsigned r=0;r<16;++r) mixed.push_back(uint16_t(0x7001|(r<<8)));
        mixed.push_back(op); mixed.push_back(uint16_t(0x7001|(n<<8)));
        test(mixed,true,0,bank+alignment);
      }
  {
    sh2a9::RegisterRegion region(uint32_t(low_ram.data()),uint32_t(high_ram.data()));
    for(uint32_t pc : {0u,0x20200000u,0x26000000u,0xfffffe00u,0x06000001u})
      for(uint16_t op : {uint16_t(0x9000),uint16_t(0xd000)}) {
        assert(!region.Emit(op,pc)); assert(region.code.empty());
      }
    test({0xd000},false,0,0x060ffff8u);
    test({0x9000},false,0,0x002ffffau);
    assert(!region.Emit(0xd001,0x060ffff8u)); // destination leaves cached RAM
  }
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n) {
      std::vector<uint16_t> ops{uint16_t(0x0007 | n << 8 | m << 4)};
      for (unsigned r = 0; r < 16; ++r) ops.push_back(uint16_t(0x7001 | r << 8));
      ops.push_back(uint16_t(0x001a | n << 8));
      ops.push_back(uint16_t(0x0007 | m << 8 | n << 4));
      ops.push_back(uint16_t(0x001a | m << 8));
      ops.push_back(uint16_t(0x000a | n << 8));
      for (unsigned edge = 0; edge < 5; ++edge) {
        test(ops, false, edge);
        test(ops, true, edge);
      }
    }
  // MACH/MACL chains as in matrix code: CLRMAC constants, 64-bit products,
  // LDS/STS copies and XTRCT readback, under 16-GPR pressure and flushes.
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n) {
      const unsigned k = (n + 3) & 15;
      std::vector<uint16_t> ops{0x0028, uint16_t(0x000a | k << 8), uint16_t(0x300d | n << 8 | m << 4)};
      for (unsigned r = 0; r < 16; ++r) ops.push_back(uint16_t(0x7001 | r << 8));
      ops.insert(ops.end(), {uint16_t(0x000a | n << 8), uint16_t(0x001a | m << 8),
                             uint16_t(0x200d | m << 8 | n << 4), uint16_t(0x3005 | m << 8 | n << 4),
                             uint16_t(0x200f | n << 8 | m << 4), uint16_t(0x001a | k << 8),
                             uint16_t(0x200e | m << 8 | k << 4), uint16_t(0x400a | n << 8),
                             uint16_t(0x401a | m << 8), 0x0028, uint16_t(0x001a | n << 8),
                             uint16_t(0x200d | n << 8 | n << 4), uint16_t(0x000a | m << 8)});
      for (unsigned edge = 0; edge < 5; ++edge) {
        test(ops, false, edge);
        test(ops, true, edge);
      }
    }
  // Explicit T=0/1, aliases, edge operands, repeated carry chains and mixed
  // SR observers. Include flushes to exercise both resident and reloaded T.
  for (unsigned n = 0; n < 16; ++n)
    for (unsigned m = 0; m < 16; ++m)
      for (unsigned t = 0; t < 2; ++t)
        for (unsigned edge = 1; edge < 5; ++edge) {
          std::vector<uint16_t> ops{uint16_t(t ? 0x18 : 8)};
          for (unsigned i = 0; i < 32; ++i) {
            ops.push_back(uint16_t((i & 1 ? 0x300a : 0x300e) | n << 8 | m << 4));
            if ((i & 7) == 7) ops.push_back(uint16_t(0x0029 | ((n + 1) & 15) << 8));
          }
          test(ops, false, edge);
          test(ops, true, edge);
        }
  auto address_ops = [](unsigned r, uint32_t value) {
    std::vector<uint16_t> ops{uint16_t(0xe000 | (r << 8))};
    for (int shift = 30; shift >= 0; shift -= 2) {
      ops.push_back(uint16_t(0x4008 | (r << 8)));
      ops.push_back(uint16_t(0x7000 | (r << 8) | ((value >> shift) & 3)));
    }
    return ops;
  };
  for (uint32_t base : {0x00200000u, 0x06000000u})
    for (unsigned n = 0; n < 16; ++n)
      for (unsigned m = 0; m < 16; ++m)
        for (unsigned kind = 0; kind < 3; ++kind)
          for (uint32_t offset : {0u, 4u, 0xffffcu}) {
            auto ops = address_ops(m, base + offset);
            // Keep unrelated dirty values live across the memory operation.
            ops.insert(ops.begin(), uint16_t(0x7001 | (((m + 1) & 15) << 8)));
            ops.push_back(uint16_t(0x6000 | (n << 8) | (m << 4) | kind));
            ops.push_back(uint16_t(0x7001 | (n << 8)));
            test(ops, false);
          }
  for (uint32_t address : {0x20200000u, 0x26000000u, 0x06100000u,
                          0x05f80000u, 0u, 0xffffffffu, 0x00200001u}) {
    sh2a9::RegisterRegion rejected(reinterpret_cast<uint32_t>(low_ram.data()),
                                 reinterpret_cast<uint32_t>(high_ram.data()));
    for (auto op : address_ops(0, address)) assert(rejected.Emit(op));
    const auto code = rejected.code;
    const unsigned count = rejected.instructions;
    assert(!rejected.CanEmit(0x6102));
    assert(!rejected.Emit(0x6102));
    assert(rejected.code == code && rejected.instructions == count);
  }
  // Force every admitted operation through constant propagation, including
  // aliasing and eviction/materialization when the next operand is unknown.
  for (auto op : supported) {
    std::vector<uint16_t> ops;
    for (unsigned i = 0; i < 16; ++i)
      ops.push_back(uint16_t(0xe000 | (i << 8) | (0x80 + i)));
    ops.push_back(op);
    test(ops, false);
    test(ops, true);
  }
  for (unsigned run = 0; run < 1000; ++run) {
    std::vector<uint16_t> ops;
    for (unsigned i = 0; i < 256; ++i) {
      ops.push_back((random() & 3) == 0 ?
        uint16_t(0xe000 | (random() & 0xfff)) :
        supported[random() % supported.size()]);
    }
    test(ops, run & 1);
  }
  for (unsigned run = 0; run < 2000; ++run) {
    std::vector<uint16_t> ops;
    for (unsigned i = 0; i < 256; ++i) ops.push_back(supported[random() % supported.size()]);
    test(ops, run & 1);
  }
  // Exit writeback must preserve untouched canonical words. Exercise every
  // starting guest register, clean host-slot gaps, reversed mappings and
  // pressure/eviction; the execution oracle checks the entire canonical array.
  for (unsigned first = 0; first < 16; ++first) {
    std::vector<uint16_t> stores;
    for (unsigned r = first; r < 16; ++r)
      stores.push_back(uint16_t(0x7001 | r << 8));
    test(stores, false);
    std::reverse(stores.begin(), stores.end());
    test(stores, false);
  }
  test({0x7001, 0x3120, 0x7101, 0x7201}, false); // clean mapping amid dirty slots
  test({0x7001, 0x7201, 0x7401}, false); // guest gaps must remain untouched
  sh2a9::RegisterRegion compact;
  for (unsigned i = 0; i < 32; ++i) assert(compact.Emit(0x301c)); // add r1,r0
  compact.Flush();
  assert(compact.code.size() == 35); // two initial loads, 32 adds, one final store
  sh2a9::RegisterRegion folded;
  assert(folded.Emit(0xe001)); // mov #1,r0
  for (unsigned i = 0; i < 32; ++i) assert(folded.Emit(0x7001));
  assert(folded.code.empty());
  folded.Flush();
  assert(folded.code.size() == 2); // mov #33,r0; canonical store
  sh2a9::RegisterRegion overwritten;
  assert(overwritten.Emit(0x301c)); // dirty runtime result
  assert(overwritten.Emit(0xe080)); // overwritten before any observer
  overwritten.Flush();
  test({0x301c, 0xe080, 0x6013, 0x7001, 0x6103}, false);
  test({0xe001, 0x4010, 0x0129, 0x4010, 0x0229, 0x4011, 0x0329}, false);
  test({0x3013, 0x0029, 0x3012, 0x0129, 0x4015, 0x0229}, false, 1);
  // Repeated DIV1 must retain operands without flushing between quotient bits.
  sh2a9::RegisterRegion division_flags;
  for (unsigned i = 0; i < 32; ++i) assert(division_flags.Emit(0x3104));
  const auto pending_code = division_flags.code;
  assert(!division_flags.Emit(0xffff));
  assert(division_flags.code == pending_code); // Failed admission cannot publish.
  division_flags.Flush();
  unsigned sr_loads = 0, sr_stores = 0;
  for (auto word : division_flags.code) {
    sr_loads += word == 0xe597a040u;
    sr_stores += word == 0xe587a040u;
  }
  // A DIV1 chain's fast path keeps SR; only its cold generic path reloads it.
  assert(sr_loads == 1 && sr_stores == 1);
  std::printf("A9 DIV1 x32: %zu words including flush\n", division_flags.code.size());
  for (uint16_t op : {uint16_t(0x310e), uint16_t(0x310a)}) {
    sh2a9::RegisterRegion carry;
    for (unsigned i = 0; i < 32; ++i) assert(carry.Emit(op));
    carry.Flush();
    unsigned loads = 0, stores = 0;
    for (auto word : carry.code) {
      loads += word == 0xe597a040u;
      stores += word == 0xe587a040u;
    }
    assert(loads == 1 && stores == 1);
    assert(carry.code.size() <= ((op & 15) == 14 ? 134u : 166u));
    std::printf("A9 carry chain %04x: %zu words for 32 guest instructions including flush\n",
      op, carry.code.size());
  }
  const auto flushed_size = division_flags.code.size();
  division_flags.Flush();
  assert(division_flags.code.size() == flushed_size);
  test({0x3104, 0x3104, 0x0029, 0x0018, 0x3104, 0x0129}, false);
  test({0x3104, 0x3104, 0x0029, 0x0018, 0x3104, 0x0129}, true);
  // Mix dirty unrelated registers to exercise eviction and aliasing pressure.
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n) {
      std::vector<uint16_t> division;
      for (unsigned g = 0; g < 16; ++g) division.push_back(0x7001 | (g << 8));
      division.insert(division.end(), 32, uint16_t(0x3004 | (n << 8) | (m << 4)));
      for (unsigned edge = 0; edge < 5; ++edge) test(division, false, edge);
      for (unsigned low : {m, n, (n + 1) & 15}) {
        std::vector<uint16_t> wide_division;
        for (unsigned i = 0; i < 32; ++i) {
          wide_division.push_back(0x4024 | (low << 8));
          wide_division.push_back(0x3004 | (n << 8) | (m << 4));
        }
        for (unsigned edge = 0; edge < 5; ++edge) test(wide_division, false, edge);
      }
    }
  // DIV1 chains: random ROTCL/DIV1 mixes (any M/Q/T, aliased operands),
  // interrupted by other T writers, including the udivsi3 shape (after DIV0U).
  for (unsigned run = 0; run < 4000; ++run) {
    std::vector<uint16_t> ops;
    const unsigned q = random() & 15, n = random() & 15, m = random() & 15;
    if (run & 1) {
      for (unsigned i = 0, k = random() % 40; i < k; ++i) {
        ops.push_back(0x4024 | (q << 8));
        ops.push_back(0x3004 | (n << 8) | (m << 4));
      }
      if (random() & 1) ops.push_back(0x4024 | (q << 8));
    } else {
      for (unsigned i = 0, k = 1 + random() % 70; i < k; ++i) {
        const unsigned c = random() % 16;
        const unsigned a = random() & 15, b = random() & 15;
        ops.push_back(c < 7 ? uint16_t(0x3004 | (n << 8) | (m << 4)) :
                      c < 11 ? uint16_t(0x4024 | (q << 8)) :
                      c == 11 ? uint16_t(0x3004 | (a << 8) | (b << 4)) :
                      c == 12 ? uint16_t(0x300e | (a << 8) | (b << 4)) :  // ADDC
                      c == 13 ? uint16_t(0x0008 | ((random() & 1) << 4)) : // CLRT/SETT
                      c == 14 ? uint16_t(0x4000 | (a << 8) | ((random() & 1) << 5)) : // SHLL/SHLR
                      supported[random() % supported.size()]);
      }
    }
    for (unsigned edge = 0; edge < 5; edge += 2) test(ops, run & 2, edge);
  }
  // DIV1 chains whose operands take the M=0 in-range path (1 <= Rm <= 2^31,
  // -Rm <= Rn < Rm), including the bounds, over more than 32 ROTCLs.
  for (unsigned run = 0; run < 4000; ++run) {
    std::vector<uint16_t> ops;
    unsigned q = random() & 15, n = random() & 15, m = random() & 15;
    while (m == n) m = random() & 15;
    while (q == n || q == m) q = random() & 15;
    if (random() & 1) ops.push_back(0x4024 | (q << 8));
    for (unsigned i = 0, k = 1 + random() % 40; i < k; ++i) {
      ops.push_back(0x3004 | (n << 8) | (m << 4));
      if (random() % 8) ops.push_back(0x4024 | (q << 8));
    }
    const uint32_t d = run % 5 == 0 ? 0x80000000u : run % 5 == 1 ? 1 : 1 + random() % 0x80000000u;
    const uint32_t kind = random() % 4;
    const uint32_t p = kind == 0 ? -d : kind == 1 ? d - 1 : uint32_t(-int64_t(d) + int64_t(random() % (2ull * d)));
    test(ops, run & 2, 0, 0, [&](uint32_t *state) {
      state[m] = d; state[n] = p;
      state[16] &= ~0x200u;                      // M = 0; Q/T random
    });
  }
  unsigned loop_cases = 0;
  unsigned interpreter_cases = 0;
  auto loop_test = [&](const std::vector<uint16_t>& body, bool bt) {
    const uint16_t branch = (bt ? 0x8900 : 0x8b00) |
      uint8_t(-int(body.size()) - 2);
    const auto code = sh2a9::RegisterRegion::ResidentLoop(body, branch);
    assert(!code.empty());
    // The unconditional data-processing body and guards must not reload or
    // store guest GPRs on the back edge. Initial loads and exit stores are OK.
    for (size_t at = 0; at < code.size(); ++at) {
      if ((code[at] & 0xff000000u) != 0x2a000000u || !(code[at] & 0x00800000u))
        continue;
      const int displacement = int32_t(code[at] << 8) >> 8;
      const size_t head = size_t(int(at) + 2 + displacement);
      for (size_t inner = head; inner < at; ++inner) {
        const uint32_t word = code[inner];
        const bool state_access = (word & 0x0fef0000u) == 0x05870000u;
        assert(!state_access || (word & 0xfffu) >= 64);
      }
    }
    uint32_t actual[33], expected[33];
    for (auto &value : actual) value = random();
    actual[23] = random() % 20;
    actual[24] = (loop_cases % 3 == 0) ? 0xf0 : 0;
    actual[32] = actual[23] + random() % 1000;
    // Exercise both natural counted exits and deadline/interrupt exits.
    actual[0] = loop_cases % 32;
    std::memcpy(expected, actual, sizeof(actual));
    uint32_t oracle[33];
    std::memcpy(oracle, actual, sizeof(actual));
    unsigned iterations = 0;
    do {
      for (auto op : body) Reference(op, expected);
      expected[23] += body.size() + 1;
      const bool taken = bool(expected[16] & 1) == bt;
      if (!taken) { expected[22] += (body.size() + 1) * 2; break; }
      expected[23] += 2;
      if (expected[23] >= expected[32] ||
          (expected[16] & 0xf0) < expected[24]) break;
      assert(++iterations < 1000);
    } while (true);
    const bool decrement = !bt && body.size() == 1 && (body[0] & 0xf0ff) == 0x4010;
    const bool arithmetic = bt && body.size() == 2 &&
      (body[0] & 0xf0ff) == 0x70ff && (body[1] & 0xf0ff) == 0x4011 &&
      (body[0] & 0xf00) == (body[1] & 0xf00);
    if (decrement || arithmetic) {
      sh2_counted_loop_oracle(arithmetic, (body[0] >> 8) & 15, oracle);
      assert(std::memcmp(oracle, expected, sizeof(oracle)) == 0);
      ++interpreter_cases;
    }
    std::vector<uint32_t> function = {0xe92d4ff8u, 0xe1a07000u,
      0xe5978058u, 0xe597905cu};
    function.insert(function.end(), code.begin(), code.end());
    function.insert(function.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
    assert(function.size() * 4 <= capacity);
    std::memcpy(memory, function.data(), function.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(memory),
      reinterpret_cast<char *>(memory) + function.size() * 4);
    reinterpret_cast<void (*)(uint32_t *)>(memory)(actual);
    assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
    ++loop_cases;
  };
  std::vector<uint16_t> loop_supported;
  for (auto op : supported) {
    if (sh2a9::RegisterRegion::IsDivisionStep(op) ||
        sh2a9::RegisterRegion::IsMacOperation(op)) {
      assert(sh2a9::RegisterRegion::ResidentLoop({op}, 0x8bfd).empty());
      continue;
    }
    loop_supported.push_back(op);
    loop_test({op}, false);
    loop_test({op}, true);
  }
  for (unsigned run = 0; run < 3000; ++run) {
    std::vector<uint16_t> body;
    for (unsigned i = 0, count = 1 + random() % 64; i < count; ++i) {
      uint16_t op = loop_supported[random() % loop_supported.size()];
      if (op != 8 && op != 9 && op != 0x18 &&
          (op >> 8) != 0x88 && (op >> 8) != 0xc8 &&
          !sh2a9::RegisterRegion::IsImmediateLogic(op))
        op = (op & 0xf0ff) | (((op >> 8) & 15) % 7) << 8;
      if ((op >> 12) == 2 || (op >> 12) == 3 || (op >> 12) == 6)
        op = (op & 0xff0f) | (((op >> 4) & 15) % 7) << 4;
      body.push_back(op);
    }
    loop_test(body, run & 1);
    loop_test({0x4010}, false);
    const unsigned r = run & 15;
    loop_test({uint16_t(0x70ff | (r << 8)), uint16_t(0x4011 | (r << 8))}, true);
  }
  assert(sh2a9::RegisterRegion::ResidentLoop({0x6002}, 0x8bfd).empty());
  assert(sh2a9::RegisterRegion::ResidentLoop({9}, 0x8ffc).empty());
  assert(sh2a9::RegisterRegion::ResidentLoop({9}, 0x8bfc).empty());
  assert(sh2a9::RegisterRegion::ResidentLoop(
    {0x7001,0x7101,0x7201,0x7301,0x7401,0x7501,0x7601,0x7701},0x8bf6).empty());
  std::printf("A9 resident loops: %u cases, %u actual-interpreter comparisons, both branch outcomes, deadlines and IRQ exits\n", loop_cases, interpreter_cases);
  std::printf("A9 register regions: %u cases; 768000 randomized guest instructions; constant chain 2 words; repeated ADD 35 words (excluding PC/cycles)\n", cases);
  munmap(memory, capacity);
}
