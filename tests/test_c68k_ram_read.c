/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/sound_ram_read.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned calls;
static u32 last;
static u32 FASTCALL fallback(u32 address) {
  ++calls;
  last = address;
  return 0xabcdef12;
}
// c68k.c links against the executor; these tests exercise mapping policy only.
s32 FASTCALL C68k_Exec(c68k_struc *cpu, s32 cycles) { (void)cpu; return cycles; }
int main(void) {
  c68k_struc cpu = {0};
  u8 *ram = malloc(0x80000);
  assert(ram);
  for (unsigned i = 0; i < 0x80000; ++i) ram[i] = (i * 131u + (i >> 9)) & 255;
  C68k_Set_ReadB(&cpu, fallback);
  C68k_Set_ReadW(&cpu, fallback);
  cpu.DirectReadRam = ram;
  for (unsigned a = 0; a < 0x80000; ++a) {
    assert(C68k_ReadByte(&cpu, a) == ram[a ^ 1]);
    if (!(a & 1)) {
      assert(C68k_ReadWord(&cpu, a) == (u32)(ram[a] | ram[a + 1] << 8));
    } else {
      unsigned before = calls;
      assert(C68k_ReadWord(&cpu, a) == 0xabcdef12);
      assert(calls == before + 1 && last == a);
    }
  }
  const u32 slow[] = {0x80000, 0xfffff, 0x100000, 0x100001, 0xffffffff};
  for (unsigned i = 0; i < sizeof(slow)/sizeof(slow[0]); ++i) {
    unsigned before = calls;
    assert(C68k_ReadByte(&cpu, slow[i]) == 0xabcdef12);
    assert(C68k_ReadWord(&cpu, slow[i]) == 0xabcdef12);
    assert(calls == before + 2 && last == slow[i]);
  }
  C68k_Set_ReadB(&cpu, fallback);
  assert(!cpu.DirectReadRam);
  assert(C68k_ReadByte(&cpu, 0) == 0xabcdef12);
  cpu.DirectReadRam = ram;
  C68k_Set_ReadW(&cpu, fallback);
  assert(!cpu.DirectReadRam);
  assert(C68k_ReadWord(&cpu, 0) == 0xabcdef12);
  free(ram);
  puts("C68K RAM reads: all 524288 addresses, odd-word fallback, MMIO and callback revocation passed");
}
