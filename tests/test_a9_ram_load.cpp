/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/a9_ram_load.h"
#include "../src/core/sh2_dynarec/a9_register_region.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <array>

static unsigned calls, called_width;
static uint32_t called_address;
static uint32_t *callback_state;
static uint32_t before_callback[16];
static std::vector<std::array<uint32_t, 16>> callback_snapshots;
static size_t snapshot_index;
static bool check_sr;
static uint32_t expected_sr;
static uint32_t expected_macl;
static uint32_t Slow(uint32_t address, unsigned width) {
  if (callback_state) {
    const uint32_t *expected = before_callback;
    if (!callback_snapshots.empty()) {
      assert(snapshot_index < callback_snapshots.size());
      expected = callback_snapshots[snapshot_index++].data();
    }
    assert(std::memcmp(callback_state, expected, sizeof(before_callback)) == 0);
    if (check_sr) {
      assert(callback_state[16] == expected_sr);
      assert(callback_state[20] == expected_macl);
      callback_state[16] ^= 0x101; // observer changes both T and Q
      callback_state[20] ^= 0x13579bdf;
    }
    // Model a callback changing canonical state: resident copies must reload.
    callback_state[15] ^= 0x76543210u;
  }
  ++calls;
  called_address = address;
  called_width = width;
  return address ^ 0x8f73c591u;
}
static uint32_t Byte(uint32_t a) { return Slow(a, 1) & 255; }
static uint32_t Word(uint32_t a) { return Slow(a, 2) & 65535; }
static uint32_t Long(uint32_t a) { return Slow(a, 4); }

int main() {
  sh2a9::RegisterRegion::base_reg_enabled = true; // r11 high-RAM base
  for (unsigned op = 0; op < 65536; ++op) {
    const unsigned kind = op & 0xf00f;
    const bool expected = kind == 0x6000 || kind == 0x6001 || kind == 0x6002 ||
                          kind == 0x6004 || kind == 0x6005 || kind == 0x6006;
    assert(sh2a9::RegisterRegion::IsIndirectLoad(uint16_t(op)) == expected);
  }
  constexpr unsigned size = 1u << 20;
  std::vector<uint8_t> low(size), high(size);
  std::mt19937 random(0x52414d);
  for (auto &v : low) v = random();
  for (auto &v : high) v = random();
  constexpr size_t code_capacity = 65536;
  auto *code = static_cast<uint32_t *>(mmap(nullptr, code_capacity,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(code != MAP_FAILED);
  // RAM edges, unaligned cases, uncached aliases, mirrors, MMIO, ROM and
  // address-array space. Slow cases must preserve the full original address.
  const uint32_t addresses[] = {
    0x00200000, 0x00200001, 0x00200002, 0x00200003, 0x002ffffc, 0x002ffffe,
    0x002fffff, 0x06000000, 0x06000001, 0x06000002, 0x060ffffc, 0x060ffffe,
    0x060fffff, 0x20200000, 0x26000000, 0x06100000, 0x00300000, 0x05a00000,
    0x05f80000, 0x00000000, 0xc0000000, 0xffffffff
  };
  unsigned cases = 0;
  for (bool region_mode : {false, true})
  for (bool postincrement : {false, true})
  for (unsigned width : {1u, 2u, 4u})
    for (unsigned n = 0; n < 16; ++n)
      for (unsigned m = 0; m < 16; ++m) {
        auto body = sh2a9::RamLoad(n, m, width,
          reinterpret_cast<uint32_t>(low.data()), reinterpret_cast<uint32_t>(high.data()), postincrement);
        std::vector<uint32_t> fn = {0xe92d4ff8u, 0xe1a07000u};
        if (region_mode) {
          sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                     reinterpret_cast<uint32_t>(high.data()), true);
          assert(region.Emit(uint16_t(0x7001 | (((m + 1) & 15) << 8))));
          assert(region.Emit(uint16_t(0xe080 | (((m + 2) & 15) << 8))));
          const uint16_t op = uint16_t(0x6000 | (postincrement ? 4 : 0) | (n << 8) | (m << 4) |
                                      (width == 4 ? 2 : width == 2 ? 1 : 0));
          const auto before = region.code.size();
          const unsigned maximum = region.MaxEmissionWords(op);
          assert(region.Emit(op));
          assert(region.code.size() - before <= maximum);
          assert(region.Emit(uint16_t(0x7005 | (((m + 1) & 15) << 8))));
          region.Finish();
          body = region.code;
          fn.push_back(0xe5978058u);
          fn.push_back(0xe597905cu);
        }
        fn.insert(fn.end(), body.begin(), body.end());
        if (region_mode) {
          fn.push_back(0xe5878058u);
          fn.push_back(0xe587905cu);
        }
        fn.push_back(0xe8bd8ff8u);
        assert(fn.size() * 4 <= 4096);
        std::memcpy(code, fn.data(), fn.size() * 4);
        __builtin___clear_cache(reinterpret_cast<char *>(code),
          reinterpret_cast<char *>(code) + fn.size() * 4);
        for (uint32_t address : addresses) {
          uint32_t state[40], expected[40];
          for (auto &v : state) v = random();
          state[25] = reinterpret_cast<uint32_t>(&Byte);
          state[26] = reinterpret_cast<uint32_t>(&Word);
          state[27] = reinterpret_cast<uint32_t>(&Long);
          state[m] = address;
          std::memcpy(expected, state, sizeof(state));
          callback_state = region_mode ? state : nullptr;
          if (region_mode) {
            expected[(m + 1) & 15] += 1;
            expected[(m + 2) & 15] = 0xffffff80u;
            std::memcpy(before_callback, expected, sizeof(before_callback));
          }
          const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60)
            && !(address & (width - 1));
          uint32_t value = 0;
          if (direct) {
            const auto &ram = (address >> 20) == 2 ? low : high;
            unsigned offset = address & (size - 1);
            // Independent byte assembly for the halfword-swapped T2 layout.
            for (unsigned i = 0; i < width; ++i)
              value = (value << 8) | ram[(offset + i) ^ 1];
          } else {
            value = address ^ 0x8f73c591u;
            if (region_mode) expected[15] ^= 0x76543210u;
          }
          if (width == 1) value = static_cast<int8_t>(value);
          if (width == 2) value = static_cast<int16_t>(value);
          expected[n] = value;
          if (postincrement && n != m) expected[m] += width;
          if (region_mode) {
            expected[(m + 1) & 15] += 5;
            expected[22] += 8;
            expected[23] += 4;
          }
          calls = 0;
          reinterpret_cast<void (*)(uint32_t *)>(code)(state);
          assert(std::memcmp(state, expected, sizeof(state)) == 0);
          assert(calls == (direct ? 0u : 1u));
          if (!direct) assert(called_address == address && called_width == width);
          ++cases;
        }
      }
  for (unsigned run = 0; run < 500; ++run) {
    uint32_t state[40], expected[40];
    for (auto &v : state) v = random();
    for (unsigned r = 0; r < 16; ++r) state[r] = addresses[random() % 22];
    state[25] = reinterpret_cast<uint32_t>(&Byte);
    state[26] = reinterpret_cast<uint32_t>(&Word);
    state[27] = reinterpret_cast<uint32_t>(&Long);
    std::memcpy(expected, state, sizeof(state));
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    callback_snapshots.clear();
    for (unsigned step = 0; step < 64; ++step) {
      const unsigned n = random() % 16, m = random() % 16, kind = random() % 3;
      const bool postincrement = (random() & 1) != 0;
      const unsigned width = 1u << kind;
      const uint32_t address = expected[m];
      const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60) &&
                          !(address & (width - 1));
      uint32_t value = 0;
      if (direct) {
        const auto &ram = (address >> 20) == 2 ? low : high;
        for (unsigned i = 0; i < width; ++i)
          value = (value << 8) | ram[((address & 0xfffff) + i) ^ 1];
      } else {
        std::array<uint32_t, 16> snapshot;
        std::memcpy(snapshot.data(), expected, sizeof(snapshot));
        callback_snapshots.push_back(snapshot);
        expected[15] ^= 0x76543210u;
        value = address ^ 0x8f73c591u;
      }
      if (width == 1) value = int8_t(value);
      if (width == 2) value = int16_t(value);
      expected[n] = value;
      if (postincrement && n != m) expected[m] += width;
      assert(region.Emit(uint16_t(0x6000 | (postincrement ? 4 : 0) | (n << 8) | (m << 4) | kind)));
      assert(region.Emit(uint16_t(0x7001 | (n << 8))));
      ++expected[n];
    }
    region.Finish();
    expected[22] += 256;
    expected[23] += 128;
    std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
    fn.insert(fn.end(), region.code.begin(), region.code.end());
    fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
    assert(fn.size() * 4 <= code_capacity);
    std::memcpy(code, fn.data(), fn.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(code),
                           reinterpret_cast<char *>(code) + fn.size() * 4);
    callback_state = state;
    snapshot_index = calls = 0;
    reinterpret_cast<void (*)(uint32_t *)>(code)(state);
    assert(std::memcmp(state, expected, sizeof(state)) == 0);
    assert(calls == callback_snapshots.size() && snapshot_index == calls);
    ++cases;
  }
  callback_snapshots.clear();
  check_sr = true;
  for (unsigned run = 0; run < 128; ++run) {
    uint32_t state[40], expected[40];
    for (auto &v : state) v = random();
    state[0] = 0x05f80000;
    state[27] = reinterpret_cast<uint32_t>(&Long);
    std::memcpy(expected, state, sizeof(state));
    std::memcpy(before_callback, state, sizeof(before_callback));
    expected_sr = state[16] | 1;
    expected_macl = uint32_t(uint64_t(state[3]) * state[4]);
    expected[20] = expected_macl ^ 0x13579bdf;
    expected[5] = expected[20];
    expected[16] = expected_sr ^ 0x101;
    expected[15] ^= 0x76543210;
    expected[1] = state[0] ^ 0x8f73c591;
    expected[2] = expected[16] & 1;
    expected[22] += 10; expected[23] += 8;
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    assert(region.Emit(0x0018)); // SETT, initially resident only
    assert(region.Emit(0x0347)); // MUL.L R4,R3; deferred MACL and extra cycles
    const auto before = region.code.size();
    const auto bound = region.MaxEmissionWords(0x6102);
    assert(region.Emit(0x6102)); // callback must see published T
    assert(region.code.size() - before <= bound);
    assert(region.Emit(0x0229)); // MOVT must reload the callback's T
    assert(region.Emit(0x051a)); // STS MACL,R5 must see the callback's new MACL
    region.Finish();
    std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
    fn.insert(fn.end(), region.code.begin(), region.code.end());
    fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
    assert(fn.size() * 4 <= code_capacity);
    std::memcpy(code, fn.data(), fn.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(code),
                           reinterpret_cast<char *>(code) + fn.size() * 4);
    callback_state = state; calls = 0;
    reinterpret_cast<void (*)(uint32_t *)>(code)(state);
    assert(calls == 1 && std::memcmp(state, expected, sizeof(state)) == 0);
    ++cases;
  }
  // Displacement loads (5nmd, 85md, 84md): effective address Rm + disp*width,
  // Rm unchanged, 84/85 load R0. Same guard, alias and callback contract.
  for (unsigned op = 0; op < 65536; ++op) {
    const bool expected = (op >> 12) == 5 || (op >> 8) == 0x85 || (op >> 8) == 0x84;
    assert(sh2a9::RegisterRegion::IsDisplacementLoad(uint16_t(op)) == expected);
  }
  check_sr = false;
  callback_snapshots.clear();
  sh2a9::RegisterRegion::disp_loads_enabled = true;
  for (unsigned form = 0; form < 3; ++form)
    for (unsigned m = 0; m < 16; ++m)
      for (unsigned n = 0; n < (form == 0 ? 16u : 1u); ++n)
        for (unsigned disp = 0; disp < 16; ++disp) {
          const unsigned width = form == 0 ? 4 : form == 1 ? 2 : 1;
          const uint16_t op = form == 0 ? uint16_t(0x5000 | (n << 8) | (m << 4) | disp)
                                        : uint16_t((form == 1 ? 0x8500 : 0x8400) | (m << 4) | disp);
          sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                     reinterpret_cast<uint32_t>(high.data()), true);
          assert(region.Emit(uint16_t(0x7001 | (((m + 1) & 15) << 8))));
          assert(region.Emit(uint16_t(0xe080 | (((m + 2) & 15) << 8))));
          const auto before = region.code.size();
          const unsigned maximum = region.MaxEmissionWords(op);
          assert(region.Emit(op));
          assert(region.code.size() - before <= maximum);
          assert(region.Emit(uint16_t(0x7005 | (((m + 1) & 15) << 8))));
          region.Finish();
          std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
          fn.insert(fn.end(), region.code.begin(), region.code.end());
          fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
          assert(fn.size() * 4 <= code_capacity);
          std::memcpy(code, fn.data(), fn.size() * 4);
          __builtin___clear_cache(reinterpret_cast<char *>(code),
                                 reinterpret_cast<char *>(code) + fn.size() * 4);
          for (uint32_t address : addresses) {
            uint32_t state[40], expected[40];
            for (auto &v : state) v = random();
            state[25] = reinterpret_cast<uint32_t>(&Byte);
            state[26] = reinterpret_cast<uint32_t>(&Word);
            state[27] = reinterpret_cast<uint32_t>(&Long);
            state[m] = address - disp * width;
            std::memcpy(expected, state, sizeof(state));
            callback_state = state;
            expected[(m + 1) & 15] += 1;
            expected[(m + 2) & 15] = 0xffffff80u;
            std::memcpy(before_callback, expected, sizeof(before_callback));
            const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60)
              && !(address & (width - 1));
            uint32_t value = 0;
            if (direct) {
              const auto &ram = (address >> 20) == 2 ? low : high;
              for (unsigned i = 0; i < width; ++i)
                value = (value << 8) | ram[((address & (size - 1)) + i) ^ 1];
            } else {
              value = address ^ 0x8f73c591u;
              expected[15] ^= 0x76543210u;
            }
            if (width == 1) value = static_cast<int8_t>(value);
            if (width == 2) value = static_cast<int16_t>(value);
            expected[n] = value;
            expected[(m + 1) & 15] += 5;
            expected[22] += 8;
            expected[23] += 4;
            calls = 0;
            reinterpret_cast<void (*)(uint32_t *)>(code)(state);
            assert(std::memcmp(state, expected, sizeof(state)) == 0);
            assert(calls == (direct ? 0u : 1u));
            if (!direct) assert(called_address == address && called_width == width);
            ++cases;
          }
        }
  // GBR loads MOV.B/W/L @(disp,GBR),R0 (C4dd-C6dd): effective address
  // GBR + disp*width (up to 1020, one or two ADDs), GBR unchanged.
  for (unsigned op = 0; op < 65536; ++op) {
    const bool expected = (op >> 8) == 0xc4 || (op >> 8) == 0xc5 || (op >> 8) == 0xc6;
    assert(sh2a9::RegisterRegion::IsGbrLoad(uint16_t(op)) == expected);
  }
  sh2a9::RegisterRegion::gbr_loads_enabled = true;
  for (unsigned kind = 0; kind < 3; ++kind)
    for (unsigned disp : {0u, 1u, 15u, 63u, 64u, 127u, 128u, 129u, 191u, 255u}) {
      const unsigned width = 1u << kind, m = 3;
      const uint16_t op = uint16_t(((0xc4 + kind) << 8) | disp);
      sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                 reinterpret_cast<uint32_t>(high.data()), true);
      assert(region.Emit(uint16_t(0x7001 | ((m + 1) << 8))));
      assert(region.Emit(uint16_t(0xe080 | ((m + 2) << 8))));
      const auto before = region.code.size();
      const unsigned maximum = region.MaxEmissionWords(op);
      assert(region.Emit(op));
      assert(region.code.size() - before <= maximum);
      assert(region.Emit(uint16_t(0x7005 | ((m + 1) << 8))));
      region.Finish();
      std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
      fn.insert(fn.end(), region.code.begin(), region.code.end());
      fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
      assert(fn.size() * 4 <= code_capacity);
      std::memcpy(code, fn.data(), fn.size() * 4);
      __builtin___clear_cache(reinterpret_cast<char *>(code),
                             reinterpret_cast<char *>(code) + fn.size() * 4);
      for (uint32_t address : addresses) {
        uint32_t state[40], expected[40];
        for (auto &v : state) v = random();
        state[25] = reinterpret_cast<uint32_t>(&Byte);
        state[26] = reinterpret_cast<uint32_t>(&Word);
        state[27] = reinterpret_cast<uint32_t>(&Long);
        state[17] = address - disp * width;
        std::memcpy(expected, state, sizeof(state));
        callback_state = state;
        expected[m + 1] += 1;
        expected[m + 2] = 0xffffff80u;
        std::memcpy(before_callback, expected, sizeof(before_callback));
        const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60)
          && !(address & (width - 1));
        uint32_t value = 0;
        if (direct) {
          const auto &ram = (address >> 20) == 2 ? low : high;
          for (unsigned i = 0; i < width; ++i)
            value = (value << 8) | ram[((address & (size - 1)) + i) ^ 1];
        } else {
          value = address ^ 0x8f73c591u;
          expected[15] ^= 0x76543210u;
        }
        if (width == 1) value = static_cast<int8_t>(value);
        if (width == 2) value = static_cast<int16_t>(value);
        expected[0] = value;
        expected[m + 1] += 5;
        expected[22] += 8;
        expected[23] += 4;
        calls = 0;
        reinterpret_cast<void (*)(uint32_t *)>(code)(state);
        assert(std::memcmp(state, expected, sizeof(state)) == 0);
        assert(calls == (direct ? 0u : 1u));
        if (!direct) assert(called_address == address && called_width == width);
        ++cases;
      }
    }
  // Indexed loads MOV.B/W/L @(R0,Rm),Rn (0nmC-E): effective address R0 + Rm.
  for (unsigned op = 0; op < 65536; ++op) {
    const bool expected = (op >> 12) == 0 && (op & 15) >= 0xc && (op & 15) <= 0xe;
    assert(sh2a9::RegisterRegion::IsIndexedLoad(uint16_t(op)) == expected);
  }
  for (unsigned kind = 0; kind < 3; ++kind)
    for (unsigned m = 0; m < 14; ++m)
      for (unsigned n = 0; n < 16; ++n) {
        const unsigned width = 1u << kind;
        const uint16_t op = uint16_t(0x000c | (n << 8) | (m << 4)) + kind;
        sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                                   reinterpret_cast<uint32_t>(high.data()), true);
        assert(region.Emit(uint16_t(0x7001 | (((m + 1) & 15) << 8))));
        assert(region.Emit(uint16_t(0xe080 | (((m + 2) & 15) << 8))));
        const auto before = region.code.size();
        const unsigned maximum = region.MaxEmissionWords(op);
        assert(region.Emit(op));
        assert(region.code.size() - before <= maximum);
        assert(region.Emit(uint16_t(0x7005 | (((m + 1) & 15) << 8))));
        region.Finish();
        std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
        fn.insert(fn.end(), region.code.begin(), region.code.end());
        fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
        std::memcpy(code, fn.data(), fn.size() * 4);
        __builtin___clear_cache(reinterpret_cast<char *>(code),
                               reinterpret_cast<char *>(code) + fn.size() * 4);
        for (uint32_t address : addresses) {
          if (m == 0 && (address & 1)) continue;
          uint32_t state[40], expected[40];
          for (auto &v : state) v = random();
          state[25] = reinterpret_cast<uint32_t>(&Byte);
          state[26] = reinterpret_cast<uint32_t>(&Word);
          state[27] = reinterpret_cast<uint32_t>(&Long);
          const uint32_t index = m == 0 ? address / 2 : random() % 0x2000;
          state[0] = index;
          if (m) state[m] = address - index;
          std::memcpy(expected, state, sizeof(state));
          callback_state = state;
          expected[(m + 1) & 15] += 1;
          expected[(m + 2) & 15] = 0xffffff80u;
          std::memcpy(before_callback, expected, sizeof(before_callback));
          const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60)
            && !(address & (width - 1));
          uint32_t value = 0;
          if (direct) {
            const auto &ram = (address >> 20) == 2 ? low : high;
            for (unsigned i = 0; i < width; ++i)
              value = (value << 8) | ram[((address & (size - 1)) + i) ^ 1];
          } else {
            value = address ^ 0x8f73c591u;
            expected[15] ^= 0x76543210u;
          }
          if (width == 1) value = static_cast<int8_t>(value);
          if (width == 2) value = static_cast<int16_t>(value);
          expected[n] = value;
          expected[(m + 1) & 15] += 5;
          expected[22] += 8;
          expected[23] += 4;
          calls = 0;
          reinterpret_cast<void (*)(uint32_t *)>(code)(state);
          assert(std::memcmp(state, expected, sizeof(state)) == 0);
          assert(calls == (direct ? 0u : 1u));
          if (!direct) assert(called_address == address && called_width == width);
          ++cases;
        }
      }
  // Mixed random sequences of indirect and displacement loads.
  for (unsigned run = 0; run < 400; ++run) {
    uint32_t state[40], expected[40];
    for (auto &v : state) v = random();
    for (unsigned r = 0; r < 16; ++r) state[r] = addresses[random() % 22] - (random() % 16) * 4;
    state[25] = reinterpret_cast<uint32_t>(&Byte);
    state[26] = reinterpret_cast<uint32_t>(&Word);
    state[27] = reinterpret_cast<uint32_t>(&Long);
    std::memcpy(expected, state, sizeof(state));
    sh2a9::RegisterRegion region(reinterpret_cast<uint32_t>(low.data()),
                               reinterpret_cast<uint32_t>(high.data()), true);
    callback_snapshots.clear();
    for (unsigned step = 0; step < 48; ++step) {
      const unsigned form = random() % 4, m = random() % 16, disp = random() % 16;
      unsigned n = random() % 16, width;
      uint16_t op;
      uint32_t address;
      if (form == 3) {
        const unsigned kind = random() % 3;
        width = 1u << kind;
        op = uint16_t(0x6000 | (n << 8) | (m << 4) | kind);
        address = expected[m];
      } else {
        width = form == 0 ? 4 : form == 1 ? 2 : 1;
        if (form != 0) n = 0;
        op = form == 0 ? uint16_t(0x5000 | (n << 8) | (m << 4) | disp)
                       : uint16_t((form == 1 ? 0x8500 : 0x8400) | (m << 4) | disp);
        address = expected[m] + disp * width;
      }
      const bool direct = ((address >> 20) == 2 || (address >> 20) == 0x60) &&
                          !(address & (width - 1));
      uint32_t value = 0;
      if (direct) {
        const auto &ram = (address >> 20) == 2 ? low : high;
        for (unsigned i = 0; i < width; ++i)
          value = (value << 8) | ram[((address & 0xfffff) + i) ^ 1];
      } else {
        std::array<uint32_t, 16> snapshot;
        std::memcpy(snapshot.data(), expected, sizeof(snapshot));
        callback_snapshots.push_back(snapshot);
        expected[15] ^= 0x76543210u;
        value = address ^ 0x8f73c591u;
      }
      if (width == 1) value = int8_t(value);
      if (width == 2) value = int16_t(value);
      expected[n] = value;
      assert(region.Emit(op));
      // Keep loaded values plausible bases for later steps half of the time.
      if (random() & 1) {
        const uint32_t base = addresses[random() % 22];
        assert(region.Emit(uint16_t(0xe000 | (n << 8) | (base & 0xff))));
        expected[n] = uint32_t(int32_t(int8_t(base & 0xff)));
      } else {
        assert(region.Emit(uint16_t(0x7001 | (n << 8))));
        ++expected[n];
      }
    }
    region.Finish();
    std::vector<uint32_t> fn{0xe92d4ff8u, 0xe1a07000u, 0xe5978058u, 0xe597905cu};
    fn.insert(fn.end(), region.code.begin(), region.code.end());
    fn.insert(fn.end(), {0xe5878058u, 0xe587905cu, 0xe8bd8ff8u});
    assert(fn.size() * 4 <= code_capacity);
    std::memcpy(code, fn.data(), fn.size() * 4);
    __builtin___clear_cache(reinterpret_cast<char *>(code),
                           reinterpret_cast<char *>(code) + fn.size() * 4);
    callback_state = state;
    snapshot_index = calls = 0;
    reinterpret_cast<void (*)(uint32_t *)>(code)(state);
    expected[22] = state[22];
    expected[23] = state[23];
    assert(std::memcmp(state, expected, sizeof(state)) == 0);
    assert(calls == callback_snapshots.size() && snapshot_index == calls);
    ++cases;
  }
  sh2a9::RegisterRegion::disp_loads_enabled = false;
  munmap(code, code_capacity);
  std::printf("A9 RAM loads: %u executed cases, direct/postincrement templates and guarded regions, all register pairs and three widths\n", cases);
}
