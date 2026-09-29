/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
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
  struct Fixup { unsigned at, target; uint32_t cond; };
  std::vector<uint32_t> cold;              // slow edges, appended by Finish
  std::vector<Fixup> to_cold, to_hot;      // hot->cold / cold->hot branches
  unsigned instructions = 0;

  static bool IsIndirectLoad(uint16_t op) {
    return (op & 0xf008) == 0x6000 && (op & 3) != 3;
  }
  // MOV.L @(disp,Rm),Rn (5nmd), MOV.W @(disp,Rm),R0 (85md), MOV.B @(disp,Rm),R0
  // (84md): SH-1/SH-2 Programming Manual MOV (disp) forms, 1 cycle, no side
  // effects besides the read. Disabled unless explicitly enabled per region.
  static bool IsDisplacementLoad(uint16_t op) {
    return (op >> 12) == 5 || (op >> 8) == 0x85 || (op >> 8) == 0x84;
  }
  // MOV.B/W/L @(disp,GBR),R0 (C4dd-C6dd): the same read with GBR as base
  // and disp scaled by the width (at most 1020).
  static bool IsGbrLoad(uint16_t op) {
    return (op >> 8) == 0xc4 || (op >> 8) == 0xc5 || (op >> 8) == 0xc6;
  }
  bool gbr_loads = true;                   // per region: see the MAC-region pass
  // MOV.B/W/L @(R0,Rm),Rn (0nmC-E), enabled with the displacement loads.
  static bool IsIndexedLoad(uint16_t op) {
    return (op >> 12) == 0 && (op & 15) >= 0xc && (op & 15) <= 0xe;
  }
  // SHLL/SHAL, SHLR, SHAR: one-bit shifts whose shifted-out bit becomes T.
  static bool IsShiftT(uint16_t op) {
    const unsigned kind = op & 0xf0ff;
    return kind == 0x4000 || kind == 0x4020 || kind == 0x4001 || kind == 0x4021;
  }
#ifdef VITA_SH2_DISP_LOADS
  static inline bool disp_loads_enabled = true;
#else
  static inline bool disp_loads_enabled = false;
#endif
#ifdef VITA_SH2_GBR_LOADS
  static inline bool gbr_loads_enabled = true;
#else
  static inline bool gbr_loads_enabled = false;
#endif
  // r11 holds the high work-RAM host base once a guard in this region set it.
  // Blocks save/restore r11 (prologue push {r3-r11,lr}); callbacks preserve
  // it (AAPCS callee-saved). Set before the guard's first branch, so every
  // later guard in the straight-line hot code finds it loaded.
  // DIV1 chains: from the first DIV1 until the next other operation (or
  // Flush), T and the next DIV1's operation live in host flags/lr instead of
  // the SR word, so each ROTCL/DIV1 is a few flag-chained instructions.
#ifdef VITA_SH2_DIV_CHAIN
  static inline bool div_chain_enabled = true;
#else
  static inline bool div_chain_enabled = false;
#endif
#ifdef VITA_SH2_BASE_REG
  static inline bool base_reg_enabled = true;
#else
  static inline bool base_reg_enabled = false;
#endif
  bool base11 = false;
  // r10 = s ^ region_base; branch to a cold path unless r10 < 0x100000 and
  // (width > 1) r10 is width-aligned. For width 2/4, ROR moves the alignment
  // bits above bit 29/30, so one unsigned compare tests both: with
  // ip = r10 ROR k (k = log2 width), ip < (0x100000 >> k) exactly when bits
  // 20..31 and 0..k-1 of r10 are all clear. Appends the branch sources.
  void RangeAlignCheck(unsigned width, std::vector<unsigned> &sources, std::vector<uint32_t> *conds,
                       uint32_t cs_cond = 0x20000000u) {
    if (width == 1) {
      code.push_back(0xe35a0601u);                          // CMP r10,#0x100000
    } else {
      const unsigned k = width == 4 ? 2 : 1;
      code.push_back(0xe1a0c06au | (k << 7));               // MOV ip,r10,ROR #k
      code.push_back(width == 4 ? 0xe35c0701u : 0xe35c0702u); // CMP ip,#0x40000 / #0x80000
    }
    sources.push_back(code.size());
    if (conds) conds->push_back(cs_cond);
    code.push_back(conds ? 0u : cs_cond);                   // BHS
  }
  // ---- Stack (R15) speculation -------------------------------------------
  // The compiler validates, once at BLOCK ENTRY (before any instruction of the
  // block executes): R15 in [0x06000400, 0x060FC400), 4-aligned, and the three
  // KiBs around it free of compiled code; r11 = host address of that R15. A
  // failed check returns to the dispatcher with nothing executed (exact) and
  // the block is recompiled without speculation. It also plans R15's offset
  // from its entry value at every instruction of the block (pushes, pops and
  // ADD #imm tracked; any other R15 write ends the plan; see R15Effect).
  // Memory helpers never change the executing CPU's registers (SMPC CPU
  // commands run from SmpcExec between slices), so the plan holds. R15-based
  // accesses whose offset stays within +-1020 bytes then use [r11,#offset]
  // without a guard: cached high RAM, aligned (checked here), no store can
  // reach compiled code. Blocks save r11 (push {r3-r11,lr}).
#ifdef VITA_SH2_STACK_SPEC
  static inline bool stack_spec_enabled = true;
#else
  static inline bool stack_spec_enabled = false;
#endif
  // Plan lookup: true and *off = R15 - R15(entry) before the instruction at pc.
  bool (*spec_plan)(void *ctx, uint32_t pc, int *off) = nullptr;
  void *spec_ctx = nullptr;
  // ADD/SUB rd, rn, #|v| with |v| < 1024 in encodable chunks (low byte, then bits 8-9).
  void AddSmall(unsigned rd, unsigned rn, int v) {
    const uint32_t opc = v < 0 ? 0xe2400000u : 0xe2800000u;
    unsigned a = unsigned(v < 0 ? -v : v);
    if (!a) { if (rd != rn) code.push_back(0xe1a00000u | (rd << 12) | rn); return; }
    const unsigned lo = a & 0xFF, hi = a & 0x300;
    if (lo) { code.push_back(opc | (rn << 16) | (rd << 12) | lo); rn = rd; }
    if (hi) code.push_back(opc | (rn << 16) | (rd << 12) | 0xC00 | (hi >> 8)); // imm8 ror 24
  }
  struct SpecAccess { bool ok = false, store = false; unsigned width = 0, reg = 0; int disp = 0, delta = 0; };
  // SpecAccess::reg for PR (tagSH2 SysReg, never held in a region slot).
  static constexpr unsigned kSpecPr = 16, kPrOffset = 84;
  // STS.L PR/MACH/MACL,@-R15 and LDS.L @R15+,PR/MACH/MACL: admitted only as
  // planned stack accesses.
  static bool IsStackPr(uint16_t op) {
    return op == 0x4F22 || op == 0x4F26 || op == 0x4F02 || op == 0x4F06 || op == 0x4F12 || op == 0x4F16;
  }
  bool CanStackPr(uint16_t op, uint32_t pc) const {
    int off;
    return stack_spec_enabled && spec_plan && IsStackPr(op) && spec_plan(spec_ctx, pc, &off) &&
           SpecOffsetOk(SpecDecode(op), off + SpecDecode(op).disp);
  }
  // R15-based forms: 6nmX/5nmd/85md/84md loads (m=15, not indexed), 2nmX
  // (plain/pre-dec), 1nmd, 80nd/81nd stores (n=15).
  static SpecAccess SpecDecode(uint16_t op) {
    SpecAccess a;
    const unsigned hi = op >> 12, n = (op >> 8) & 15, m = (op >> 4) & 15, lo = op & 15;
    if (hi == 6 && m == 15 && lo <= 6 && lo != 3) {
      a.width = 1u << (lo & 3); a.reg = n; a.ok = true;
      if (lo & 4) { if (n == 15) a.ok = false; else a.delta = int(a.width); }
    } else if (hi == 5 && m == 15) { a.width = 4; a.reg = n; a.disp = int(lo) * 4; a.ok = true; }
    else if ((op >> 8) == 0x85 && m == 15) { a.width = 2; a.reg = 0; a.disp = int(lo) * 2; a.ok = true; }
    else if ((op >> 8) == 0x84 && m == 15) { a.width = 1; a.reg = 0; a.disp = int(lo); a.ok = true; }
    else if (hi == 2 && n == 15 && (lo <= 2 || (lo >= 4 && lo <= 6))) {
      a.store = true; a.width = 1u << (lo & 3); a.reg = m; a.ok = true;
      if (lo & 4) { a.disp = -int(a.width); a.delta = -int(a.width); }
    } else if (hi == 1 && n == 15) { a.store = true; a.width = 4; a.reg = m; a.disp = int(lo) * 4; a.ok = true; }
    else if ((op >> 8) == 0x80 && m == 15) { a.store = true; a.width = 1; a.reg = 0; a.disp = int(lo); a.ok = true; }
    else if ((op >> 8) == 0x81 && m == 15) { a.store = true; a.width = 2; a.reg = 0; a.disp = int(lo) * 2; a.ok = true; }
    else if (op == 0x4F22) { a.store = true; a.width = 4; a.reg = kSpecPr; a.disp = -4; a.delta = -4; a.ok = true; } // STS.L PR,@-R15
    else if (op == 0x4F26) { a.width = 4; a.reg = kSpecPr; a.delta = 4; a.ok = true; }
    else if (op == 0x4F02 || op == 0x4F12) {       // MACH/MACL: canonical words 19/20
      a.store = true; a.width = 4; a.reg = op == 0x4F02 ? 19 : 20; a.disp = -4; a.delta = -4; a.ok = true;
    }
    else if (op == 0x4F06 || op == 0x4F16) { a.width = 4; a.reg = op == 0x4F06 ? 19 : 20; a.delta = 4; a.ok = true; }  // LDS.L @R15+,PR
    return a;
  }
  static bool SpecOffsetOk(const SpecAccess &a, int o) {
    if (o <= -1020 || o >= 1020 || (o & int(a.width - 1))) return false;
    return a.width == 4 || (o > -255 && o < 255);
  }
  // Effect of ANY SH-2 instruction on R15: 0 none, 1 tracked (*delta), 2 other
  // write (plan ends). SH-1/SH-2 Programming Manual instruction formats.
  static int R15Effect(uint16_t op, int *delta) {
    const unsigned hi = op >> 12, n = (op >> 8) & 15, m = (op >> 4) & 15, lo = op & 15;
    *delta = 0;
    // Rm (bits 7-4) writers: MOV.x @Rm+,Rn, MAC.L @Rm+,@Rn+, MAC.W @Rm+,@Rn+.
    if (hi == 6 && lo >= 4 && lo <= 6 && m == 15) {
      if (n == 15) return 2;
      *delta = int(1u << (lo & 3)); return 1;
    }
    if ((hi == 0 || hi == 4) && lo == 0xF && (m == 15 || n == 15)) return 2;
    if ((op >> 8) == 0xC3 || op == 0x002B) return 2;       // TRAPA pushes, RTE pops R15
    if (n != 15) return 0;
    // Rn (bits 11-8) = 15 forms that do not write R15.
    switch (hi) {
    case 0x0:
      if (lo >= 4 && lo <= 7) return 0;                    // indexed stores, MUL.L
      if ((op & 0xff) == 0x2b || (op & 0xff) == 0x23) return 0; // JMP-like, BRAF/BSRF read Rn
      if ((op & 0xff) == 0x03) return 0;                   // BSRF Rn
      break;
    case 0x1: return 0;                                    // MOV.L Rm,@(disp,Rn)
    case 0x2:
      if (lo <= 2 || lo == 8 || lo == 0xC) return 0;       // stores, TST, CMP/STR
      if (lo >= 4 && lo <= 6) { *delta = -int(1u << (lo & 3)); return 1; } // MOV.x Rm,@-R15
      break;
    case 0x3:
      if (lo == 0 || lo == 2 || lo == 3 || lo == 6 || lo == 7) return 0; // CMP
      break;
    case 0x4:
      switch (op & 0xff) {
      case 0x11: case 0x15: case 0x1b: case 0x0b: case 0x2b: return 0; // CMP/PZ, CMP/PL, TAS.B, JSR, JMP
      case 0x0e: case 0x1e: case 0x2e: case 0x0a: case 0x1a: case 0x2a: return 0; // LDC/LDS Rm,x
      case 0x02: case 0x12: case 0x22: case 0x03: case 0x13: case 0x23: *delta = -4; return 1; // STS.L/STC.L x,@-R15
      case 0x06: case 0x16: case 0x26: case 0x07: case 0x17: case 0x27: *delta = 4; return 1;  // LDS.L/LDC.L @R15+,x
      }
      break;
    case 0x7: *delta = int(int8_t(op & 0xff)); return 1;   // ADD #imm,R15
    case 0x8: case 0xC: return 0;                          // bits 11-8 are not a register here
    case 0xA: case 0xB: return 0;                          // BRA/BSR displacement
    }
    return 2;
  }
  // Emits the speculative access for a planned R15 offset, or returns false.
  bool EmitSpecAccess(uint16_t op, int off) {
    const SpecAccess a = SpecDecode(op);
    const int o = off + a.disp;
    if (!a.ok || !SpecOffsetOk(a, o)) return false;
    const unsigned mag = unsigned(o < 0 ? -o : o);
    const uint32_t up = o < 0 ? 0 : (1u << 23);
    auto imm8 = [&](unsigned v) { return ((v >> 4) << 8) | (v & 15); };
    if (a.reg == kSpecPr) {
      if (a.store) {
        code.push_back(0xe597c000u | kPrOffset);                   // LDR ip,[r7,#PR]
        code.push_back(0xe1a0c86cu);                                // ROR ip,ip,#16
        code.push_back(0xe50bc000u | up | mag);                     // STR ip,[r11,#o]
      } else {
        code.push_back(0xe51bc000u | up | mag);                     // LDR ip,[r11,#o]
        code.push_back(0xe1a0c86cu);                                // ROR ip,ip,#16
        code.push_back(0xe587c000u | kPrOffset);                   // STR ip,[r7,#PR]
      }
    } else if (!a.store) {
      const unsigned d = Get(a.reg, false);
      if (a.width == 4) {
        code.push_back(0xe51b0000u | up | (d << 12) | mag);        // LDR d,[r11,#o]
        code.push_back(0xe1a00860u | (d << 12) | d);                // ROR d,d,#16
      } else if (a.width == 2) {
        code.push_back(0xe15b00f0u | up | (d << 12) | imm8(mag));  // LDRSH d,[r11,#o]
      } else {
        const int ob = o ^ 1; const unsigned mb = unsigned(ob < 0 ? -ob : ob);
        code.push_back(0xe15b00d0u | (ob < 0 ? 0 : (1u << 23)) | (d << 12) | imm8(mb)); // LDRSB
      }
      slots[d].dirty = true;
    } else {
      const unsigned v = Get(a.reg, true);
      if (a.width == 4) {
        code.push_back(0xe1a0c860u | v);                            // MOV ip,v,ROR #16
        code.push_back(0xe50bc000u | up | mag);                     // STR ip,[r11,#o]
      } else if (a.width == 2) {
        code.push_back(0xe14b00b0u | up | (v << 12) | imm8(mag));  // STRH v,[r11,#o]
      } else {
        const int ob = o ^ 1; const unsigned mb = unsigned(ob < 0 ? -ob : ob);
        code.push_back(0xe54b0000u | (ob < 0 ? 0 : (1u << 23)) | (v << 12) | mb); // STRB
      }
    }
    if (a.delta) {                                   // SH-2 pre-decrement / post-increment of R15
      const unsigned h15 = Get(15, true);
      AddSmall(h15, h15, a.delta);
      slots[h15].dirty = true;
    }
    return true;
  }
  bool Emit(uint16_t op, uint32_t pc = 0) {
    const bool emitted = EmitOp(op, pc);
    // Constant hints: a known guest value stays a hint after Get materializes
    // it (clearing known) until an operation may write that guest.
    const uint32_t writes = GuestWrites(op);
    for (unsigned g = 0; g < 16; ++g) {
      if (known[g]) { hint_ok[g] = true; hint_val[g] = values[g]; }
      else if (writes >> g & 1) hint_ok[g] = false;
    }
    return emitted;
  }
  // Guests an operation may write: none for stores except MOV.x Rm,@-Rn
  // (Rn); otherwise conservatively both register fields and R0.
  static uint32_t GuestWrites(uint16_t op) {
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    if (IsStore(op)) return (op >> 12) == 2 && (op & 4) ? 1u << n : 0u;
    const unsigned top = op >> 12;
    if (top == 3 || top == 7 || top == 0xe) return 1u << n;          // ALU/ADD #imm/MOV #imm: Rn
    if (top == 6) return (op & 0xf) >= 4 && (op & 0xf) <= 6 ? (1u << n) | (1u << m) : 1u << n; // @Rm+
    return (1u << n) | (1u << m) | 1u;
  }
  std::array<bool, 16> hint_ok{};
  std::array<uint32_t, 16> hint_val{};
  bool EmitOp(uint16_t op, uint32_t pc) {
    if (div_chain_enabled && !resident_loop) {
      const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
      const bool div = (op & 0xf00f) == 0x3004, rot = (op & 0xf0ff) == 0x4024;
      if (div_open && !ChainAccepts(op) && (div || rot || CanEmit(op, pc))) CloseDivision();
      if (div) { ++instructions; ChainDivide(n, m); return true; }
      if (div_open && rot) { ++instructions; ChainRotate(n); return true; }
    }
    int off;
    if (stack_spec_enabled && spec_plan && CanEmit(op, pc) && SpecDecode(op).ok &&
        spec_plan(spec_ctx, pc, &off) && EmitSpecAccess(op, off)) {
      ++instructions;
      return true;
    }
    return EmitInner(op, pc);
  }
  // r11 holds R15's host address in blocks with an active stack plan; other
  // blocks keep the high-RAM base there. legacy_words: words the r12-base
  // emission (every guard materializing the base, as in stack-plan blocks)
  // would add to this region. The compiler sizes blocks by that emission, so
  // block boundaries, hence interrupt and scheduler observation points, do
  // not depend on which base register a block uses.
  int legacy_words = 0;
  unsigned HighBase() {
    if ((stack_spec_enabled && spec_plan) || !base_reg_enabled) return 12;
    if (!base11) {
      const size_t at = code.size();
      Constant(11, high_ram);
      base11 = true;
      if (stack_spec_enabled) legacy_words -= int(code.size() - at);
    }
    return 11;
  }
  // The base in ip at a guard: materialized unless r11 holds it.
  void HighBaseIp(unsigned hb) {
    if (hb == 12) { Constant(12, high_ram); return; }
    if (stack_spec_enabled) legacy_words += ConstantWords(high_ram);
  }
  // MOV.B/W/L Rm,@Rn (2nm0-2), Rm,@-Rn (2nm4-6), Rm,@(R0,Rn) (0nm4-6),
  // MOV.L Rm,@(disp,Rn) (1nmd), MOV.B/W R0,@(disp,Rn) (80nd, 81nd):
  // SH-1/SH-2 Programming Manual MOV forms, 1 cycle.
  static bool IsStore(uint16_t op) {
    const unsigned low = op & 15;
    return ((op >> 12) == 2 && (low <= 2 || (low >= 4 && low <= 6))) ||
           ((op >> 12) == 0 && low >= 4 && low <= 6) || (op >> 12) == 1 ||
           (op >> 8) == 0x80 || (op >> 8) == 0x81;
  }
  // Host address of a byte map with one entry per 1 KiB of high work RAM,
  // nonzero once any compiled block has claimed an instruction in that
  // KiB. Guarded stores are admitted only when set (see GuardStore).
  uint32_t code_pages = 0;

  static bool IsDivisionStep(uint16_t op) {
    return (op & 0xf00f) == 0x3004 || (op & 0xf0ff) == 0x4024;
  }

  // Operations on MACH/MACL (plus XTRCT, their usual readback partner):
  // MUL.L, DMULS.L/DMULU.L, MULS.W/MULU.W, CLRMAC, STS/LDS MACH/MACL and
  // MAC.L/MAC.W, all admitted together by VITA_SH2_MAC_REGIONS.
  static bool IsMacOperation(uint16_t op) {
    const unsigned kind = op & 0xf00f, reg = op & 0xf0ff;
    return kind == 0x0007 || kind == 0x300d || kind == 0x3005 || kind == 0x200f ||
           kind == 0x200e || kind == 0x200d || IsMacLong(op) || IsMacWord(op) ||
           op == 0x0028 || reg == 0x000a || reg == 0x001a || reg == 0x400a || reg == 0x401a;
  }
  // MAC.L/MAC.W @Rm+,@Rn+ read memory, so they need the guarded-load edges.
  static bool IsMacLong(uint16_t op) { return (op & 0xf00f) == 0x000f; }
  static bool IsMacWord(uint16_t op) { return (op & 0xf00f) == 0x400f; }
  // States the region charges: the baseline table cost of each multiply
  // (SH7604 table: MUL.L/DMULx.L 2-4, MULx.W/MAC.L 1-3 kept at their upper
  // bounds), one for everything else.
  static unsigned Cycles(uint16_t op) {
    const unsigned kind = op & 0xf00f;
    if (kind == 0x0007 || kind == 0x300d || kind == 0x3005) return 4;
    if (kind == 0x200f || kind == 0x200e || kind == 0x000f || kind == 0x400f) return 3;
    return 1;
  }
  static bool Supports(uint16_t op) {
    if (IsImmediateLogic(op)) return true;
    if (IsMacOperation(op)) return !IsMacLong(op) && !IsMacWord(op);
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) return true;
    if (IsDivisionStep(op)) return true;
    if (IsPredicate(op)) return true;
    if (IsShiftT(op)) return true;
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
  bool EmitInner(uint16_t op, uint32_t pc) {
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
    if (CanGuardStore(op)) {
      GuardStore(op);
      ++instructions;
      return true;
    }
    if (CanMacLong(op) || CanMacWord(op)) {
      MacMemory(op, IsMacWord(op));
      ++instructions;
      extra_cycles += Cycles(op) - 1;
      return true;
    }
    if (!Supports(op)) return false; // No mutation for unsupported operations.
    ++instructions;
    // Multiply table costs, coalesced at the same observation boundaries as
    // ordinary region cycles rather than added per multiply.
    extra_cycles += Cycles(op) - 1;
    if (op == 9) return true;
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    if ((op & 0xf00f) == 0x0007) {
      const unsigned a = Get(m, true), b = Get(n, true, a);
      code.push_back(0xe00c0090u | (b << 8) | a); // MUL ip,a,b
      const unsigned d = Get(20, false); // MACL; operands are now consumed
      code.push_back(0xe1a0000cu | (d << 12));
      slots[d].dirty = true;
      return true;
    }
    if (IsMacOperation(op)) { MacRegister(op, n, m); return true; }
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) {
      CarryArithmetic(n, m, (op & 15) == 10); return true;
    }
    if ((op & 0xf00f) == 0x3004) { Divide(n, m); return true; }
    if ((op & 0xf0ff) == 0x4024) { RotateCarry(n); return true; }
    if (IsPredicate(op)) { Predicate(op, n, m); return true; }
    if (IsShiftT(op)) { ShiftT(op, n); return true; }
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
    CloseDivision();
    PublishSR();
    for (unsigned guest = 0; guest < known.size(); ++guest)
      if (known[guest]) (void)Get(guest, true);
    for (unsigned h = 0; h < slots.size(); ++h) {
      Store(h);
      slots[h] = Slot{};
    }
  }

  bool CanEmit(uint16_t op, uint32_t pc = 0) const {
    return Supports(op) || CanLoad(op) || CanGuardLoad(op) || CanPcLoad(op, pc) ||
           CanGuardStore(op) || CanMacLong(op) || CanMacWord(op) || CanStackPr(op, pc);
  }
  // Admitted operations that write T (predicates incl. MOVT, T shifts,
  // DIV0S/DIV1/ROTCL, ADDC/SUBC/ADDV/SUBV).
  static bool WritesT(uint16_t op) {
    return IsPredicate(op) || IsShiftT(op) || IsDivisionStep(op) || (op & 0xf00a) == 0x300a;
  }
  // MOV/MVN or MOVW(+MOVT) of a constant into host register h.
  void LoadConstant(unsigned h, uint32_t value) { Constant(h, value); }
  bool CanGuardStore(uint16_t op) const {
    return guarded_loads && low_ram && high_ram && code_pages && IsStore(op);
  }
  bool CanMacLong(uint16_t op) const {
    return guarded_loads && low_ram && high_ram && macl_helper && IsMacLong(op);
  }
  bool CanMacWord(uint16_t op) const {
    return guarded_loads && low_ram && high_ram && macw_helper && IsMacWord(op);
  }
  // Addresses of sh2_macl_region/sh2_macw_region (dynalib_arm.s); MAC.L and
  // MAC.W are admitted only when set.
  uint32_t macl_helper = 0, macw_helper = 0;
  // OnchipWrite{Byte,Word,Long} (VITA_SH2_ONCHIP_DIRECT); constant on-chip
  // stores call them directly only when set (see OnchipConstantStore).
  uint32_t onchip_write[3] = {0, 0, 0};
  bool cold_owed = false;  // GuardStore would have given this region cold code

  unsigned MaxEmissionWords(uint16_t op) const {
    if (div_open && !ChainAccepts(op)) return 28 + MaxEmissionWordsInner(op); // CloseDivision
    return MaxEmissionWordsInner(op);
  }
  unsigned MaxEmissionWordsInner(uint16_t op) const {
    if (IsMacLong(op) || IsMacWord(op)) return 160;
    if (IsMacOperation(op)) return 16;
    if ((op & 0xf00f) == 0x300e || (op & 0xf00f) == 0x300a) return 16;
    if ((op & 0xf00f) == 0x3004) return 48;
    if ((op & 0xf0ff) == 0x4024) return 16;
    return unsigned(sr_dirty) +
      (!CanLoad(op) && (CanGuardLoad(op) || CanGuardStore(op)) ? 128 :
       IsPredicate(op) ? 16 : 7);
  }

  // Finish a non-observing straight-line region. Advance publishes all cycles,
  // including the additional three states per MUL.L.
  // Only call once.
  // Chunk immediates so every emitted ADD uses an unrotated 8-bit constant.
  void Finish() {
    Flush();
    Advance();
    if (cold.empty() && cold_owed) ++legacy_words;  // the branch over it
    if (cold.empty()) return;
    // Out-of-line slow edges: [hot code][B over][cold stubs], so guarded fast
    // paths stay contiguous. ARM A8.8.18 B: offset = target - (at + 2) words.
    auto branch = [](uint32_t cond, unsigned at, unsigned target) {
      return cond | 0x0a000000u | (uint32_t(int32_t(target) - int32_t(at) - 2) & 0xffffffu);
    };
    const unsigned h = code.size();
    code.push_back(branch(0xe0000000u, h, h + 1 + cold.size()));
    for (const auto &f : to_cold) code[f.at] = branch(f.cond, f.at, h + 1 + f.target);
    for (const auto &f : to_hot) cold[f.at] = branch(f.cond, h + 1 + f.at, f.target);
    code.insert(code.end(), cold.begin(), cold.end());
    cold.clear(); to_cold.clear(); to_hot.clear();
  }
  // Finish without placing the slow edges: cold, to_cold (hot word -> cold
  // index) and to_hot (cold word -> hot index) are left for the compiler,
  // which appends every region's cold code after the block's exits so the
  // block's hot code stays contiguous. Encode branches with Branch().
  void FinishHot() {
    Flush();
    Advance();
    if (cold.empty() && cold_owed) ++legacy_words;  // the caller's branch over it
  }
  static uint32_t Branch(uint32_t cond, unsigned at, unsigned target) {
    return cond | 0x0a000000u | (uint32_t(int32_t(target) - int32_t(at) - 2) & 0xffffffu);
  }
  // Words this region will occupy once finished (hot, branch-over, cold).
  size_t PendingWords() const {
    return code.size() + (cold.empty() && !cold_owed ? 0 : cold.size() + 1) + (div_open ? 28 : 0);
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
      if (IsMacOperation(op)) return {}; // GPR-only loop allocation.
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
  // SH-2 DIV1 (Programming Manual pp.87-88), for any M: with f = borrow of the
  // subtraction or carry of the addition, Q = Qs ^ f ^ M, so T = (Q == M) =
  // !(Qs ^ f), and the next DIV1 subtracts exactly when (Q == M), i.e. when
  // T after this DIV1. In the chain lr = -(Q == M) (0 or -1) and r10 is
  // scratch: SR is in the state word from entry until CloseDivision. T is in
  // the SR word (first operation only), in the ARM carry (after ROTCL), or
  // equals lr's bit (after DIV1). Emitted code between chain operations
  // (Get's loads, stores and constants) never sets flags.
  enum { kTInSR, kTInCarry, kTInLr };
  bool div_open = false;
  unsigned div_t = kTInSR;
  // Generic chain: host registers given, emitted into the current stream.
  void TToCarry() {
    if (div_t == kTInSR) code.push_back(0xe1b0c0aau);      // MOVS ip,r10,LSR #1
    else if (div_t == kTInLr) code.push_back(0xe13e008eu); // TEQ lr,lr,LSL #1
  }
  void GenericOpen() {
    code.push_back(0xe02ae0aau);   // EOR lr,SR,SR,LSR #1: bit 8 = Q ^ M
    code.push_back(0xe7e0e45eu);   // UBFX lr,lr,#8,#1
    code.push_back(0xe24ee001u);   // SUB lr,lr,#1: -(Q == M)
    div_t = kTInSR;
  }
  void GenericRotate(unsigned d) {
    TToCarry();
    code.push_back(0xe0b00000u | (d << 16) | (d << 12) | d); // ADCS Rn,Rn,Rn
    div_t = kTInCarry;
  }
  void GenericDivide(unsigned d, unsigned s) {
    TToCarry();                    // C = T (before r10 is overwritten)
    code.push_back(0xe0b00000u | (d << 16) | (d << 12) | d); // ADCS Rn,Rn,Rn: C = old bit 31
    // Like Divide: when n == m the operand is the shifted Rn.
    code.push_back(0xe02ea000u | s); // EOR r10,lr,Rm (flags unchanged)
    code.push_back(0xe0ccc00cu);   // SBC ip,ip,ip: Qs - 1
    code.push_back(0xe13e008eu);   // TEQ lr,lr,LSL #1: C = subtract
    code.push_back(0xe0b0000au | (d << 16) | (d << 12)); // ADCS Rn,Rn,r10: C = !borrow / carry
    code.push_back(0xe2acc000u);   // ADC ip,ip,#0: bit 0 = !(Qs ^ C)
    code.push_back(0xe02cc00eu);   // EOR ip,ip,lr: bit 0 = T
    code.push_back(0xe7a0e05cu);   // SBFX lr,ip,#0,#1
    div_t = kTInLr;
  }
  void GenericClose() {
    code.push_back(0xe597a040u);   // SR (flags unchanged)
    code.push_back(0xe3caa001u);   // BIC SR,#1
    code.push_back(0xe3caac01u);   // BIC SR,#256
    if (div_t == kTInCarry) {
      code.push_back(0x238aa001u); // ORRCS SR,#1
    } else {
      code.push_back(0xe20ec001u); // AND ip,lr,#1
      code.push_back(0xe18aa00cu); // ORR SR,SR,ip
    }
    code.push_back(0xe02ec4aau);   // EOR ip,lr,SR,LSR #9: bit 0 = (Q == M) ^ M
    code.push_back(0xe22cc001u);   // EOR ip,ip,#1: bit 0 = Q
    code.push_back(0xe20cc001u);   // AND ip,ip,#1
    code.push_back(0xe18aa40cu);   // ORR SR,SR,ip,LSL #8
  }
  // Fast chain: DIV1 Rs,Rr with M = 0, 1 <= Rs <= 2^31, and partial
  // remainder P = (Q ? Rr - 2^32 : Rr) in [-Rs, Rs) at entry, alternating
  // with ROTCL Rq (q, r, s distinct, fixed for the chain). Each step then
  // keeps P in [-Rs, Rs) (non-restoring division), so Q:Rr is P as a 33-bit
  // value, Q = bit 31 of Rr and T = !Q after every DIV1: the step is
  // Rr = 2Rr + t, then Rr += Rs if P < 0 else Rr -= Rs. A ROTCL after a DIV1
  // inserts bit 31 of Rr (= !T) and the chain corrects those bits in Rq with
  // one EOR at the end. Checked once at the first DIV1 (the guard branches
  // to the generic chain, emitted alongside in the cold stream); registers
  // stay resident, and whatever Get emits after the guard is copied to both.
  enum { kFOpen, kFRot, kFDiv };
  bool div_fast = false;
  unsigned f_r = 0, f_s = 0, f_last = kFOpen, f_hr = 0, f_hs = 0, f_hq = 0;
  int f_q = -1;
  uint32_t f_mask = 0;
  bool ChainAccepts(uint16_t op) const {
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    const bool div = (op & 0xf00f) == 0x3004, rot = (op & 0xf0ff) == 0x4024;
    if (!div_fast) return div || rot;
    if (div) return n == f_r && m == f_s;
    // A rotcl whose outgoing bit is still inverted would carry the wrong T out.
    if (rot) return (f_mask >> 31) == 0 && (f_q < 0 ? n != f_r && n != f_s : int(n) == f_q);
    return false;
  }
  unsigned ChainGet(unsigned guest, unsigned pinned = 7) {
    const size_t before = code.size();
    if (div_open && div_fast) slots[f_hr].used = slots[f_hs].used = ++clock; // never evicted
    const unsigned h = Get(guest, true, pinned);
    if (div_open && div_fast) cold.insert(cold.end(), code.begin() + before, code.end());
    return h;
  }
  void ChainRotate(unsigned n) {
    const unsigned d = ChainGet(n);
    if (div_fast) {
      if (f_q < 0) { f_q = int(n); f_hq = d; }
      if (f_last == kFDiv) code.push_back(0xe3500102u | (f_hr << 16)); // CMP Rr,#0x80000000: C = !T
      code.push_back(0xe0b00000u | (d << 16) | (d << 12) | d);         // ADCS Rq,Rq,Rq
      f_mask = (f_mask << 1) | (f_last == kFDiv ? 1u : 0u);
      f_last = kFRot;
      code.swap(cold); GenericRotate(d); code.swap(cold);
    } else {
      GenericRotate(d);
    }
    slots[d].dirty = true;
  }
  void ChainDivide(unsigned n, unsigned m) {
    const unsigned s = ChainGet(m), d = ChainGet(n, s);
    if (!div_open) {
      code.push_back(sr_dirty ? 0xe587a040u : 0xe597a040u); // publish / load SR
      sr_dirty = false;
      div_open = true;
      div_fast = n != m;
      if (div_fast) {
        f_r = n; f_s = m; f_hr = d; f_hs = s; f_q = -1; f_mask = 0; f_last = kFOpen;
        std::vector<unsigned> sources;
        std::vector<uint32_t> conds;
        auto to_cold_if = [&](uint32_t cond) {
          sources.push_back(code.size()); conds.push_back(cond); code.push_back(0);
        };
        code.push_back(0xe31a0c02u);                 // TST SR,#0x200 (M)
        to_cold_if(0x10000000u);                     // BNE
        code.push_back(0xe240c001u | (s << 16));     // SUB ip,Rs,#1
        code.push_back(0xe35c0102u);                 // CMP ip,#0x80000000
        to_cold_if(0x20000000u);                     // BHS: Rs == 0 or Rs > 2^31
        code.push_back(0xe31a0c01u);                 // TST SR,#0x100 (Q)
        code.push_back(0x1a000002u);                 // BNE q1
        code.push_back(0xe1500000u | (d << 16) | s); // CMP Rr,Rs
        to_cold_if(0x20000000u);                     // BHS: P >= Rs
        code.push_back(0xea000001u);                 // B ok
        code.push_back(0xe1700000u | (d << 16) | s); // q1: CMN Rr,Rs
        to_cold_if(0x30000000u);                     // BCC: P < -Rs
        BeginCold(sources, conds);                   // ok:
        GenericOpen();
        code.swap(cold);
      } else {
        GenericOpen();
      }
    }
    if (div_fast) {
      if (f_last == kFOpen) code.push_back(0xe1b0c0aau);    // MOVS ip,SR,LSR #1: C = T
      else if (f_last == kFDiv) {
        code.push_back(0xe1e0c000u | d);                     // MVN ip,Rr
        code.push_back(0xe09cc00cu);                         // ADDS ip,ip,ip: C = T
      }
      code.push_back(0xe0b00000u | (d << 16) | (d << 12) | d); // ADCS Rr,Rr,Rr: C = Q
      code.push_back(0x20800000u | (d << 16) | (d << 12) | s); // ADDCS Rr,Rr,Rs
      code.push_back(0x30400000u | (d << 16) | (d << 12) | s); // SUBCC Rr,Rr,Rs
      f_last = kFDiv;
      code.swap(cold); GenericDivide(d, s); code.swap(cold);
    } else {
      GenericDivide(d, s);
    }
    slots[d].dirty = true;
  }
  void CloseDivision() {
    if (!div_open) return;
    if (div_fast) {
      if (f_mask) {                                  // flags unchanged
        Constant(12, f_mask);
        code.push_back(0xe020000cu | (f_hq << 16) | (f_hq << 12)); // EOR Rq,Rq,ip
      }
      code.push_back(0xe3caa001u);                   // BIC SR,#1
      code.push_back(0xe3caac01u);                   // BIC SR,#256
      if (f_last == kFRot) code.push_back(0x238aa001u); // ORRCS SR,#1
      else {
        code.push_back(0xe1e0c000u | f_hr);          // MVN ip,Rr
        code.push_back(0xe18aafacu);                 // ORR SR,SR,ip,LSR #31: T = !Q
      }
      code.push_back(0xe200c102u | (f_hr << 16));    // AND ip,Rr,#0x80000000
      code.push_back(0xe18aabacu);                   // ORR SR,SR,ip,LSR #23: Q
      const unsigned join = code.size();
      code.swap(cold);
      GenericClose();
      to_hot.push_back({unsigned(code.size()), join, 0xe0000000u});
      code.push_back(0);
      code.swap(cold);
    } else {
      GenericClose();
    }
    div_open = false;
    div_fast = false;
    sr_dirty = true;
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
  // The old template's live PC/cycles at a slow-edge helper call, without
  // committing the region's accounting on the fast path: ADD before the call,
  // SUB after (r8/r9 are callee-saved, so helpers cannot change them).
  // Slow-edge stubs are emitted with the ordinary helpers by swapping the
  // cold buffer into `code`; branch indices are resolved by Finish.
  void BeginCold(const std::vector<unsigned> &sources, const std::vector<uint32_t> &conds) {
    for (size_t i = 0; i < sources.size(); ++i)
      to_cold.push_back({sources[i], unsigned(cold.size()), conds[i]});
    code.swap(cold);
  }
  void ColdJumpHot(unsigned target) {       // B target (hot index), from cold
    to_hot.push_back({unsigned(code.size()), target, 0xe0000000u});
    code.push_back(0);
  }
  void EndCold(unsigned done) {
    to_hot.push_back({unsigned(code.size()), done, 0xe0000000u});
    code.push_back(0);
    code.swap(cold);
  }
  void Pending(uint32_t add_or_sub) {
    for (unsigned left = instructions - accounted; left;) {
      const unsigned k = left > 127 ? 127 : left;
      code.push_back(add_or_sub | 0x00088000u | (k * 2));
      code.push_back(add_or_sub | 0x00099000u | k);
      left -= k;
    }
    for (unsigned left = extra_cycles; left;) {
      const unsigned k = left > 255 ? 255 : left;
      code.push_back(add_or_sub | 0x00099000u | k);
      left -= k;
    }
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
  void ShiftT(uint16_t op, unsigned n) {
    const bool right = op & 1, arithmetic = (op & 0x20) != 0;
    if (known[n]) {
      const uint32_t v = values[n];
      values[n] = !right ? v << 1 : arithmetic ? uint32_t(int32_t(v) >> 1) : v >> 1;
      SetT(right ? (v & 1) : (v >> 31));
      return;
    }
    const unsigned d = Get(n, true);
    // MOVS d,d,LSL/LSR/ASR #1: C is the shifted-out bit (ARM A8.4.1).
    code.push_back((!right ? 0xe1b00080u : arithmetic ? 0xe1b000c0u : 0xe1b000a0u) |
                   (d << 12) | d);
    slots[d].dirty = true;
    if (!sr_dirty) code.push_back(0xe597a040u);
    code.push_back(0xe3caa001u);                 // BIC r10,r10,#1
    code.push_back(0x238aa001u);                 // ORRCS r10,r10,#1
    sr_dirty = true;
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
    return guarded_loads && low_ram && high_ram &&
      (IsIndirectLoad(op) ||
       (disp_loads_enabled && (IsDisplacementLoad(op) || IsIndexedLoad(op))) ||
       (gbr_loads_enabled && gbr_loads && IsGbrLoad(op)));
  }

  void GuardLoad(uint16_t op) {
    PublishSR(); // r10 is guard scratch; slow callbacks can observe SR.
    // A helper can observe canonical state. Materialize pending constants,
    // but keep register mappings live on the successful RAM path.
    for (unsigned g = 0; g < known.size(); ++g)
      if (known[g]) (void)Get(g, true);
    const bool displacement = IsDisplacementLoad(op), indexed = IsIndexedLoad(op), gbr = IsGbrLoad(op);
    const unsigned n = (displacement && (op >> 12) == 8) || gbr ? 0 : (op >> 8) & 15, m = (op >> 4) & 15;
    const unsigned width = displacement ? ((op >> 12) == 5 ? 4u : (op >> 8) == 0x85 ? 2u : 1u)
                         : gbr ? 1u << ((op >> 8) & 3)
                         : indexed ? 1u << ((op & 15) - 0xc) : 1u << (op & 3);
    const unsigned base_reg = Get(gbr ? 17 : m, true);   // guest 17: GBR
    // Address register for the guard/callback: Rm itself, or LR = Rm + disp
    // or Rm + R0, formed before the destination can take a host register.
    // LR is saved by the block prologue and already clobbered by BLX callbacks.
    unsigned s = base_reg;
    if (displacement) {
      code.push_back(0xe280e000u | (base_reg << 16) | ((op & 15) * width)); // ADD lr,Rm,#disp
      s = 14;
    } else if (gbr) {
      const unsigned v = (op & 0xff) * width;
      if (v <= 0xff) {
        code.push_back(0xe280e000u | (base_reg << 16) | v);                 // ADD lr,GBR,#disp
      } else if (width == 4) {
        code.push_back(0xe280ef00u | (base_reg << 16) | (v >> 2));          // ADD lr,GBR,#disp (ROR 30)
      } else {
        code.push_back(0xe280e000u | (base_reg << 16) | (v & 0xff));        // ADD lr,GBR,#disp&0xff
        code.push_back(0xe28eec00u | (v >> 8));                             // ADD lr,lr,#disp&0x300
      }
      s = 14;
    } else if (indexed) {
      const unsigned r0 = Get(0, true, base_reg);
      code.push_back(0xe080e000u | (base_reg << 16) | r0); // ADD lr,Rm,R0
      s = 14;
    }
    const unsigned d = Get(n, false, s == 14 ? 7 : base_reg);
    auto jump = [&](unsigned at, unsigned target, uint32_t cond) {
      code[at] = cond | 0x0a000000u |
        (uint32_t(int32_t(target) - int32_t(at) - 2) & 0xffffffu);
    };
    // Hot path: cached high work RAM only. r10 = s ^ 0x06000000 is below
    // 0x100000 exactly for 0x060xxxxx and is then the 20-bit offset.
    auto load = [&](unsigned b) {
      if (width == 1) {
        code.push_back(0xe22aa001u);               // EOR r10,r10,#1 (T2 bytes)
        code.push_back(0xe19000dau | (b << 16) | (d << 12)); // LDRSB d,[b,r10]
      } else if (width == 2) code.push_back(0xe19000fau | (b << 16) | (d << 12)); // LDRSH
      else {
        code.push_back(0xe790000au | (b << 16) | (d << 12)); // LDR d,[b,r10]
        code.push_back(0xe1a00860u | (d << 12) | d); // ROR d,d,#16
      }
    };
    const unsigned hb = HighBase();                // before the first branch
    std::vector<unsigned> sources;
    std::vector<uint32_t> conds;
    code.push_back(0xe220a406u | (s << 16));       // EOR r10,s,#0x06000000
    RangeAlignCheck(width, sources, &conds);       // BHS cold
    HighBaseIp(hb);
    load(hb);
    const unsigned done = code.size();
    BeginCold(sources, conds);
    // Cold: cached low work RAM (0x002xxxxx), aligned, is also a direct read.
    std::vector<unsigned> to_helper;
    code.push_back(0xe220a602u | (s << 16));       // EOR r10,s,#0x00200000
    RangeAlignCheck(width, to_helper, nullptr);    // BHS helper (cond kept in the word)
    Constant(12, low_ram);
    load(12);
    ColdJumpHot(done);
    for (unsigned at : to_helper) jump(at, code.size(), code[at]);
    // Slow edge publishes dirty values before calling and reloads all resident
    // mappings afterwards. r10 holds the result while r0-r6 are reconstructed.
    for (unsigned h = 0; h < slots.size(); ++h) Store(h);
    Pending(0xe2800000u); // Match the old template's live PC/cycles at the callback.
    code.push_back(0xe1a00000u | s);
    code.push_back(0xe597a000u | (width == 1 ? 100 : width == 2 ? 104 : 108));
    code.push_back(0xe12fff3au);
    code.push_back(width == 1 ? 0xe6afa070u : width == 2 ? 0xe6bfa070u : 0xe1a0a000u);
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest >= 0)
        code.push_back(0xe5970000u | (h << 12) | (unsigned(slots[h].guest) * 4));
    Pending(0xe2400000u);
    code.push_back(0xe1a0000au | (d << 12));
    EndCold(done);
    slots[d].dirty = true;
    // SH-1/SH-2 Programming Manual MOVBP/MOVWP/MOVLP: post-increment
    // follows the read, and is suppressed for n == m. On the slow edge s
    // has been reloaded, so callback changes to canonical Rm are preserved.
    if (!displacement && !indexed && !gbr && (op & 4) && n != m) {
      code.push_back(0xe2800000u | (s << 16) | (s << 12) | width);
      slots[s].dirty = true;
    }
  }
  // Register-only MACH/MACL operations, each exactly its baseline template:
  // STS/LDS copy, CLRMAC zeroes both (as known constants), MULS.W/MULU.W
  // write the 16x16 product to MACL only, DMULS.L/DMULU.L the 64-bit product
  // to MACH:MACL, XTRCT Rn = Rm << 16 | Rn >> 16. Destinations are allocated
  // after their sources: Get evicts the least recently used slot, never one
  // of the (at most four) touched here.
  void MacRegister(uint16_t op, unsigned n, unsigned m) {
    const unsigned kind = op & 0xf00f, reg = op & 0xf0ff;
    if (op == 0x0028) {                            // CLRMAC
      for (auto &slot : slots) if (slot.guest == 19 || slot.guest == 20) slot = Slot{};
      known[19] = known[20] = true;
      values[19] = values[20] = 0;
      return;
    }
    if (reg == 0x000a || reg == 0x001a || reg == 0x400a || reg == 0x401a) {
      const unsigned mac = (op & 0x10) ? 20 : 19;
      const bool sts = (op >> 12) == 0;
      const unsigned s = Get(sts ? mac : n, true);
      const unsigned d = Get(sts ? n : mac, false, s);
      code.push_back(0xe1a00000u | (d << 12) | s);
      slots[d].dirty = true;
      return;
    }
    const unsigned a = Get(m, true), b = Get(n, true, a);
    if (kind == 0x200d) {                          // XTRCT
      code.push_back(0xe1a0c820u | b);             // MOV ip,Rn,LSR #16
      code.push_back(0xe18c0800u | (b << 12) | a); // ORR Rn,ip,Rm,LSL #16
      slots[b].dirty = true;
      return;
    }
    if (kind == 0x300d || kind == 0x3005) {        // DMULS.L / DMULU.L
      const unsigned lo = Get(20, false, b), hi = Get(19, false, lo);
      code.push_back((kind == 0x300d ? 0xe0c00090u : 0xe0800090u) |
                     (hi << 16) | (lo << 12) | (b << 8) | a); // SMULL/UMULL lo,hi,a,b
      slots[lo].dirty = slots[hi].dirty = true;
      return;
    }
    if (kind == 0x200e) {                          // MULU.W
      code.push_back(0xe6ffc070u | a);             // UXTH ip,Rm
      code.push_back(0xe6ffe070u | b);             // UXTH lr,Rn
      const unsigned d = Get(20, false, b);
      code.push_back(0xe0000e9cu | (d << 16));     // MUL d,ip,lr
      slots[d].dirty = true;
      return;
    }
    const unsigned d = Get(20, false, b);          // MULS.W
    code.push_back(0xe1600080u | (d << 16) | (b << 8) | a); // SMULBB d,Rm,Rn
    slots[d].dirty = true;
  }
  // MAC.L/MAC.W @Rm+,@Rn+. The templates read the first operand (MAC.L @Rn,
  // MAC.W @Rm), bump its register, then read the second (so n == m reads
  // the bumped address). Hot path only when S=0 and both reads are plain:
  // cached high work RAM 0x060xxxxx of any alignment (the templates read it
  // inline) or cached low work RAM 0x002xxxxx aligned to the width, where
  // memGetLong/memGetWord is the same T2 read with no cycles (a short cold
  // stub forms that host address and rejoins). Plain reads commute, so the
  // hot path forms both host addresses, then reads. MAC.L then does
  // MACH:MACL += the signed 64-bit product (SMLAL is the template's
  // SMULL/ADDS/ADC). MAC.W stores MACL += the signed 16x16 product and MACH =
  // that sum's carry-out taken from MACL's own sign, not the old MACH (MOV
  // hi,lo,ASR #31 + SMLALBB is its SMULBB/ADDS/ADC), so MACH is not loaded.
  // Anything else leaves state untouched and one cold edge publishes it and
  // runs the template itself (sh2_macl_region/sh2_macw_region: callbacks,
  // their order, saturation) with live PC/cycles, then reloads every
  // resident mapping. All four mappings are marked dirty after that edge, so
  // it stores only what was already dirty.
  void MacMemory(uint16_t op, bool word) {
    PublishSR();
    for (unsigned g = 0; g < known.size(); ++g)
      if (known[g]) (void)Get(g, true);
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    const unsigned first_guest = word ? m : n, second_guest = word ? n : m;
    const unsigned width = word ? 2 : 4;
    const unsigned first = Get(first_guest, true), second = Get(second_guest, true, first);
    const unsigned lo = Get(20, true, second), hi = Get(19, !word, lo);
    const unsigned hb = HighBase();                // before the first branch
    std::vector<unsigned> slow_hot, slow_cold;
    std::vector<uint32_t> conds;
    code.push_back(0xe597c040u);                   // LDR ip,[r7,#64] (SR)
    code.push_back(0xe31c0002u);                   // TST ip,#2 (S)
    slow_hot.push_back(code.size()); conds.push_back(0x10000000u); code.push_back(0); // BNE
    HighBaseIp(hb);
    // Host address of guest (reg + bump) into scratch h (10 or 14).
    auto address = [&](unsigned h, unsigned reg, unsigned bump, bool restore) {
      auto guest = [&](uint32_t eor) {
        if (bump) {
          code.push_back(0xe2800000u | (reg << 16) | (h << 12) | bump); // ADD h,reg,#bump
          code.push_back(eor | (h << 16) | (h << 12));                  // EOR h,h,#area
        } else {
          code.push_back(eor | (reg << 16) | (h << 12));                // EOR h,reg,#area
        }
      };
      guest(0xe2200406u);                          // ^ 0x06000000
      code.push_back(0xe3500601u | (h << 16));     // CMP h,#0x100000
      const unsigned low = code.size(); code.push_back(0); // BHS low stub
      code.push_back(0xe0800000u | (h << 16) | (h << 12) | hb); // ADD h,h,hb
      const unsigned back = code.size();
      BeginCold({low}, {0x20000000u});
      guest(0xe2200602u);                          // ^ 0x00200000
      code.push_back(0xe1a0c060u | (width == 4 ? 2u : 1u) << 7 | h); // MOV ip,h,ROR #k
      code.push_back(width == 4 ? 0xe35c0701u : 0xe35c0702u); // CMP ip,#0x40000/#0x80000
      slow_cold.push_back(code.size()); code.push_back(0x20000000u); // BHS slow edge
      Constant(12, low_ram);
      code.push_back(0xe080000cu | (h << 16) | (h << 12)); // ADD h,h,ip
      if (restore) HighBaseIp(hb);
      ColdJumpHot(back);
      code.swap(cold);
    };
    address(10, first, 0, true);
    address(14, first_guest == second_guest ? first : second,
            first_guest == second_guest ? width : 0, false);
    if (word) {
      code.push_back(0xe1daa0b0u);                 // LDRH r10,[r10]
      code.push_back(0xe1dee0b0u);                 // LDRH lr,[lr]
    } else {
      code.push_back(0xe59aa000u);                 // LDR r10,[r10]
      code.push_back(0xe59ee000u);                 // LDR lr,[lr]
      code.push_back(0xe1a0a86au);                 // ROR r10,r10,#16
      code.push_back(0xe1a0e86eu);                 // ROR lr,lr,#16
    }
    code.push_back(0xe2800000u | (first << 16) | (first << 12) | width);    // ADD first,#w
    code.push_back(0xe2800000u | (second << 16) | (second << 12) | width);  // ADD second,#w
    if (word) {
      code.push_back(0xe1a00fc0u | (hi << 12) | lo);         // MOV hi,lo,ASR #31
      code.push_back(0xe1400e8au | (hi << 16) | (lo << 12)); // SMLALBB lo,hi,r10,lr
    } else {
      code.push_back(0xe0e00e9au | (hi << 16) | (lo << 12)); // SMLAL lo,hi,r10,lr
    }
    const unsigned done = code.size();
    BeginCold(slow_hot, conds);
    for (unsigned at : slow_cold)                  // cold-to-cold: offsets within the cold buffer
      code[at] = code[at] | 0x0a000000u | (uint32_t(int32_t(code.size()) - int32_t(at) - 2) & 0xffffffu);
    for (unsigned h = 0; h < slots.size(); ++h) Store(h);
    Pending(0xe2800000u);
    code.push_back(0xe3a00000u | (m * 4));         // MOV r0,#Rm*4
    code.push_back(0xe3a01000u | (n * 4));         // MOV r1,#Rn*4
    Constant(12, word ? macw_helper : macl_helper);
    code.push_back(0xe12fff3cu);                   // BLX ip
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest >= 0)
        code.push_back(0xe5970000u | (h << 12) | (unsigned(slots[h].guest) * 4));
    Pending(0xe2400000u);
    EndCold(done);
    slots[first].dirty = slots[second].dirty = slots[hi].dirty = slots[lo].dirty = true;
  }
  // Exactly the dynarec memSet helpers' observable effect: cached high RAM
  // (0x060xxxxx), aligned, in a KiB with no compiled-code owner, is a plain
  // T2 store with no cycles and no invalidation (setDirty sees no owner:
  // owners are only added where code_pages is also set). Everything else
  // (low RAM invalidation, aliases, devices, misalignment, code pages) calls
  // the original helper with published state, like the old template.
  void GuardStore(uint16_t op) {
    if (onchip_write[0] && !resident_loop && OnchipConstantStore(op)) return;
    PublishSR(); // r10 is guard scratch; helpers can observe SR.
    for (unsigned g = 0; g < known.size(); ++g)
      if (known[g]) (void)Get(g, true);
    unsigned width, n, m, disp = 0;
    bool predecrement = false, indexed = false;
    if ((op >> 12) == 2) {
      width = 1u << (op & 3); n = (op >> 8) & 15; m = (op >> 4) & 15; predecrement = (op & 4) != 0;
    } else if ((op >> 12) == 0) {
      width = 1u << (op & 3); n = (op >> 8) & 15; m = (op >> 4) & 15; indexed = true;
    } else if ((op >> 12) == 1) { width = 4; n = (op >> 8) & 15; m = (op >> 4) & 15; disp = (op & 15) * 4; }
    else { width = (op >> 8) == 0x81 ? 2 : 1; n = (op >> 4) & 15; m = 0; disp = (op & 15) * width; }
    const unsigned base_reg = Get(n, true);
    unsigned v = Get(m, true, base_reg);
    unsigned s = base_reg;
    if (disp) {
      code.push_back(0xe280e000u | (base_reg << 16) | disp); // ADD lr,Rn,#disp
      s = 14;
    } else if (indexed) {
      // Rm must stay resident with Rn and R0 (<= 3 of 7 host registers).
      const unsigned r0 = Get(0, true, base_reg);
      v = Get(m, true, base_reg);
      code.push_back(0xe080e000u | (base_reg << 16) | r0); // ADD lr,Rn,R0
      s = 14;
    } else if (predecrement) {
      // Like the old template (MOVBM/MOVWM/MOVLM): Rm is read before Rn -= size.
      if (n == m) { code.push_back(0xe1a0e000u | v); v = 14; } // MOV lr,Rm
      code.push_back(0xe2400000u | (base_reg << 16) | (base_reg << 12) | width);
      slots[base_reg].dirty = true;
    }
    std::vector<unsigned> slow;
    std::vector<uint32_t> conds;
    // r10 = s ^ 0x06000000 < 0x100000 exactly for cached high RAM; it is then
    // the 20-bit offset, and offset >> 10 indexes code_pages.
    const unsigned hb = HighBase();              // before the first branch
    code.push_back(0xe220a406u | (s << 16));     // EOR r10,s,#0x06000000
    RangeAlignCheck(width, slow, &conds);        // BHS
    Constant(12, code_pages);
#ifdef VITA_SH2_CODE_LINES
    code.push_back(0xe7dcc22au);                 // LDRB ip,[ip,r10,LSR #4] (code_lines)
#else
    code.push_back(0xe7dcc52au);                 // LDRB ip,[ip,r10,LSR #10]
#endif
    code.push_back(0xe35c0000u);                 // CMP ip,#0
    slow.push_back(code.size()); conds.push_back(0x10000000u); code.push_back(0); // BNE
    HighBaseIp(hb);
    if (width == 1) {
      code.push_back(0xe22aa001u);               // EOR r10,r10,#1
      code.push_back(0xe7c0000au | (hb << 16) | (v << 12)); // STRB v,[hb,r10]
    } else if (width == 2) {
      code.push_back(0xe18000bau | (hb << 16) | (v << 12)); // STRH v,[hb,r10]
    } else if (hb == 11) {
      code.push_back(0xe1a0c860u | v);           // MOV ip,v,ROR #16
      code.push_back(0xe78bc00au);               // STR ip,[r11,r10]
      if (stack_spec_enabled) ++legacy_words;    // r12 base: ADD, MOV, STR
    } else {
      code.push_back(0xe08cc00au);               // ADD ip,ip,r10
      code.push_back(0xe1a0a860u | v);           // MOV r10,v,ROR #16
      code.push_back(0xe58ca000u);               // STR r10,[ip]
    }
    const unsigned done = code.size();
    BeginCold(slow, conds);
    for (unsigned h = 0; h < slots.size(); ++h) Store(h);
    Pending(0xe2800000u);                        // ADD r8/r9
    if (s != 0) code.push_back(0xe1a00000u | s); // MOV r0,s
    const unsigned value_offset = m * 4;          // canonical after publish
    if (v == 14) {                                // pre-decrement n == m: old Rm
      code.push_back(width == 1 ? 0xe6ef107eu : width == 2 ? 0xe6ff107eu : 0xe1a0100eu);
    } else if (width == 1) code.push_back(0xe5d71000u | value_offset); // LDRB r1
    else if (width == 2)
      code.push_back(0xe1d710b0u | ((value_offset >> 4) << 8) | (value_offset & 15)); // LDRH r1
    else code.push_back(0xe5971000u | value_offset); // LDR r1
    code.push_back(0xe597a000u | (width == 1 ? 112 : width == 2 ? 116 : 120));
    code.push_back(0xe12fff3au);                 // BLX r10
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest >= 0)
        code.push_back(0xe5970000u | (h << 12) | (unsigned(slots[h].guest) * 4));
    Pending(0xe2400000u);                        // SUB r8/r9
    EndCold(done);
  }
  // MOV.x Rm,@Rn / Rm,@(disp,Rn) / R0,@(disp,Rn) to a constant on-chip
  // address (0xFFFFFE00 and up, e.g. the divider): memSet* calls OnchipWrite*
  // with addr & 0x1FF and adds no cycles there, so call that handler directly
  // (value zero-extended as the slow edge's LDRB/LDRH), with SR published,
  // r0-r3 preserved and the live PC/count as the slow edge has them. Every
  // state change is GuardStore's (checked against a copy that ran it), and
  // legacy_words/cold_owed carry the difference to its words, so block sizing
  // and boundaries are those of the guarded emission.
  bool OnchipConstantStore(uint16_t op) {
    unsigned width, n, m, disp = 0;
    if ((op >> 12) == 2) {
      if (op & 4) return false;                   // pre-decrement
      width = 1u << (op & 3); n = (op >> 8) & 15; m = (op >> 4) & 15;
    } else if ((op >> 12) == 0) return false;     // indexed
    else if ((op >> 12) == 1) { width = 4; n = (op >> 8) & 15; m = (op >> 4) & 15; disp = (op & 15) * 4; }
    else { width = (op >> 8) == 0x81 ? 2 : 1; n = (op >> 4) & 15; m = 0; disp = (op & 15) * width; }
    if (!(known[n] || hint_ok[n]) || !onchip_write[width >> 1]) return false;
    const uint32_t address = (known[n] ? values[n] : hint_val[n]) + disp;
    if (address < 0xFFFFFE00u) return false;
    RegisterRegion guarded = *this;
    guarded.onchip_write[0] = 0;
    guarded.GuardStore(op);
    const size_t code0 = code.size();
    const int guarded_words = int(guarded.code.size() - code0) + int(guarded.cold.size() - cold.size());
    if (cold.empty() && !guarded.cold.empty()) cold_owed = true;
    PublishSR();
    for (unsigned g = 0; g < known.size(); ++g)
      if (known[g]) (void)Get(g, true);
    const unsigned base_reg = Get(n, true);
    const unsigned v = Get(m, true, base_reg);
    (void)HighBase();
    for (unsigned h = 0; h < 4; ++h) Store(h);    // r0-r3 are caller-saved
    Pending(0xe2800000u);
    code.push_back(width == 1 ? 0xe6ef1070u | v : width == 2 ? 0xe6ff1070u | v : 0xe1a01000u | v); // UXTB/UXTH/MOV r1,v
    Constant(0, address & 0x1FF);
    Constant(12, onchip_write[width >> 1]);
    code.push_back(0xe12fff3cu);                 // BLX ip
    for (unsigned h = 0; h < 4; ++h)
      if (slots[h].guest >= 0)
        code.push_back(0xe5970000u | (h << 12) | (unsigned(slots[h].guest) * 4));
    Pending(0xe2400000u);
    legacy_words = guarded.legacy_words + guarded_words - int(code.size() - code0);
    for (unsigned h = 0; h < slots.size(); ++h)
      if (slots[h].guest != guarded.slots[h].guest || slots[h].dirty != guarded.slots[h].dirty ||
          slots[h].used != guarded.slots[h].used) abort();
    if (known != guarded.known || base11 != guarded.base11 || sr_dirty != guarded.sr_dirty || clock != guarded.clock)
      abort();
    return true;
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
  static int ConstantWords(uint32_t value) {
    return value <= 255 || ~value <= 255 ? 1 : value >> 16 ? 2 : 1;
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
