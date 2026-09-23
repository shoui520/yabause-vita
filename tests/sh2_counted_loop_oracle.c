/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compile the actual interpreter, not a second transcription of its opcodes.
 * Section GC discards unrelated emulator entrypoints and their dependencies. */
#include "../src/core/sh2int.c"
#include <assert.h>
#include <stdlib.h>

static void sh2_division_oracle(u32 *state, u16 instruction) {
   SH2_struct cpu = {0};
   memcpy(cpu.regs.R, state, 16 * sizeof(u32));
   cpu.regs.SR.all = state[16];
   cpu.regs.PC = state[22];
   cpu.cycles = state[23];
   cpu.instruction = instruction;
   if ((instruction & 0xf0ff) == 0x4024) SH2rotcl(&cpu);
   else if ((instruction & 0xf00f) == 0x300e) SH2addc(&cpu);
   else if ((instruction & 0xf00f) == 0x300a) SH2subc(&cpu);
   else SH2div1(&cpu);
   memcpy(state, cpu.regs.R, 16 * sizeof(u32));
   state[16] = cpu.regs.SR.all;
   state[22] = cpu.regs.PC;
   state[23] = cpu.cycles;
}

void sh2_div1_oracle(u32 *state, unsigned m, unsigned n) {
   sh2_division_oracle(state, 0x3004 | (n << 8) | (m << 4));
}
void sh2_rotcl_oracle(u32 *state, unsigned n) {
   sh2_division_oracle(state, 0x4024 | (n << 8));
}
void sh2_carry_oracle(u32 *state, unsigned instruction) {
   sh2_division_oracle(state, instruction);
}

void sh2_counted_loop_oracle(unsigned variant, unsigned reg, u32 *state)
{
   SH2_struct *cpu = calloc(1, sizeof(*cpu));
   assert(cpu && variant < 2 && reg < 16);
   memcpy(cpu->regs.R, state, 16 * sizeof(u32));
   cpu->regs.SR.all = state[16];
   cpu->regs.PC = state[22];
   cpu->cycles = state[23];
   const u32 start = cpu->regs.PC;
   do {
      cpu->instruction = (variant ? 0x70ff : 0x4010) | (reg << 8);
      if (variant) {
         SH2addi(cpu);
         cpu->instruction = 0x4011 | (reg << 8);
         SH2cmppz(cpu);
         cpu->instruction = 0x89fc;
         SH2bt(cpu);
      } else {
         SH2dt(cpu);
         cpu->instruction = 0x8bfd;
         SH2bf(cpu);
      }
   } while (cpu->regs.PC == start && cpu->cycles < state[32] &&
            (cpu->regs.SR.all & 0xf0) >= state[24]);
   memcpy(state, cpu->regs.R, 16 * sizeof(u32));
   state[16] = cpu->regs.SR.all;
   state[22] = cpu->regs.PC;
   state[23] = cpu->cycles;
   free(cpu);
}
