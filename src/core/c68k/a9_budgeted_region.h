/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "a9_register_region.h"
#include "c68k.h"
#include <cstddef>

namespace m68ka9 {
// Callable A32 arithmetic unit: unsigned run(c68k_struc *, int remaining).
// Returns zero, with state untouched, if the ENTIRE region cannot fit. Caller
// then uses its instruction-boundary fallback; it must not spin on zero.
// Returns consumed cycles otherwise. This is not C68k_Exec: IRQ admission,
// RUNNING/CycleToDo/CycleSup ownership, source validity, mapped fetch span and
// any asynchronous observation boundary belong to the enclosing dispatcher.
// One preflight check replaces internal budget checks only because these
// admitted instructions have fixed costs and no memory/exception/callbacks.
struct BudgetedRegion {
  std::vector<uint32_t> code;
  unsigned cycles = 0, bytes = 0;

  static BudgetedRegion Compile(const std::vector<uint16_t>& ops) {
    // Generated state accesses are for the Vita's 32-bit C68K layout.
    static_assert(sizeof(pointer) == 4, "build A32 regions with a 32-bit target layout");
    BudgetedRegion result;
    if (ops.empty() || ops.size() > 256) return result;
    RegisterRegion body;
    for (uint16_t op : ops) if (!body.Emit(op)) return result;
    body.Flush();
    result.cycles = body.cycles;
    result.bytes = unsigned(ops.size()) * 2;
    auto &code = result.code;
    auto constant = [&](unsigned reg, unsigned value) {
      // Bounded region size makes every metadata constant fit MOVW.
      code.push_back(0xe3000000u | reg << 12 | (value & 0xfff) |
                     ((value >> 12) & 15) << 16);
    };
    constant(12, result.cycles);
    code.push_back(0xe151000c); // CMP r1,ip (signed budget, including negatives)
    code.push_back(0xb3a00000); // MOVLT r0,#0
    code.push_back(0xb12fff1e); // BXLT lr: no state/stack change on rejection
    code.push_back(0xe92d5ff0); // PUSH r4-r12,lr, 40 bytes
    // At the only exit, reproduce the original NEXT publication before the
    // last guest instruction. r1 is scratch in the arithmetic body, so publish
    // first; no admitted body instruction observes or modifies CycleIO.
    constant(12, result.cycles - RegisterRegion::Cycles(ops.back()));
    code.push_back(0xe041c00c); // SUB ip,r1,ip (non-S)
    code.push_back(0xe580c000u | offsetof(c68k_struc, CycleIO));
    code.insert(code.end(), body.code.begin(), body.code.end());
    code.push_back(0xe5901000u | offsetof(c68k_struc, PC));
    constant(12, result.bytes);
    code.push_back(0xe081100c); // ADD r1,r1,ip
    code.push_back(0xe5801000u | offsetof(c68k_struc, PC));
    constant(0, result.cycles);
    code.push_back(0xe8bd9ff0); // POP r4-r12,pc
    return result;
  }
};
} // namespace m68ka9
