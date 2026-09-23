/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2int.c"
#include <assert.h>

static u32 read_value, read_cycles, seen_address, seen_width, reads;
static SH2_struct *callback_cpu;
static unsigned mutate_callback;
static void callback_effect(void) {
  if (mutate_callback) {
    callback_cpu->regs.R[15] ^= 0x76543210u;
    callback_cpu->regs.SR.all ^= 0x101u;
  }
}
u8 FASTCALL MappedMemoryReadByte(u32 address, u32 *cycles) {
  seen_address = address; seen_width = 1; ++reads;
  callback_effect();
  if (cycles) *cycles = read_cycles;
  return read_value;
}
u16 FASTCALL MappedMemoryReadWord(u32 address, u32 *cycles) {
  seen_address = address; seen_width = 2; ++reads;
  callback_effect();
  if (cycles) *cycles = read_cycles;
  return read_value;
}
u32 FASTCALL MappedMemoryReadLong(u32 address, u32 *cycles) {
  seen_address = address; seen_width = 4; ++reads;
  callback_effect();
  if (cycles) *cycles = read_cycles;
  return read_value;
}

void sh2_poll_step_oracle(u32 *state, const u16 *ops, unsigned count,
                          u32 value, u32 wait, unsigned mutate, u32 *receipt) {
  SH2_struct cpu = {0};
  memcpy(cpu.regs.R, state, 16 * sizeof(u32));
  cpu.regs.SR.all = state[16]; cpu.regs.PC = state[17]; cpu.cycles = state[18];
  read_value = value; read_cycles = wait; reads = 0;
  callback_cpu = &cpu; mutate_callback = mutate;
  cpu.instruction = ops[0];
  switch (ops[0] & 3) {
    case 0: SH2movbl(&cpu); break;
    case 1: SH2movwl(&cpu); break;
    case 2: SH2movll(&cpu); break;
    default: assert(0);
  }
  for (unsigned i = 1; i < count; ++i) {
    cpu.instruction = ops[i];
    switch (ops[i] & 0xf00f) {
      case 0x600c: SH2extub(&cpu); break;
      case 0x2009: SH2y_and(&cpu); break;
      case 0x3000: SH2cmpeq(&cpu); break;
      case 0x2008: SH2tst(&cpu); break;
      default: SH2bf(&cpu); break;
    }
  }
  memcpy(state, cpu.regs.R, 16 * sizeof(u32));
  state[16] = cpu.regs.SR.all; state[17] = cpu.regs.PC; state[18] = cpu.cycles;
  receipt[0] = seen_address; receipt[1] = seen_width; receipt[2] = reads;
}
