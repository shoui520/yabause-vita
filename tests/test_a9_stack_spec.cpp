/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/a9_register_region.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>

// Stack (R15) speculation inside register regions. The caller (the compiler)
// has validated R15 at block entry and put its host address in r11; the test
// wrapper does the same. The plan gives R15's offset from its entry value
// before each instruction, derived with RegisterRegion::R15Effect exactly as
// the compiler does. Random stack-heavy sequences (pushes/pops of all widths,
// displacement loads/stores, R0 forms, ADD #imm,R15, ALU work, guarded
// non-stack accesses, and occasional untracked R15 writes that end the plan)
// are checked against an interpreter on a T2-layout memory model.
constexpr unsigned size = 1u << 20;
static std::vector<uint8_t> low(size), high(size), pages(1024);
static unsigned helper_calls;
static uint32_t HelperRead(uint32_t a, unsigned w) {
  ++helper_calls;
  const bool hi = (a & 0xdff00000u) == 0x06000000u, lo = (a & 0xdff00000u) == 0x00200000u;
  if (!hi && !lo) return a ^ 0x5a5a5a5au;
  const auto &ram = hi ? high : low;
  uint32_t v = 0;
  for (unsigned i = 0; i < w; ++i) v = (v << 8) | ram[((a & (size - 1)) + i) ^ 1];
  return v;
}
static void HelperWrite(uint32_t a, uint32_t v, unsigned w) {
  ++helper_calls;
  const bool hi = (a & 0xdff00000u) == 0x06000000u, lo = (a & 0xdff00000u) == 0x00200000u;
  if (!hi && !lo) return;
  auto &ram = hi ? high : low;
  for (unsigned i = 0; i < w; ++i) ram[((a & (size - 1)) + i) ^ 1] = uint8_t(v >> (8 * (w - 1 - i)));
}
static uint32_t RB(uint32_t a) { return HelperRead(a, 1) & 255; }
static uint32_t RW(uint32_t a) { return HelperRead(a, 2) & 65535; }
static uint32_t RL(uint32_t a) { return HelperRead(a, 4); }
static void WB(uint32_t a, uint32_t v) { HelperWrite(a, v & 255, 1); }
static void WW(uint32_t a, uint32_t v) { HelperWrite(a, v & 65535, 2); }
static void WL(uint32_t a, uint32_t v) { HelperWrite(a, v, 4); }

struct Plan { std::vector<int> off; std::vector<uint8_t> valid; uint32_t start; };
static bool Lookup(void *ctx, uint32_t pc, int *off) {
  auto *p = static_cast<Plan *>(ctx);
  const uint32_t i = (pc - p->start) >> 1;
  if (i >= p->valid.size() || !p->valid[i]) return false;
  *off = p->off[i];
  return true;
}
static uint32_t Mem(std::vector<uint8_t> &m, uint32_t a, unsigned w) {
  uint32_t v = 0;
  for (unsigned i = 0; i < w; ++i) v = (v << 8) | m[((a & (size - 1)) + i) ^ 1];
  return v;
}
static void SetMem(std::vector<uint8_t> &m, uint32_t a, unsigned w, uint32_t v) {
  for (unsigned i = 0; i < w; ++i) m[((a & (size - 1)) + i) ^ 1] = uint8_t(v >> (8 * (w - 1 - i)));
}
static uint32_t Sext(uint32_t v, unsigned w) {
  return w == 1 ? uint32_t(int32_t(int8_t(v))) : w == 2 ? uint32_t(int32_t(int16_t(v))) : v;
}

int main() {
  sh2a9::RegisterRegion::stack_spec_enabled = true;
  sh2a9::RegisterRegion::disp_loads_enabled = true;
  std::mt19937 random(0x5354);
  for (auto &v : low) v = random();
  for (auto &v : high) v = random();
  auto *code = static_cast<uint32_t *>(mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(code != MAP_FAILED);
  unsigned cases = 0, spec_ops = 0;
  for (unsigned run = 0; run < 3000; ++run) {
    uint32_t state[40], expected[40];
    for (auto &v : state) v = random();
    // R15 validated by the caller: [0x06000400, 0x060FC400), 4-aligned.
    const uint32_t r15 = 0x06000400u + 4 * (random() % ((0xFC000u - 0x400u) / 4 - 300)) + 1024;
    state[15] = r15;
    for (unsigned r = 1; r < 15; ++r)
      if (random() % 3 == 0) state[r] = 0x06000000u + 4 * (random() % 0x3ff00);  // guarded bases
    state[25] = reinterpret_cast<uint32_t>(&RB); state[26] = reinterpret_cast<uint32_t>(&RW);
    state[27] = reinterpret_cast<uint32_t>(&RL); state[28] = reinterpret_cast<uint32_t>(&WB);
    state[29] = reinterpret_cast<uint32_t>(&WW); state[30] = reinterpret_cast<uint32_t>(&WL);
    state[34] = reinterpret_cast<uint32_t>(high.data()) + (r15 - 0x06000000u); // r11
    std::memcpy(expected, state, sizeof(state));
    auto model = high, low_model = low;
    Plan plan; plan.start = 0x06001000u;
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    region.code_pages = reinterpret_cast<uint32_t>(pages.data());
    region.spec_plan = Lookup; region.spec_ctx = &plan;
    int off = 0; bool valid = true;
    const unsigned steps = 1 + random() % 40;
    std::vector<uint16_t> ops;
    for (unsigned k = 0; k < steps; ++k) {
      const unsigned kind = random() % 12, a = 1 + random() % 6, b = 1 + random() % 6, d = random() % 16;
      uint16_t op;
      switch (kind) {
      case 0: op = uint16_t(0x2f04 | (a << 4) | (random() % 3)); break;          // MOV.x Ra,@-R15
      case 1: op = uint16_t(0x60f4 | (a << 8) | (random() % 3)); break;          // MOV.x @R15+,Ra
      case 2: op = uint16_t(0x50f0 | (a << 8) | d); break;                       // MOV.L @(d,R15),Ra
      case 3: op = uint16_t(0x1f00 | (a << 4) | d); break;                       // MOV.L Ra,@(d,R15)
      case 4: op = uint16_t((random() & 1 ? 0x85f0 : 0x84f0) | d); break;         // MOV.W/B @(d,R15),R0
      case 5: op = uint16_t((random() & 1 ? 0x81f0 : 0x80f0) | d); break;         // MOV.W/B R0,@(d,R15)
      case 6: op = uint16_t(0x7f00 | uint8_t(int8_t(4 * (int(random() % 9) - 4)))); break; // ADD #imm,R15
      case 7: op = uint16_t(0x300c | (a << 8) | (b << 4)); break;                // ADD Rb,Ra
      case 8: op = uint16_t(0x6002 | (a << 8) | (b << 4)); break;                // MOV.L @Rb,Ra (guarded)
      case 9: op = uint16_t(0x2002 | (a << 8) | (b << 4)); break;                // MOV.L Rb,@Ra (guarded)
      case 10: op = uint16_t(0x62f2 | (a << 8)); break;                          // MOV.L @R15,Ra
      default: op = random() % 8 == 0 ? uint16_t(0x6f03 | (a << 4)) : uint16_t(0x0009); // MOV Ra,R15 / NOP
      }
      if (op == uint16_t(0x6f03 | (a << 4)) && (expected[a] & 3 || (expected[a] ^ 0x06000000u) >= 0xF0000u ||
          (expected[a] ^ 0x06000000u) < 0x10000u)) op = 0x0009; // keep R15 in RAM
      plan.valid.push_back(valid); plan.off.push_back(off);
      // Oracle: execute op on expected/model (helper semantics: RAM, else a^K).
      uint32_t *r = expected;
      const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
      auto load = [&](uint32_t addr, unsigned w) -> uint32_t {
        if ((addr & 0xdff00000u) == 0x06000000u) return Sext(Mem(model, addr, w), w);
        if ((addr & 0xdff00000u) == 0x00200000u) return Sext(Mem(low_model, addr, w), w);
        const uint32_t v = addr ^ 0x5a5a5a5au;
        return Sext(w == 4 ? v : v & ((1u << (8 * w)) - 1), w);
      };
      auto store = [&](uint32_t addr, unsigned w, uint32_t v) {
        if ((addr & 0xdff00000u) == 0x06000000u) SetMem(model, addr, w, v);
        else if ((addr & 0xdff00000u) == 0x00200000u) SetMem(low_model, addr, w, v);
      };
      int delta; const int effect = sh2a9::RegisterRegion::R15Effect(op, &delta);
      if ((op & 0xff0f) >= 0x2f04 && (op & 0xff0f) <= 0x2f06) {
        const unsigned w = 1u << (op & 3); const uint32_t v = r[m];
        r[15] -= w; store(r[15], w, v);
      } else if ((op & 0xf0ff) >= 0x60f4 && (op & 0xf0ff) <= 0x60f6) {
        const unsigned w = 1u << (op & 3);
        r[n] = load(r[15], w); r[15] += w;
      } else if ((op & 0xf0f0) == 0x50f0) r[n] = load(r[15] + (op & 15) * 4, 4);
      else if ((op & 0xff00) == 0x1f00) store(r[15] + (op & 15) * 4, 4, r[m]);
      else if ((op & 0xfff0) == 0x85f0) r[0] = load(r[15] + (op & 15) * 2, 2);
      else if ((op & 0xfff0) == 0x84f0) r[0] = load(r[15] + (op & 15), 1);
      else if ((op & 0xfff0) == 0x81f0) store(r[15] + (op & 15) * 2, 2, r[0]);
      else if ((op & 0xfff0) == 0x80f0) store(r[15] + (op & 15), 1, r[0]);
      else if ((op & 0xff00) == 0x7f00) r[15] += uint32_t(int32_t(int8_t(op & 0xff)));
      else if ((op & 0xf00f) == 0x300c) r[n] += r[m];
      else if ((op & 0xf00f) == 0x6002) r[n] = load(r[m], 4);
      else if ((op & 0xf00f) == 0x2002) store(r[n], 4, r[m]);
      else if ((op & 0xf0ff) == 0x62f2) r[n] = load(r[15], 4);
      else if ((op & 0xff0f) == 0x6f03) r[15] = r[m];
      if (effect == 1) { off += delta; if (off <= -512 || off >= 512) valid = false; }
      else if (effect == 2) valid = false;
      ops.push_back(op);
      const auto before = region.code.size();
      const unsigned maximum = region.MaxEmissionWords(op);
      int po;
      if (Lookup(&plan, plan.start + 2 * k, &po) && sh2a9::RegisterRegion::SpecDecode(op).ok) ++spec_ops;
      assert(region.Emit(op, plan.start + 2 * k));
      assert(region.code.size() - before <= maximum);
      (void)before;
    }
    region.Finish();
    std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu, 0xe597b088u};
    fn.insert(fn.end(), region.code.begin(), region.code.end());
    fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
    assert(fn.size() * 4 <= (1u << 16));
    std::memcpy(code, fn.data(), fn.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + fn.size() * 4);
    const uint32_t pc0 = state[22], cy0 = state[23];
    reinterpret_cast<void (*)(uint32_t *)>(code)(state);
    expected[22] = pc0 + region.instructions * 2;
    expected[23] = cy0 + region.instructions;
    if (std::memcmp(state, expected, 24 * 4) || high != model || low != low_model) {
      for (unsigned i = 0; i < 24; ++i)
        if (state[i] != expected[i]) std::fprintf(stderr, "run %u r%u %08x want %08x\n", run, i, state[i], expected[i]);
      for (unsigned k = 0; k < ops.size(); ++k) std::fprintf(stderr, "%04x ", ops[k]);
      std::fprintf(stderr, "\n");
      return 1;
    }
    high = model;  // keep both in step for the next run
    ++cases;
  }
  std::printf("A9 stack speculation: %u sequences, %u speculative accesses, all identical to the interpreter\n",
              cases, spec_ops);
  return spec_ops ? 0 : 1;
}
