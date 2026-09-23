/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace sh2a9 {
// First tier of the A32 emitter. r7 is the canonical guest register array;
// r8/r9 remain PC/cycles. Caller must preserve r4-r10 and its return address;
// division uses lr as scratch (r11 remains the private-entry return register).
// Only the guarded-load
// slow edge may call a helper, with explicit publish/reload of resident state.
// Other calls, branches, or unsupported SR writes require Flush(). DIV1
// retains Q/T across supported region operations, publishing before helper-capable
// loads, loop observation points or Flush; the interrupt mask is unchanged. Encodings:
// ARM DDI 0406B, A5 data processing and A8 MOV/MOVW/MOVT/LDR/STR.
// Selected by the experimental VITA_SH2_A9_REGIONS compiler option.
class RegisterRegion {
public:
  explicit RegisterRegion(uint32_t low = 0, uint32_t high = 0,
                          bool guarded_loads = false)
      : low_ram(low), high_ram(high), guarded_loads(guarded_loads) {}
  std::vector<uint32_t> code;
  unsigned instructions = 0;

  static bool IsIndirectLoad(uint16_t op) {
    return (op & 0xf008) == 0x6000 && (op & 3) != 3;
  }

  static bool IsDivisionStep(uint16_t op) {
    return (op & 0xf00f) == 0x3004 || (op & 0xf0ff) == 0x4024;
  }

  static bool IsMacOperation(uint16_t op) {
    return (op & 0xf00f) == 0x0007 || (op & 0xf0ff) == 0x000a ||
           (op & 0xf0ff) == 0x001a;
  }
  static bool Supports(uint16_t op) {
    if (IsImmediateLogic(op)) return true;
    if (IsMacOperation(op)) return true;
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) return true;
    if (IsDivisionStep(op)) return true;
    if (IsPredicate(op)) return true;
    if ((op >> 12) == 0xe || (op >> 12) == 7) return true;
    switch (op & 0xf00f) {
    case 0x300c: case 0x3008: case 0x2009: case 0x200a: case 0x200b:
    case 0x6003: case 0x6007: case 0x600b: return true;
    case 0x6009: case 0x600c: case 0x600d: case 0x600e: case 0x600f: return true;
    }
    switch (op & 0xf0ff) {
    case 0x4008: case 0x4018: case 0x4028:
    case 0x4009: case 0x4019: case 0x4029: return true;
    }
    return op == 0x0009;
  }

  static bool IsPcLoad(uint16_t op) { return (op >> 12) == 9 || (op >> 12) == 13; }
  static bool IsImmediateLogic(uint16_t op) {
    return (op >> 8) >= 0xc9 && (op >> 8) <= 0xcb;
  }
  static uint32_t PcLoadAddress(uint16_t op, uint32_t pc) {
    return ((pc + 4) & ((op >> 12) == 13 ? ~3u : ~0u)) +
        (op & 255) * ((op >> 12) == 13 ? 4u : 2u);
  }
  bool CanPcLoad(uint16_t op, uint32_t pc) const {
    if (!IsPcLoad(op) || (pc & 1)) return false;
    const uint32_t address = PcLoadAddress(op, pc);
    const unsigned region = address >> 20;
    return ((region == 2 && low_ram) || (region == 0x60 && high_ram));
  }
  bool Emit(uint16_t op, uint32_t pc = 0) {
    if (!CanEmit(op, pc)) return false; // Unsupported admission must not mutate code.
    const bool pc_load = CanPcLoad(op, pc);
    if (pc_load || CanLoad(op)) {
      ++instructions;
      const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
      const unsigned width = pc_load ? ((op >> 12) == 13 ? 4u : 2u) : 1u << (op & 3);
      const uint32_t address = pc_load ? PcLoadAddress(op, pc) : values[m];
      uint32_t offset = address & 0xfffff;
      if (width == 1) offset ^= 1;
      // Address is proven from guest arithmetic or the non-delay-slot PC,
      // never from assumed immutable
      // RAM contents. The data load itself still executes every invocation.
      const uint32_t host = ((address >> 20) == 2 ? low_ram : high_ram) + offset;
      for (auto &slot : slots) if (slot.guest == int(n)) slot = Slot{};
      known[n] = false;
      const unsigned d = Get(n, false);
      Constant(d, host);
      if (width == 1) code.push_back(0xe1d000d0u | (d << 16) | (d << 12));
      else if (width == 2) code.push_back(0xe1d000f0u | (d << 16) | (d << 12));
      else {
        code.push_back(0xe5900000u | (d << 16) | (d << 12));
        code.push_back(0xe1a00860u | (d << 12) | d);
      }
      slots[d].dirty = true;
      return true;
    }
    if (CanGuardLoad(op)) {
      GuardLoad(op);
      ++instructions;
      return true;
    }
    if (!Supports(op)) return false; // No mutation for unsupported operations.
    ++instructions;
    if (op == 9) return true;
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    if ((op & 0xf00f) == 0x0007) {
      const unsigned a = Get(m, true), b = Get(n, true, a);
      code.push_back(0xe00c0090u | (b << 8) | a); // MUL ip,a,b
      const unsigned d = Get(20, false); // MACL; operands are now consumed
      code.push_back(0xe1a0000cu | (d << 12));
      slots[d].dirty = true;
      // Preserve four-state MUL.L accounting, coalesced at the same observation
      // boundaries as ordinary region cycles rather than adding per multiply.
      extra_cycles += 3;
      return true;
    }
    if ((op & 0xf0ff) == 0x000a || (op & 0xf0ff) == 0x001a) {
      const unsigned s = Get((op & 0x10) ? 20 : 19, true);
      const unsigned d = Get(n, false, s);
      code.push_back(0xe1a00000u | (d << 12) | s);
      slots[d].dirty = true;
      return true;
    }
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) {
      CarryArithmetic(n, m, (op & 15) == 10); return true;
    }
    if ((op & 0xf00f) == 0x3004) { Divide(n, m); return true; }
    if ((op & 0xf0ff) == 0x4024) { RotateCarry(n); return true; }
    if (IsPredicate(op)) { Predicate(op, n, m); return true; }
    if (IsImmediateLogic(op)) {
      // SH-2 immediate logic zero-extends imm8, targets R0, and preserves T.
      // Non-S A32 forms also leave live host condition flags unchanged.
      const unsigned kind = op >> 8, imm = op & 255;
      if (known[0] && !resident_loop) {
        if (kind == 0xc9) values[0] &= imm;
        else if (kind == 0xca) values[0] ^= imm;
        else values[0] |= imm;
      } else {
        const unsigned d = Get(0, true);
        const uint32_t base = kind == 0xc9 ? 0xe2000000u :
                              kind == 0xca ? 0xe2200000u : 0xe3800000u;
        code.push_back(base | (d << 16) | (d << 12) | imm);
        slots[d].dirty = true;
      }
      return true;
    }
    // Defer known values until an actual observer needs them. This removes
    // overwritten constants and arithmetic on constants from runtime code;
    // guest instruction/cycle accounting remains unchanged.
    if (!resident_loop && Fold(op, n, m)) return true;
    if ((op >> 12) == 0xe) {
      const unsigned d = Get(n, false);
      Constant(d, uint32_t(int32_t(int8_t(op))));
      slots[d].dirty = true;
      return true;
    }
    if ((op >> 12) == 7) {
      unsigned d = Get(n, true);
      const int value = static_cast<int8_t>(op);
      code.push_back((value < 0 ? 0xe2400000u : 0xe2800000u) |
                     (d << 16) | (d << 12) | unsigned(value < 0 ? -value : value));
      slots[d].dirty = true;
      return true;
    }
    if ((op & 0xf000) == 0x4000) {
      unsigned d = Get(n, true);
      unsigned amount = (op & 0x30) == 0 ? 2 : (op & 0x30) == 0x10 ? 8 : 16;
      code.push_back(0xe1a00000u | (d << 12) | (amount << 7) |
                     ((op & 1) ? 0x20u : 0u) | d);
      slots[d].dirty = true;
      return true;
    }
    unsigned s = Get(m, true);
    const unsigned kind = op & 0xf00f;
    const bool binary = (op >> 12) != 6;
    unsigned d = Get(n, binary, s);
    uint32_t word = 0;
    switch (kind) {
    case 0x300c: word = 0xe0800000u; break; // ADD
    case 0x3008: word = 0xe0400000u; break; // SUB
    case 0x2009: word = 0xe0000000u; break; // AND
    case 0x200a: word = 0xe0200000u; break; // EOR
    case 0x200b: word = 0xe1800000u; break; // ORR
    case 0x6003: word = 0xe1a00000u; break; // MOV
    case 0x6007: word = 0xe1e00000u; break; // MVN
    case 0x600b: word = 0xe2600000u; break; // RSB #0
    case 0x6009: word = 0xe1a00860u; break; // ROR #16: SWAP.W
    case 0x600c: word = 0xe6ef0070u; break; // UXTB
    case 0x600d: word = 0xe6ff0070u; break; // UXTH
    case 0x600e: word = 0xe6af0070u; break; // SXTB
    case 0x600f: word = 0xe6bf0070u; break; // SXTH
    }
    if (kind == 0x600b) code.push_back(word | (s << 16) | (d << 12));
    else code.push_back(word | (binary ? d << 16 : 0) | (d << 12) | s);
    slots[d].dirty = true;
    return true;
  }

  void Flush() {
    PublishSR();
    for (unsigned guest = 0; guest < known.size(); ++guest)
      if (known[guest]) (void)Get(guest, true);
    for (unsigned h = 0; h < slots.size(); ++h) {
      Store(h);
      slots[h] = Slot{};
    }
  }

  bool CanEmit(uint16_t op, uint32_t pc = 0) const {
    return Supports(op) || CanLoad(op) || CanGuardLoad(op) || CanPcLoad(op, pc);
  }

  unsigned MaxEmissionWords(uint16_t op) const {
    if ((op & 0xf00f) == 0x0007) return 16;
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) return 16;
    if ((op & 0xf00f) == 0x3004) return 48;
    if ((op & 0xf0ff) == 0x4024) return 16;
    return unsigned(sr_dirty) +
      (!CanLoad(op) && CanGuardLoad(op) ? 128 : IsPredicate(op) ? 16 : 7);
  }

  // Finish a non-observing straight-line region. Advance publishes all cycles,
  // including the additional three states per MUL.L.
  // Only call once.
  // Chunk immediates so every emitted ADD uses an unrotated 8-bit constant.
  void Finish() {
    Flush();
    Advance();
  }

  // Register-only self-loop body for the existing r7/r8/r9 entry contract.
  // No callbacks, memory accesses, delay slots or non-T SR changes are admitted.
  // Every back edge observes exitcount and pending interrupt level, exactly at
  // the old block boundary. Registers are published only on loop exit.
  // Not a general block-link ABI: callers must retain source ownership and
  // must not use this body for single-step execution.
  static std::vector<uint32_t> ResidentLoop(const std::vector<uint16_t>& body,
                                             uint16_t branch) {
    if (body.empty() || body.size() > 64 ||
        ((branch >> 8) != 0x89 && (branch >> 8) != 0x8b) ||
        int(body.size() * 2) + 4 + int(int8_t(branch)) * 2 != 0)
      return {};
    uint32_t mask = 0;
    for (uint16_t op : body) {
      if (!Supports(op)) return {};
      if ((op & 0xf00f) == 0x0007 || (op & 0xf0ff) == 0x000a ||
          (op & 0xf0ff) == 0x001a) return {}; // GPR-only loop allocation.
      if (IsDivisionStep(op)) return {}; // Deferred SR remains outside loop tier.
      if (op == 9 || op == 8 || op == 0x18) continue;
      if ((op >> 8) == 0x88 || (op >> 8) == 0xc8 || IsImmediateLogic(op)) {
        mask |= 1; continue;
      }
      mask |= 1u << ((op >> 8) & 15);
      if ((op >> 12) == 2 || (op >> 12) == 3 || (op >> 12) == 6)
        mask |= 1u << ((op >> 4) & 15);
    }
    if (__builtin_popcount(mask) > 7) return {};
    RegisterRegion region;
    region.resident_loop = true;
    unsigned host = 0;
    for (unsigned guest = 0; guest < 16; ++guest) if (mask & (1u << guest)) {
      region.slots[host] = {int(guest), false, 0};
      region.code.push_back(0xe5970000u | (host << 12) | (guest * 4));
      ++host;
    }
    const size_t head = region.code.size();
    for (uint16_t op : body) region.Emit(op);
    region.PublishSR(); // Back-edge condition and IRQ checks observe SR.
    auto &out = region.code;
    // SH-2 Programming Manual BF/BT: 3 states taken, 1 not taken.
    out.push_back(0xe2899000u | unsigned(body.size() + 1));
    out.push_back(0xe597a040u); // SR
    out.push_back(0xe31a0001u); // TST T
    const unsigned not_taken = (branch >> 8) == 0x89 ? 0 : 1;
    out.push_back((not_taken << 28) | 0x02888000u | unsigned((body.size() + 1) * 2));
    const size_t fallthrough = out.size(); out.push_back(0);
    out.push_back(0xe2899002u);
    out.push_back(0xe597c080u); // existing tagSH2::exitcount
    out.push_back(0xe159000cu); // unsigned cycle deadline
    const size_t deadline = out.size(); out.push_back(0);
    out.push_back(0xe20aa0f0u); // interrupt mask
    out.push_back(0xe597c060u); // pending interrupt level
    out.push_back(0xe15a000cu);
    const size_t repeat = out.size(); out.push_back(0);
    const size_t exit = out.size();
    region.Flush();
    auto patch = [&](size_t at, size_t target, unsigned condition) {
      out[at] = (condition << 28) | 0x0a000000u |
        (uint32_t(int32_t(target) - int32_t(at) - 2) & 0x00ffffffu);
    };
    // ARM DDI0406B A8.6.16: A32 B displacement is relative to PC+8.
    patch(fallthrough, exit, not_taken);
    patch(deadline, exit, 2); // HS
    patch(repeat, head, 2);  // masked interrupt: another resident iteration
    return out;
  }

private:
  void RotateCarry(unsigned n) {
    const unsigned d = Get(n, true);
    if (!sr_dirty) code.push_back(0xe597a040u);
    code.push_back(0xe20ac001u); // old T
    code.push_back(0xe1b00080u | (d << 12) | d); // LSLS Rn,#1; C=old bit31
    code.push_back(0xe180000cu | (d << 16) | (d << 12)); // insert old T
    code.push_back(0xe3caa001u);
    code.push_back(0x238aa001u); // ORRCS SR,#1; preserve Q/M
    sr_dirty = true;
    slots[d].dirty = true;
  }
  void PublishSR() {
    if (!sr_dirty) return;
    code.push_back(0xe587a040u);
    sr_dirty = false;
  }
  void CarryArithmetic(unsigned n, unsigned m, bool subtract) {
    const unsigned s = Get(m, true), d = Get(n, true, s);
    if (!sr_dirty) code.push_back(0xe597a040u);
    // SH-2 T is carry for ADDC but borrow for SUBC. ARM SBC consumes
    // NOT-borrow, so invert T both when entering and leaving that operation.
    if (subtract) code.push_back(0xe22ac001u); // EOR ip,SR,#1
    code.push_back(subtract ? 0xe1b0c0acu : 0xe1b0c0aau); // LSRS ip,src,#1
    code.push_back((subtract ? 0xe0d00000u : 0xe0b00000u) |
                   (d << 16) | (d << 12) | s); // SBCS / ADCS
    code.push_back(0xe3caa001u); // BIC SR,#1, preserving arithmetic flags
    code.push_back(subtract ? 0x338aa001u : 0x238aa001u); // ORRCC / ORRCS
    sr_dirty = true;
    slots[d].dirty = true;
  }
  void Divide(unsigned n, unsigned m) {
    const unsigned s = Get(m, true), d = Get(n, true, s);
    // SH-2 DIV1 pp.87-88. Preserve sign/old Q before shifting; when n==m
    // the source host register is also d, so the arithmetic reads shifted Rn.
    if (!sr_dirty) code.push_back(0xe597a040u); // SR
    code.push_back(0xe02ae0aau); // EOR lr,SR,SR,LSR #1
    code.push_back(0xe7e0e45eu); // UBFX lr,lr,#8,#1
    code.push_back(0xe24ee001u); // subtract mask: -1 or 0
    code.push_back(0xe1a0cfa0u | d); // MOV ip,Rn,LSR #31
    code.push_back(0xe3caac01u); // BIC SR,#256
    code.push_back(0xe18aa40cu); // ORR SR,ip,LSL #8 (temporary Q = sign)
    code.push_back(0xe20ac001u);
    code.push_back(0xe18c0080u | (d << 12) | d); // Rn = T | Rn<<1
    code.push_back(0xe020c00eu | (s << 16)); // operand XOR subtract mask
    code.push_back(0xe1b0e0aeu); // LSRS lr,lr,#1: carry-in=subtract
    code.push_back(0xe0b0000cu | (d << 16) | (d << 12)); // ADCS
    code.push_back(0xe3a0c000u);
    code.push_back(0xe2acc000u); // ADC ip,ip,#0: arithmetic carry
    code.push_back(0xe02cc00eu); // convert NOT-borrow to borrow
    code.push_back(0xe02cc42au); // XOR temporary Q
    code.push_back(0xe02cc4aau); // XOR M
    code.push_back(0xe20cc001u);
    code.push_back(0xe3caac01u);
    code.push_back(0xe3caa001u);
    code.push_back(0xe18aa40cu);
    code.push_back(0xe02cc4aau);
    code.push_back(0xe20cc001u);
    code.push_back(0xe22cc001u);
    code.push_back(0xe18aa00cu);
    sr_dirty = true;
    slots[d].dirty = true;
  }
  void Advance() {
    for (unsigned left = instructions - accounted; left;) {
      const unsigned n = left > 127 ? 127 : left;
      code.push_back(0xe2888000u | (n * 2));
      code.push_back(0xe2899000u | n);
      left -= n;
    }
    accounted = instructions;
    while (extra_cycles) {
      const unsigned n = extra_cycles > 255 ? 255 : extra_cycles;
      code.push_back(0xe2899000u | n);
      extra_cycles -= n;
    }
  }

  struct Slot { int guest = -1; bool dirty = false; uint64_t used = 0; };
  std::array<Slot, 7> slots{};
  // Canonical word indices: GPR 0..15, MACH 19, MACL 20. The control-register
  // holes are never allocated; SR retains its separate r10 ownership.
  std::array<bool, 21> known{};
  std::array<uint32_t, 21> values{};
  uint32_t low_ram, high_ram;
  bool guarded_loads;
  bool resident_loop = false;
  bool sr_dirty = false;
  unsigned accounted = 0;
  unsigned extra_cycles = 0;
  uint64_t clock = 0;
  static bool IsPredicate(uint16_t op) {
    switch (op & 0xf00f) {
    case 0x3000: case 0x3002: case 0x3003: case 0x3006: case 0x3007:
    case 0x2008: return true;
    }
    switch (op & 0xf0ff) {
    case 0x4010: case 0x4011: case 0x4015: case 0x0029: return true;
    }
    return op == 0x0008 || op == 0x0018 || (op >> 8) == 0x88 || (op >> 8) == 0xc8;
  }
  void SetT(bool value) {
    if (!sr_dirty) code.push_back(0xe597a040u);
    code.push_back(value ? 0xe38aa001u : 0xe3caa001u);
    sr_dirty = true;
  }
  void Predicate(uint16_t op, unsigned n, unsigned m) {
    if (op == 8 || op == 0x18) { SetT(op == 0x18); return; }
    if ((op & 0xf0ff) == 0x0029) { // MOVT
      const unsigned d = Get(n, false);
      if (!sr_dirty) code.push_back(0xe597a040u);
      code.push_back(0xe20a0001u | (d << 12));
      slots[d].dirty = true;
      return;
    }
    unsigned condition = 0; // EQ
    if ((op & 0xf0ff) == 0x4010) { // DT: decrement then test zero
      if (known[n]) { --values[n]; SetT(values[n] == 0); return; }
      const unsigned d = Get(n, true);
      code.push_back(0xe2500001u | (d << 16) | (d << 12)); // SUBS
      slots[d].dirty = true;
    } else if ((op & 0xf0ff) == 0x4011 || (op & 0xf0ff) == 0x4015) {
      const bool positive = (op & 0xff) == 0x15;
      if (known[n]) {
        SetT(positive ? int32_t(values[n]) > 0 : int32_t(values[n]) >= 0);
        return;
      }
      code.push_back(0xe3500000u | (Get(n, true) << 16));
      condition = positive ? 12 : 10;
    } else if ((op >> 8) == 0x88 || (op >> 8) == 0xc8) {
      const bool equal = (op >> 8) == 0x88;
      const uint32_t imm = equal ? uint32_t(int32_t(int8_t(op))) : op & 255;
      if (known[0]) { SetT(equal ? values[0] == imm : !(values[0] & imm)); return; }
      const unsigned s = Get(0, true);
      if (equal) {
        Constant(12, imm);
        code.push_back(0xe150000cu | (s << 16));
      } else code.push_back(0xe3100000u | (s << 16) | imm);
    } else {
      const unsigned kind = op & 0xf00f;
      if (known[n] && known[m]) {
        const uint32_t a = values[n], b = values[m];
        bool result = false;
        switch (kind) {
        case 0x3000: result = a == b; break;
        case 0x3002: result = a >= b; break;
        case 0x3003: result = int32_t(a) >= int32_t(b); break;
        case 0x3006: result = a > b; break;
        case 0x3007: result = int32_t(a) > int32_t(b); break;
        case 0x2008: result = !(a & b); break;
        }
        SetT(result);
        return;
      }
      const unsigned s = Get(m, true), d = Get(n, true, s);
      code.push_back((kind == 0x2008 ? 0xe1100000u : 0xe1500000u) | (d << 16) | s);
      switch (kind) {
      case 0x3002: condition = 2; break; // unsigned >=
      case 0x3003: condition = 10; break; // signed >=
      case 0x3006: condition = 8; break; // unsigned >
      case 0x3007: condition = 12; break; // signed >
      }
    }
    // None of these instructions changes ARM flags before the conditional ORR.
    if (!sr_dirty) code.push_back(0xe597a040u);
    code.push_back(0xe3caa001u);
    code.push_back((condition << 28) | 0x038aa001u);
    sr_dirty = true;
  }
  bool CanGuardLoad(uint16_t op) const {
    return guarded_loads && low_ram && high_ram && IsIndirectLoad(op);
  }

  void GuardLoad(uint16_t op) {
    PublishSR(); // r10 is guard scratch; slow callbacks can observe SR.
    // A helper can observe canonical state. Materialize pending constants,
    // but keep register mappings live on the successful RAM path.
    for (unsigned g = 0; g < known.size(); ++g)
      if (known[g]) (void)Get(g, true);
    Advance(); // Match the old template's live PC/cycles at the callback.
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    const unsigned width = 1u << (op & 3);
    const unsigned s = Get(m, true), d = Get(n, false, s);
    auto jump = [&](unsigned at, unsigned target, uint32_t cond) {
      code[at] = cond | 0x0a000000u |
        (uint32_t(int32_t(target) - int32_t(at) - 2) & 0xffffffu);
    };
    code.push_back(0xe1a0aa20u | s); // mov r10,source,lsr #20
    code.push_back(0xe35a0060u);
    const unsigned high = code.size(); code.push_back(0);
    code.push_back(0xe35a0002u);
    const unsigned slow = code.size(); code.push_back(0);
    Constant(12, low_ram);
    const unsigned base = code.size(); code.push_back(0);
    jump(high, code.size(), 0);
    Constant(12, high_ram);
    jump(base, code.size(), 0xe0000000u);
    code.push_back(0xe3c0a4ffu | (s << 16));
    code.push_back(0xe3caa60fu); // BIC r10,r10,#0x00f00000
    unsigned alignment = 0;
    if (width > 1) {
      code.push_back(0xe31a0000u | (width - 1));
      alignment = code.size(); code.push_back(0);
    }
    if (width == 1) {
      code.push_back(0xe22aa001u);
      code.push_back(0xe19c00dau | (d << 12));
    } else if (width == 2) code.push_back(0xe19c00fau | (d << 12));
    else {
      code.push_back(0xe79c000au | (d << 12));
      code.push_back(0xe1a00860u | (d << 12) | d);
    }
    const unsigned done = code.size(); code.push_back(0);
    jump(slow, code.size(), 0x10000000u);
    if (alignment) jump(alignment, code.size(), 0x10000000u);
    // Slow edge publishes dirty values before calling and reloads all resident
    // mappings afterwards. r10 holds the result while r0-r6 are reconstructed.
    for (unsigned h = 0; h < slots.size(); ++h) Store(h);
    code.push_back(0xe1a00000u | s);
    code.push_back(0xe597a000u | (width == 1 ? 100 : width == 2 ? 104 : 108));
    code.push_back(0xe12fff3au);
    code.push_back(width == 1 ? 0xe6afa070u : width == 2 ? 0xe6bfa070u : 0xe1a0a000u);
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest >= 0)
        code.push_back(0xe5970000u | (h << 12) | (unsigned(slots[h].guest) * 4));
    code.push_back(0xe1a0000au | (d << 12));
    jump(done, code.size(), 0xe0000000u);
    slots[d].dirty = true;
    // SH-1/SH-2 Programming Manual MOVBP/MOVWP/MOVLP: post-increment
    // follows the read, and is suppressed for n == m. On the slow edge s
    // has been reloaded, so callback changes to canonical Rm are preserved.
    if ((op & 4) && n != m) {
      code.push_back(0xe2800000u | (s << 16) | (s << 12) | width);
      slots[s].dirty = true;
    }
  }
  bool CanLoad(uint16_t op) const {
    const unsigned kind = op & 0xf00f;
    if (kind < 0x6000 || kind > 0x6002) return false;
    const unsigned m = (op >> 4) & 15;
    if (!known[m]) return false;
    const uint32_t address = values[m];
    const unsigned region = address >> 20;
    // Match existing cached-RAM callbacks exactly. Aliases and misalignment
    // retain the existing callback path and its cycle/side-effect handling.
    return ((region == 2 && low_ram) || (region == 0x60 && high_ram)) &&
      !(address & ((1u << (op & 3)) - 1));
  }
  bool Fold(uint16_t op, unsigned n, unsigned m) {
    uint32_t value;
    if ((op >> 12) == 0xe) {
      value = uint32_t(int32_t(int8_t(op)));
    } else if ((op >> 12) == 7) {
      if (!known[n]) return false;
      value = values[n] + uint32_t(int32_t(int8_t(op)));
    } else if ((op >> 12) == 4) {
      if (!known[n]) return false;
      const unsigned amount = (op & 0x30) == 0 ? 2 : (op & 0x30) == 0x10 ? 8 : 16;
      value = (op & 1) ? values[n] >> amount : values[n] << amount;
    } else {
      if (!known[m] || ((op >> 12) != 6 && !known[n])) return false;
      const uint32_t source = values[m];
      switch (op & 0xf00f) {
      case 0x300c: value = values[n] + source; break;
      case 0x3008: value = values[n] - source; break;
      case 0x2009: value = values[n] & source; break;
      case 0x200a: value = values[n] ^ source; break;
      case 0x200b: value = values[n] | source; break;
      case 0x6003: value = source; break;
      case 0x6007: value = ~source; break;
      case 0x600b: value = 0u - source; break;
      case 0x6009: value = (source << 16) | (source >> 16); break;
      case 0x600c: value = source & 255; break;
      case 0x600d: value = source & 65535; break;
      case 0x600e: value = (source & 127) - (source & 128); break;
      case 0x600f: value = (source & 32767) - (source & 32768); break;
      default: return false;
      }
    }
    // A previous host mapping is dead, not an observable writeback.
    for (auto &slot : slots) if (slot.guest == int(n)) slot = Slot{};
    known[n] = true;
    values[n] = value;
    return true;
  }
  void Store(unsigned h) {
    if (slots[h].dirty)
      code.push_back(0xe5870000u | (h << 12) | (unsigned(slots[h].guest) * 4));
  }
  unsigned Get(unsigned guest, bool load, unsigned pinned = 7) {
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest == int(guest)) { slots[h].used = ++clock; return h; }
    unsigned victim = 7;
    for (unsigned h = 0; h < slots.size(); ++h) {
      if (h == pinned) continue;
      if (victim == 7 || slots[h].used < slots[victim].used) victim = h;
    }
    Store(victim);
    slots[victim] = {int(guest), false, ++clock};
    if (load && known[guest]) {
      Constant(victim, values[guest]);
      slots[victim].dirty = true;
    } else if (load) {
      code.push_back(0xe5970000u | (victim << 12) | (guest * 4));
    }
    known[guest] = false;
    return victim;
  }
  void Constant(unsigned h, uint32_t value) {
    if (value <= 255) { code.push_back(0xe3a00000u | (h << 12) | value); return; }
    if (~value <= 255) { code.push_back(0xe3e00000u | (h << 12) | ~value); return; }
    code.push_back(0xe3000000u | (h << 12) | ((value & 0xf000) << 4) | (value & 0xfff));
    if (value >> 16)
      code.push_back(0xe3400000u | (h << 12) | ((value >> 12) & 0xf0000) | ((value >> 16) & 0xfff));
  }
};
}
