/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/a9_register_region.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>

// Guarded register-region stores. Oracle: the fast path is exactly an
// aligned T2 store to cached high RAM in a KiB with no code owner; every
// other address calls the width's memSet helper once with the published
// canonical registers, and resident registers are reloaded afterwards.
constexpr unsigned size = 1u << 20;
static std::vector<uint8_t> low(size), high(size), pages(1024);
static uint32_t *callback_state;
static std::array<uint32_t, 16> before_callback;
static std::vector<std::array<uint32_t, 16>> snapshots;
static size_t snapshot_index;
static unsigned calls, called_width;
static uint32_t called_address, called_value;
static bool model_memory;

static bool Direct(uint32_t address, unsigned width) {
  return (address >> 20) == 0x60 && !(address & (width - 1)) &&
         !pages[(address >> 10) & 1023];
}
static void Write(std::vector<uint8_t> &ram, uint32_t address, unsigned width, uint32_t value) {
  for (unsigned i = 0; i < width; ++i)  // T2: halfword-swapped big-endian
    ram[((address & (size - 1)) + i) ^ 1] = uint8_t(value >> (8 * (width - 1 - i)));
}
static void Slow(uint32_t address, uint32_t value, unsigned width) {
  const uint32_t *expected = before_callback.data();
  if (!snapshots.empty()) {
    assert(snapshot_index < snapshots.size());
    expected = snapshots[snapshot_index++].data();
  }
  assert(std::memcmp(callback_state, expected, 16 * 4) == 0);
  callback_state[15] ^= 0x76543210u;  // resident copies must reload
  ++calls;
  called_address = address;
  called_value = value;
  called_width = width;
  if (model_memory && (((address >> 20) & 0xdff) == 0x060 || ((address >> 20) & 0xdff) == 0x002))
    Write(((address >> 20) & 0xff) == 0x60 ? high : low, address, width, value);
}
static void Byte(uint32_t a, uint32_t v) { assert(v <= 0xff); Slow(a, v, 1); }
static void Word(uint32_t a, uint32_t v) { assert(v <= 0xffff); Slow(a, v, 2); }
static void Long(uint32_t a, uint32_t v) { Slow(a, v, 4); }
static uint32_t LoadSlow(uint32_t a, unsigned width) {
  assert(snapshot_index < snapshots.size());
  assert(std::memcmp(callback_state, snapshots[snapshot_index++].data(), 16 * 4) == 0);
  callback_state[15] ^= 0x76543210u;
  ++calls;
  (void)width;
  return a ^ 0x8f73c591u;
}
static uint32_t LByte(uint32_t a) { return LoadSlow(a, 1) & 255; }
static uint32_t LWord(uint32_t a) { return LoadSlow(a, 2) & 65535; }
static uint32_t LLong(uint32_t a) { return LoadSlow(a, 4); }

static void SetCallbacks(uint32_t *state) {
  state[25] = reinterpret_cast<uint32_t>(&LByte);
  state[26] = reinterpret_cast<uint32_t>(&LWord);
  state[27] = reinterpret_cast<uint32_t>(&LLong);
  state[28] = reinterpret_cast<uint32_t>(&Byte);
  state[29] = reinterpret_cast<uint32_t>(&Word);
  state[30] = reinterpret_cast<uint32_t>(&Long);
}
static void Run(uint32_t *code, const sh2a9::RegisterRegion &region, uint32_t *state) {
  std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
  fn.insert(fn.end(), region.code.begin(), region.code.end());
  fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
  assert(fn.size() * 4 <= 65536);
  std::memcpy(code, fn.data(), fn.size() * 4);
  __builtin___clear_cache(reinterpret_cast<char *>(code),
                         reinterpret_cast<char *>(code) + fn.size() * 4);
  callback_state = state;
  reinterpret_cast<void (*)(uint32_t *)>(code)(state);
}

int main() {
  sh2a9::RegisterRegion::base_reg_enabled = true; // r11 high-RAM base
  for (unsigned op = 0; op < 65536; ++op) {
    const unsigned k = op & 0xf00f;
    const bool expected = k == 0x2000 || k == 0x2001 || k == 0x2002 || k == 0x2004 ||
      k == 0x2005 || k == 0x2006 || k == 0x0004 || k == 0x0005 || k == 0x0006 ||
      (op >> 12) == 1 || (op >> 8) == 0x80 || (op >> 8) == 0x81;
    assert(sh2a9::RegisterRegion::IsStore(uint16_t(op)) == expected);
  }
  std::mt19937 random(0x53544f);
  for (auto &v : low) v = random();
  for (auto &v : high) v = random();
  for (unsigned p = 0; p < 1024; p += 3) pages[p] = 1;  // code-owned KiBs
  auto *code = static_cast<uint32_t *>(mmap(nullptr, 65536,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(code != MAP_FAILED);
  const uint32_t code_pages = reinterpret_cast<uint32_t>(pages.data());
  {
    sh2a9::RegisterRegion none(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    assert(!none.CanEmit(0x2102) && !none.Emit(0x2102) && none.code.empty());
  }
  // Pages 0x60 (KiB 0) and 0x60+0x3ff are owned; 0x0600_0400 is free.
  const uint32_t addresses[] = {
    0x06000400, 0x06000401, 0x06000402, 0x06000403, 0x060ffbfc, 0x060ffbfe,
    0x060ffbff, 0x06000000, 0x06000c02, 0x060ffffc, 0x26000400, 0x06100400,
    0x00200000, 0x00200401, 0x002ffffc, 0x20200400, 0x00300000, 0x05a00000,
    0x05f80000, 0x00000000, 0xc0000000, 0xffffffff
  };
  unsigned cases = 0;
  for (unsigned form = 0; form < 6; ++form)
    for (unsigned n = 0; n < 16; ++n)
      for (unsigned m = 0; m < (form >= 4 ? 1u : 16u); ++m)
        for (unsigned disp : {0u, 1u, 7u, 15u}) {
          if (form < 3 && disp) continue;
          const unsigned width = form < 3 ? 1u << form : form == 3 ? 4 : form == 4 ? 1 : 2;
          const uint16_t op = form < 3 ? uint16_t(0x2000 | (n << 8) | (m << 4) | form)
            : form == 3 ? uint16_t(0x1000 | (n << 8) | (m << 4) | disp)
            : uint16_t((form == 4 ? 0x8000 : 0x8100) | (n << 4) | disp);
          const unsigned offset = form < 3 ? 0 : disp * width;
          sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                     reinterpret_cast<uint32_t>(high.data()), true);
          region.code_pages = code_pages;
          assert(region.Emit(uint16_t(0x7001 | (((n + 1) & 15) << 8))));
          assert(region.Emit(uint16_t(0xe080 | (((n + 2) & 15) << 8))));
          const auto before = region.code.size();
          const unsigned maximum = region.MaxEmissionWords(op);
          assert(region.Emit(op));
          assert(region.code.size() - before <= maximum);
          assert(region.Emit(uint16_t(0x7005 | (((n + 1) & 15) << 8))));
          region.Finish();
          for (uint32_t address : addresses) {
            uint32_t state[40], expected[40];
            for (auto &v : state) v = random();
            SetCallbacks(state);
            state[n] = address - offset;
            std::memcpy(expected, state, sizeof(state));
            expected[(n + 1) & 15] += 1;
            expected[(n + 2) & 15] = 0xffffff80u;
            std::memcpy(before_callback.data(), expected, 64);
            const uint32_t value = expected[m];
            const bool direct = Direct(address, width);
            auto expected_high = high;
            if (direct) Write(expected_high, address, width, value);
            else expected[15] ^= 0x76543210u;
            expected[(n + 1) & 15] += 5;
            expected[22] += 8;
            expected[23] += 4;
            snapshots.clear();
            snapshot_index = calls = 0;
            Run(code, region, state);
            assert(std::memcmp(state, expected, sizeof(state)) == 0);
            assert(high == expected_high);
            assert(calls == (direct ? 0u : 1u));
            if (!direct) {
              const uint32_t mask = width == 4 ? ~0u : (1u << (8 * width)) - 1;
              assert(called_address == address && called_width == width &&
                     called_value == (value & mask));
            }
            ++cases;
          }
        }
  // Pre-decrement MOV.x Rm,@-Rn (2nm4-6): Rn -= size, then (Rn) = Rm as read
  // before the decrement (old template order; the helper sees the new Rn).
  // Indexed MOV.x Rm,@(R0,Rn) (0nm4-6): address R0 + Rn.
  for (unsigned indexed = 0; indexed < 2; ++indexed)
  for (unsigned kind = 0; kind < 3; ++kind)
    for (unsigned n = 0; n < 14; ++n)
      for (unsigned m = 0; m < 16; ++m) {
        const unsigned width = 1u << kind;
        const uint16_t op = uint16_t((indexed ? 0x0004 : 0x2004) | (n << 8) | (m << 4)) + kind;
        sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                   reinterpret_cast<uint32_t>(high.data()), true);
        region.code_pages = code_pages;
        assert(region.Emit(uint16_t(0x7001 | (((n + 1) & 15) << 8))));
        assert(region.Emit(uint16_t(0xe080 | (((n + 2) & 15) << 8))));
        const auto before = region.code.size();
        const unsigned maximum = region.MaxEmissionWords(op);
        assert(region.Emit(op));
        assert(region.code.size() - before <= maximum);
        assert(region.Emit(uint16_t(0x7005 | (((n + 1) & 15) << 8))));
        region.Finish();
        for (uint32_t address : addresses) {
          if (indexed && n == 0 && (address & 1)) continue;
          uint32_t state[40], expected[40];
          for (auto &v : state) v = random();
          SetCallbacks(state);
          const uint32_t index = indexed ? (n == 0 ? address / 2 : random() % 0x2000) : 0;
          if (indexed) { state[0] = index; if (n) state[n] = address - index; }
          else state[n] = address + width;
          std::memcpy(expected, state, sizeof(state));
          expected[(n + 1) & 15] += 1;
          expected[(n + 2) & 15] = 0xffffff80u;
          const uint32_t value = expected[m];
          if (!indexed) expected[n] -= width;
          std::memcpy(before_callback.data(), expected, 64);
          const bool direct = Direct(address, width);
          auto expected_high = high;
          if (direct) Write(expected_high, address, width, value);
          else expected[15] ^= 0x76543210u;
          expected[(n + 1) & 15] += 5;
          expected[22] += 8;
          expected[23] += 4;
          snapshots.clear();
          snapshot_index = calls = 0;
          Run(code, region, state);
          assert(std::memcmp(state, expected, sizeof(state)) == 0);
          assert(high == expected_high);
          assert(calls == (direct ? 0u : 1u));
          if (!direct) {
            const uint32_t mask = width == 4 ? ~0u : (1u << (8 * width)) - 1;
            assert(called_address == address && called_width == width &&
                   called_value == (value & mask));
          }
          ++cases;
        }
      }
  // Mixed sequences: guarded stores and loads, resident arithmetic, the same
  // addresses written then read; slow stores update modeled RAM like memSet.
  model_memory = true;
  sh2a9::RegisterRegion::disp_loads_enabled = true;
  for (unsigned run = 0; run < 600; ++run) {
    uint32_t state[40], expected[40];
    for (auto &v : state) v = random();
    const uint32_t bases[] = {0x06000400, 0x06000000, 0x060ff000, 0x00200400, 0x26000400, 0x05f80000};
    for (unsigned r = 0; r < 16; ++r) state[r] = bases[random() % 6] + (random() % 64) * 4;
    SetCallbacks(state);
    std::memcpy(expected, state, sizeof(state));
    auto model_low = low, model_high = high;
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    region.code_pages = code_pages;
    snapshots.clear();
    unsigned expected_calls = 0;
    for (unsigned step = 0; step < 40; ++step) {
      const unsigned kind = random() % 5, n = random() % 16, m = random() % 16, disp = random() % 16;
      if (kind < 2) {  // store: 1nmd, 2nm0-2, 2nm4-6 or 0nm4-6
        const unsigned width = kind == 0 ? 4 : 1u << (random() % 3);
        const unsigned variant = kind == 0 ? 0 : random() % 3;  // plain, @-Rn, @(R0,Rn)
        const unsigned size_bits = width == 4 ? 2 : width == 2 ? 1 : 0;
        const uint16_t op = kind == 0 ? uint16_t(0x1000 | (n << 8) | (m << 4) | disp)
          : variant == 0 ? uint16_t(0x2000 | (n << 8) | (m << 4) | size_bits)
          : variant == 1 ? uint16_t(0x2004 | (n << 8) | (m << 4) | size_bits)
          : uint16_t(0x0004 | (n << 8) | (m << 4) | size_bits);
        const uint32_t value_before = expected[m];
        if (kind == 1 && variant == 1) expected[n] -= width;
        const uint32_t address = kind == 0 ? expected[n] + disp * 4
          : variant == 2 ? expected[n] + expected[0] : expected[n];
        const uint32_t mask = width == 4 ? ~0u : (1u << (8 * width)) - 1;
        const uint32_t value = value_before;  // read before Rn -= size / the helper
        if (!Direct(address, width)) {
          std::array<uint32_t, 16> snapshot;
          std::memcpy(snapshot.data(), expected, 64);
          snapshots.push_back(snapshot);
          expected[15] ^= 0x76543210u;
          ++expected_calls;
          if (((address >> 20) & 0xdff) == 0x060) Write(model_high, address, width, value & mask);
          else if (((address >> 20) & 0xdff) == 0x002) Write(model_low, address, width, value & mask);
        } else {
          Write(model_high, address, width, value);
        }
        assert(region.Emit(op));
      } else if (kind < 4) {  // load: 5nmd, 6nm0-2 or 0nmC-E
        const unsigned width = kind == 2 ? 4 : 1u << (random() % 3);
        const bool indexed = kind == 3 && (random() & 1);
        const unsigned size_bits = width == 4 ? 2 : width == 2 ? 1 : 0;
        const uint16_t op = kind == 2 ? uint16_t(0x5000 | (n << 8) | (m << 4) | disp)
          : indexed ? uint16_t(0x000c | (n << 8) | (m << 4)) + size_bits
          : uint16_t(0x6000 | (n << 8) | (m << 4) | size_bits);
        const uint32_t address = kind == 2 ? expected[m] + disp * 4
          : indexed ? expected[m] + expected[0] : expected[m];
        uint32_t value = 0;
        if (((address >> 20) == 2 || (address >> 20) == 0x60) && !(address & (width - 1))) {
          const auto &ram = (address >> 20) == 2 ? model_low : model_high;
          for (unsigned i = 0; i < width; ++i)
            value = (value << 8) | ram[((address & (size - 1)) + i) ^ 1];
        } else {
          std::array<uint32_t, 16> snapshot;
          std::memcpy(snapshot.data(), expected, 64);
          snapshots.push_back(snapshot);
          expected[15] ^= 0x76543210u;
          ++expected_calls;
          value = address ^ 0x8f73c591u;
        }
        if (width == 1) value = uint32_t(int32_t(int8_t(value)));
        if (width == 2) value = uint32_t(int32_t(int16_t(value)));
        assert(region.Emit(op));
        expected[n] = value;
        // Keep n a plausible base for later steps.
        assert(region.Emit(uint16_t(0x6003 | (n << 8) | (((n + 1) & 15) << 4))));
        expected[n] = expected[(n + 1) & 15];
      } else {
        assert(region.Emit(uint16_t(0x7004 | (n << 8))));
        expected[n] += 4;
      }
    }
    region.Finish();
    const uint32_t pc = state[22], cycles = state[23];
    snapshot_index = calls = 0;
    Run(code, region, state);
    expected[22] = pc + region.instructions * 2;
    expected[23] = cycles + region.instructions;
    assert(std::memcmp(state, expected, sizeof(state)) == 0);
    assert(calls == expected_calls && snapshot_index == snapshots.size());
    assert(low == model_low && high == model_high);
    ++cases;
  }
  munmap(code, 65536);
  std::printf("A9 RAM stores: %u executed cases, all store forms, registers, displacements and code-page slow paths\n", cases);
}
