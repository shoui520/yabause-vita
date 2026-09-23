/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "native_opcodes.h"

namespace m68ka9 {
// A32 arithmetic region, not a complete executor. Entry: r0 points to the
// C68K D/A/flag prefix; caller preserves r4-r11. No calls or observation points
// are allowed inside a region. Flush BEFORE budget exits, memory, exceptions,
// branches or interpreter fallback. Integration must account for guest timing.
// ARM DDI 0406B A2 AddWithCarry: subtraction C is NOT borrow.
class RegisterRegion {
public:
  std::vector<uint32_t> code;
  unsigned instructions = 0;
  unsigned cycles = 0; // C68K-compatible costs; scheduler integration is separate.

  static bool Supports(uint16_t op) {
    return C68kNativeOpcodeSupported(op) != 0;
  }

  static unsigned Cycles(uint16_t op) {
    if (!Supports(op)) return 0;
    if ((op & 0xf0f0) == 0x5080 || (op & 0xf1f8) == 0xb180) return 8;
    if ((op & 0xf100) == 0x7000 || op == 0x4e71 ||
        (op & 0xf1f0) == 0x2000 || (op & 0xf1f0) == 0x2040 ||
        (op & 0xfff8) == 0x4a80) return 4;
    return 6;
  }

  bool Emit(uint16_t op) {
    if (!Supports(op)) return false;
    ++instructions;
    cycles += Cycles(op);
    if (op == 0x4e71) return true;
    const unsigned n = (op >> 9) & 7, m = op & 7;
    if ((op & 0xf100) == 0x7000) {
      unsigned d = Get(n, false);
      Constant(d, uint32_t(int32_t(int8_t(op))));
      Dirty(d); Logical(d); return true;
    }
    if ((op & 0xf1f0) == 0x2000 || (op & 0xf1f0) == 0x2040) {
      const bool address = (op & 0x40) != 0;
      unsigned s = Get(m + ((op & 8) ? 8 : 0), true);
      unsigned d = Get(n + (address ? 8 : 0), false, s);
      Word(0xe1a00000u | d << 12 | s);
      Dirty(d); if (!address) Logical(d); return true;
    }
    if ((op & 0xf0f0) == 0x5080) {
      const bool address = (op & 8) != 0, sub = (op & 0x100) != 0;
      unsigned d = Get(m + (address ? 8 : 0), true);
      unsigned immediate = n ? n : 8;
      Word((sub ? 0xe2400000u : 0xe2800000u) |
           (address ? 0 : 1u << 20) | d << 16 | d << 12 | immediate);
      Dirty(d); if (!address) Arithmetic(d, sub, true); return true;
    }
    const unsigned unary = op & 0xfff8;
    if (unary == 0x4280 || unary == 0x4480 || unary == 0x4680 || unary == 0x4a80) {
      unsigned d = Get(m, unary != 0x4280);
      if (unary == 0x4280) Word(0xe3a00000u | d << 12);
      if (unary == 0x4480) Word(0xe2700000u | d << 16 | d << 12); // RSBS #0
      if (unary == 0x4680) Word(0xe1e00000u | d << 12 | d);
      if (unary != 0x4a80) Dirty(d);
      if (unary == 0x4480) Arithmetic(d, true, true); else Logical(d);
      return true;
    }
    const unsigned kind = op & 0xf1f8;
    const bool address = kind == 0xd1c0 || kind == 0x91c0;
    const bool compare = kind == 0xb080, eor = kind == 0xb180;
    unsigned s = Get(eor ? n : m, true);
    unsigned d = Get(eor ? m : n + (address ? 8 : 0), true, s);
    const unsigned result = compare ? 12 : d;
    uint32_t insn;
    switch (kind) {
    case 0xd080: insn = 0xe0900000; break;
    case 0x9080: case 0xb080: insn = 0xe0500000; break;
    case 0xc080: insn = 0xe0000000; break;
    case 0x8080: insn = 0xe1800000; break;
    case 0xb180: insn = 0xe0200000; break;
    case 0xd1c0: insn = 0xe0800000; break;
    default: insn = 0xe0400000; break;
    }
    Word(insn | d << 16 | result << 12 | s);
    if (!compare) Dirty(d);
    if (address) return true;
    if (kind == 0xd080 || kind == 0x9080 || compare)
      Arithmetic(result, kind != 0xd080, !compare);
    else Logical(result);
    return true;
  }

  void Flush() {
    for (unsigned i = 0; i < slots.size(); ++i) {
      if (slots[i].dirty) Store(i + 4, unsigned(slots[i].guest) * 4);
      slots[i] = Slot{};
    }
    if (flags != None) {
      if (flags == Logic) { Word(0xe3a01000); Word(0xe3a02000); }
      else {
        Word(0xe1a01aa9, 0, SavedNZCV); // MOV r1,r9,LSR #21
        Word(0xe2011c01); // AND r1,r1,#0x100
        if (flags == Subtract) Word(0xe2211c01);
        Word(0xe1a02aa9, 0, SavedNZCV); Word(0xe2022080); // V bit28 -> bit7
      }
      Store(1, 64); Store(2, 68); Store(10, 72);
      Word(0xe1a01c2a, 0, SavedResult); Store(1, 76); // N = result >> 24
    }
    MaterializeX();
    if (x_dirty) Store(11, 80);
    flags = None; x_dirty = false;
    PruneFlagTemporaries();
  }

private:
  struct Slot { int guest = -1; bool dirty = false; unsigned age = 0; };
  std::array<Slot, 5> slots{}; // r4-r8; both D and A registers compete
  unsigned clock = 0;
  enum FlagKind { None, Logic, Add, Subtract } flags = None;
  FlagKind pending_x = None;
  bool x_dirty = false;
  enum : uint8_t { SavedNZCV = 1, SavedResult = 2, SavedX = 4 };
  struct FlagEffect { uint8_t defines, uses; };
  std::vector<FlagEffect> flag_effects;
  std::size_t flush_begin = 0;
  void Word(uint32_t word, uint8_t defines = 0, uint8_t uses = 0) {
    code.push_back(word);
    flag_effects.push_back({defines, uses});
  }
  void Store(unsigned r, unsigned offset) {
    Word(0xe5800000u | r << 12 | offset, 0,
         r >= 9 && r <= 11 ? uint8_t(1u << (r - 9)) : 0);
  }
  // Only pure writes to our private r9-r11 flag temporaries are removable.
  // Guest arithmetic, stores, and APSR producers remain untouched. ARM ARM
  // DDI 0406B A4.5: MRS reads APSR into a GPR; it does not update guest state.
  // Flush is the observation boundary: all required CCR/X values are stored,
  // and none of these temporary registers is live into the next region.
  void PruneFlagTemporaries() {
    uint8_t live = 0;
    for (std::size_t i = code.size(); i-- > flush_begin;) {
      auto &effect = flag_effects[i];
      if (effect.defines && !(effect.defines & live)) {
        effect = {0xff, 0}; // Dead pure temporary definition.
      } else {
        live = uint8_t((live & ~effect.defines) | effect.uses);
      }
    }
    std::size_t out = flush_begin;
    for (std::size_t i = flush_begin; i < code.size(); ++i) {
      if (flag_effects[i].defines == 0xff) continue;
      code[out] = code[i]; flag_effects[out] = flag_effects[i]; ++out;
    }
    code.resize(out); flag_effects.resize(out); flush_begin = out;
  }
  void Dirty(unsigned r) { slots[r - 4].dirty = true; }
  unsigned Get(unsigned guest, bool load, unsigned avoid = 99) {
    unsigned victim = 99;
    for (unsigned i = 0; i < slots.size(); ++i) {
      if (slots[i].guest == int(guest)) { slots[i].age = ++clock; return i + 4; }
      if (i + 4 != avoid && (victim == 99 || slots[i].guest < 0 ||
          (slots[victim].guest >= 0 && slots[i].age < slots[victim].age))) victim = i;
    }
    Slot &slot = slots[victim];
    if (slot.dirty) Store(victim + 4, unsigned(slot.guest) * 4);
    slot = Slot{int(guest), false, ++clock};
    if (load) Word(0xe5900000u | (victim + 4) << 12 | guest * 4);
    return victim + 4;
  }
  void Constant(unsigned r, uint32_t value) {
    // MOVEQ's sign-extended byte always fits MOV or MVN imm8. In particular,
    // negative MOVEQ does not need a MOVW/MOVT pair on ARMv7-A.
    if (value <= 255) { Word(0xe3a00000u | r << 12 | value); return; }
    if (~value <= 255) { Word(0xe3e00000u | r << 12 | ~value); return; }
    Word(0xe3000000u | r << 12 | (value & 0xfff) | ((value >> 12) & 15) << 16);
    if (value >> 16) Word(0xe3400000u | r << 12 | ((value >> 16) & 0xfff) |
                         ((value >> 28) & 15) << 16);
  }
  void Logical(unsigned result) {
    Word(0xe1a0a000u | result, SavedResult); flags = Logic;
  }
  void Arithmetic(unsigned result, bool sub, bool update_x) {
    // CMP replaces NZCV without replacing guest X. Save an outstanding X
    // before its source in r9 is overwritten. These non-S operations leave
    // the just-computed ARM arithmetic flags intact for MRS below.
    if (!update_x) MaterializeX();
    Word(0xe10f9000, SavedNZCV); // MRS r9,APSR: independently preserved
    Word(0xe1a0a000u | result, SavedResult);
    flags = sub ? Subtract : Add;
    if (update_x) {
      // No admitted instruction reads X. A following X-producing operation
      // can discard this computation entirely; logical/address ops retain r9.
      pending_x = flags;
      x_dirty = true;
    }
  }
  void MaterializeX() {
    if (pending_x == None) return;
    Word(0xe1a0baa9, SavedX, SavedNZCV); Word(0xe20bbc01, SavedX, SavedX);
    if (pending_x == Subtract) Word(0xe22bbc01, SavedX, SavedX);
    pending_x = None;
  }
};
} // namespace m68ka9
