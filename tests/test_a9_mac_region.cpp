/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test of region MAC.L and MAC.W @Rm+,@Rn+ against the actual
 * MAC_L/MAC_W templates (dynalib_arm.s, VITA_SH2_MACL_WRAM/MACW_WRAM). Each
 * sequence surrounds the MAC with resident dirty GPRs and MACH/MACL (CLRMAC,
 * LDS, ADD) and STS readback. The reference runs the template for the MAC
 * and C semantics for the register operations; the region's slow edge
 * calls sh2_macl_region/sh2_macw_region. Final state, every callback
 * address, the canonical state each callback observes and its live
 * PC/cycles (r8/r9) must match exactly. Callbacks also mutate canonical R15,
 * MACL and the S bit, which both lowerings must re-read. Operands cover
 * cached high and low work RAM (aligned and not), their cache-through
 * aliases, devices, S=0/1 and MACH near the saturation bounds. The callback
 * returns work RAM contents like memGetLong/memGetWord, so the region's
 * inline low work RAM reads are checked against the template's callbacks. MAC.W sequences
 * that can reach S=1 use operands small enough never to saturate: the
 * template's MAC.W overflow tail dereferences its 64-bit sum's high word. */
#include "../src/core/sh2_dynarec/a9_register_region.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

extern "C" {
extern unsigned char prologue[], epilogue[];
extern unsigned char x86_MAC_L[], x86_MAC_W[];
extern const unsigned short MAC_L_size, MAC_W_size;
extern const unsigned char MAC_L_src, MAC_L_dest, MAC_W_src, MAC_W_dest;
void sh2_macl_saturate(void);
void sh2_macl_region(void);
void sh2_macw_saturate(void);
void sh2_macw_region(void);
uint32_t get_long_tramp(uint32_t);
uint32_t get_long_c(uint32_t a, uint32_t r8, uint32_t r9);
}
// Captures the live PC/cycle registers for the C callback.
asm(".text\n.arm\n.align 2\n.global get_long_tramp\n.type get_long_tramp,%function\n"
    "get_long_tramp:\n\tmov r1, r8\n\tmov r2, r9\n\tb get_long_c\n"
    ".size get_long_tramp, .-get_long_tramp\n");

static uint8_t full[0x100000 + 16] __attribute__((aligned(4)));
static uint8_t small[0x100000 + 16] __attribute__((aligned(4)));  // halfwords in [-2^13, 2^13)
static uint8_t low_full[0x100000 + 16] __attribute__((aligned(4)));
static uint8_t low_small[0x100000 + 16] __attribute__((aligned(4)));
static uint8_t *high, *low;
static uint32_t *live;
struct Call { uint32_t address, r8, r9; uint32_t state[22]; };
static std::vector<Call> calls;
static bool mutate, word, bounded;

// memGetLong for MAC.L, memGetWord for MAC.W (the same trampoline in both slots).
extern "C" uint32_t get_long_c(uint32_t a, uint32_t r8, uint32_t r9) {
  // Cached low work RAM (0x002xxxxx) is a plain T2 read in memGetLong/Word:
  // no cycles, no other effect, so it is neither recorded nor mutating.
  const bool pure = (a & 0xfff00000u) == 0x00200000u;
  if (!pure) {
    Call c{a, r8, r9, {}};
    std::memcpy(c.state, live, sizeof(c.state));
    calls.push_back(c);
  }
  if (mutate && !pure) {
    live[15] ^= 0x76543210u; live[16] ^= 2;
    live[20] = word ? uint32_t(int32_t(live[20] ^ 0x13579bdfu) >> 4) : live[20] ^ 0x13579bdfu;
  }
  if ((a & 0xdff00000u) == 0x06000000u || (a & 0xdff00000u) == 0x00200000u) {
    uint32_t w; std::memcpy(&w, ((a & 0xdff00000u) == 0x06000000u ? high : low) + (a & 0xfffff), 4);
    return word ? w & 0xffff : w >> 16 | w << 16;
  }
  const uint32_t h = a * 0x9e3779b1u ^ (a >> 13);
  return bounded ? uint32_t(int32_t(h << 18) >> 18) : h;
}

static uint32_t rng = 0x4d41434cu;
static uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t address() {
  static const uint32_t bases[] = {0x06000000u, 0x060ffff0u, 0x26000000u, 0x00200000u,
                                   0x05a00000u, 0x06040000u, 0x06100000u, 0x06080000u,
                                   0x002ffff0u, 0x20200000u, 0x00240000u, 0x06000000u};
  const uint32_t a = bases[next() % 12] + (next() & 0xffc);
  return (next() & 7) == 0 ? a + 1 + (next() & 2) : a;  // misaligned now and then
}

// State words: R0-15, SR 16, GBR, VBR, MACH 19, MACL 20, PR, PC 22, count 23,
// helpers #108 (27), high base #136 (34), saturation #168 (42).
static void Simple(uint16_t op, uint32_t *r) {
  const unsigned n = (op >> 8) & 15;
  if (op == 0x0028) r[19] = r[20] = 0;
  else if ((op & 0xf0ff) == 0x000a) r[n] = r[19];
  else if ((op & 0xf0ff) == 0x001a) r[n] = r[20];
  else if ((op & 0xf0ff) == 0x400a) r[19] = r[n];
  else if ((op & 0xf0ff) == 0x401a) r[20] = r[n];
  else if ((op >> 12) == 7) r[n] += uint32_t(int32_t(int8_t(op)));
  else if ((op >> 12) == 0xe) r[n] = uint32_t(int32_t(int8_t(op)));
  else assert(false);
}

int main() {
  for (unsigned i = 0; i < sizeof(full); ++i) full[i] = uint8_t(next());
  for (unsigned i = 0; i < sizeof(small); i += 2) {
    const uint16_t h = uint16_t(int16_t(int32_t(next() << 18) >> 18));
    std::memcpy(small + i, &h, 2);
    const uint16_t l = uint16_t(int16_t(int32_t(next() << 18) >> 18));
    std::memcpy(low_small + i, &l, 2);
  }
  for (unsigned i = 0; i < sizeof(low_full); ++i) low_full[i] = uint8_t(next());
  auto *code = static_cast<uint8_t *>(mmap(nullptr, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(code != MAP_FAILED);
  assert(MAC_L_size == 52 * 4 && MAC_W_size == 42 * 4);
  auto run = [&](const void *fn, size_t bytes, uint32_t *state) {
    std::memcpy(code, fn, bytes);
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + bytes);
    live = state;
    reinterpret_cast<void (*)(uint32_t *)>(code)(state);
  };
  unsigned cases[2] = {}, slow[2] = {}, saturated[2] = {};
  for (bool w : {false, true})
  for (unsigned base : {0u, 1u, 2u}) {              // r11 base, ip base (stack spec), ip base
    sh2a9::RegisterRegion::base_reg_enabled = base == 0;
    sh2a9::RegisterRegion::stack_spec_enabled = base == 1;
    for (unsigned m = 0; m < 16; ++m)
      for (unsigned n = 0; n < 16; ++n)
        for (unsigned it = 0; it < 24; ++it) {
          const unsigned k = (n + 1 + it) & 15, j = (m + 7 * it) & 15;
          std::vector<uint16_t> ops;
          ops.push_back(uint16_t(0x7001 | k << 8));
          if (it % 3 == 0) ops.push_back(0x0028);                       // known MACH/MACL
          if (it % 3 == 1) ops.push_back(uint16_t(0x401a | j << 8));   // resident dirty MACL
          if (it & 4) ops.push_back(uint16_t(0xe000 | j << 8 | (it * 37 & 255))); // known GPR
          word = w;
          const uint16_t mac = uint16_t((w ? 0x400f : 0x000f) | n << 8 | m << 4);
          ops.push_back(mac);
          if (it & 8) ops.push_back(mac);
          ops.push_back(uint16_t(0x7005 | j << 8));
          ops.push_back(uint16_t(0x001a | k << 8));
          ops.push_back(uint16_t(0x000a | j << 8));

          uint32_t start[48];
          for (auto &v : start) v = next();
          start[m] = address();
          start[n] = m == n ? start[m] : address();
          start[16] = (start[16] & ~2u) | ((it & 2) ? 2u : 0u);
          if (it & 16) start[19] = uint32_t(int32_t(next()) >> 16);   // MACH near the bounds
          mutate = (it % 5) == 4;
          // MAC.W that may reach S=1: MACL, LDS sources and operands stay small.
          bounded = w && ((it & 2) || mutate);
          high = bounded ? small : full;
          low = bounded ? low_small : low_full;
          if (bounded) {
            start[20] = uint32_t(int32_t(start[20]) >> 4);
            if (j != m && j != n) start[j] = uint32_t(int32_t(start[j]) >> 4);
          }
          start[26] = start[27] = reinterpret_cast<uint32_t>(&get_long_tramp);
          start[34] = reinterpret_cast<uint32_t>(high);
          start[41] = reinterpret_cast<uint32_t>(&sh2_macw_saturate);
          start[42] = reinterpret_cast<uint32_t>(&sh2_macl_saturate);

          // Reference: C for register operations, the real template for MAC.L.
          uint32_t expected[48];
          std::memcpy(expected, start, sizeof(expected));
          calls.clear();
          const unsigned size = w ? MAC_W_size : MAC_L_size;
          std::vector<uint8_t> tfn(16 + size + 12);
          std::memcpy(tfn.data(), prologue, 16);
          std::memcpy(tfn.data() + 16, w ? x86_MAC_W : x86_MAC_L, size);
          tfn[16 + (w ? MAC_W_src : MAC_L_src)] = uint8_t(m * 4);
          tfn[16 + (w ? MAC_W_dest : MAC_L_dest)] = uint8_t(n * 4);
          std::memcpy(tfn.data() + 16 + size, epilogue, 12);
          for (auto op : ops) {
            if (op == mac) {
              if (expected[16] & 2) ++saturated[w];
              run(tfn.data(), tfn.size(), expected);
              expected[22] += 2; expected[23] += 3;
            } else {
              Simple(op, expected);
              expected[22] += 2; expected[23] += 1;
            }
          }
          const auto expected_calls = calls;
          slow[w] += unsigned(expected_calls.size());

          sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low), reinterpret_cast<uint32_t>(high), true);
          region.macl_helper = reinterpret_cast<uint32_t>(&sh2_macl_region);
          region.macw_helper = reinterpret_cast<uint32_t>(&sh2_macw_region);
          for (auto op : ops) {
            const size_t before = region.PendingWords();
            if (!(region.CanEmit(op) && region.Emit(op))) { std::printf("reject %04x\n", op); return 1; }
            assert(region.PendingWords() - before <= region.MaxEmissionWords(op));
          }
          region.Finish();
          std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
          fn.insert(fn.end(), region.code.begin(), region.code.end());
          fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
          assert(fn.size() * 4 <= 65536);
          uint32_t actual[48];
          std::memcpy(actual, start, sizeof(actual));
          calls.clear();
          run(fn.data(), fn.size() * 4, actual);
          assert(calls.size() == expected_calls.size());
          for (size_t c = 0; c < calls.size(); ++c)
            assert(std::memcmp(&calls[c], &expected_calls[c], sizeof(Call)) == 0);
          assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
          ++cases[w];
        }
  }
  // Without guarded loads or the helper, MAC.L/MAC.W are not admitted and emit nothing.
  sh2a9::RegisterRegion plain, no_helper(reinterpret_cast<uint32_t>(low_full), reinterpret_cast<uint32_t>(full), true);
  for (uint16_t op : {uint16_t(0x010f), uint16_t(0x410f)}) {
    assert(!plain.CanEmit(op) && !plain.Emit(op) && plain.code.empty());
    assert(!no_helper.CanEmit(op) && !no_helper.Emit(op) && no_helper.code.empty());
  }
  for (bool w : {false, true})
    std::printf("A9 %s regions: %u sequences identical to the %s template "
                "(%u callbacks, %u S=1 accumulations)\n", w ? "MAC.W" : "MAC.L", cases[w],
                w ? "MAC_W" : "MAC_L", slow[w], saturated[w]);
}
