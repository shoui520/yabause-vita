/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "c68k/c68k.h"
#include <stdlib.h>
#include <string.h>
extern void YuiMsg(const char *, ...);
static u8 *test_ram;
static unsigned callback_calls;
static c68k_struc *active_cpu;
static u32 FASTCALL read_byte(u32 a) {
  ++callback_calls;
  if (a >= 0x100000) active_cpu->CycleIO -= 3;
  return a < 0x80000 ? test_ram[a ^ 1] : 0xa5;
}
static u32 FASTCALL read_word(u32 a) {
  ++callback_calls;
  if (a >= 0x100000) active_cpu->CycleIO -= 3;
  if (a >= 0x7ffff) return 0xa55a;
  // memcpy also defines the synthetic odd-address oracle without host UB.
  u16 value;
  memcpy(&value, test_ram + a, sizeof(value));
  return value;
}
int VitaC68kReadTest(void) {
  test_ram = malloc(0x80000);
  if (!test_ram) return -1;
  for (unsigned i = 0; i < 0x80000; ++i) test_ram[i] = (i * 131 + (i >> 9)) & 255;
  unsigned cases = 0, removed_calls = 0;
  const u32 addresses[] = {0, 1, 0x1234, 0x7fffc, 0x7fffe, 0x7ffff, 0x80000, 0x100000};
  // Actual MOVE indirect/postincrement/predecrement, signed MOVEA.W,
  // ADD/SUB/CMP/AND/OR handlers. MMIO callbacks also modify CycleIO.
  const u16 instructions[] = {0x1010, 0x3010, 0x2010, 0x1018, 0x3018, 0x2018,
    0x1020, 0x3020, 0x2020, 0x3250, 0xd050, 0x9050, 0xb050, 0xc050, 0x8050};
  for (unsigned op = 0; op < sizeof(instructions)/sizeof(instructions[0]); ++op) {
    memcpy(test_ram + 0x100, &instructions[op], sizeof(u16));
    for (unsigned a = 0; a < sizeof(addresses)/sizeof(addresses[0]); ++a) {
      c68k_struc reference, direct;
      C68k_Init(&reference, NULL);
      C68k_Set_ReadB(&reference, read_byte);
      C68k_Set_ReadW(&reference, read_word);
      C68k_Set_Fetch(&reference, 0, 0x7ffff, (pointer)test_ram);
      C68k_Set_PC(&reference, 0x100);
      reference.A[0] = addresses[a];
      reference.D[0] = 0x12345678;
      C68k_Set_SR(&reference, 0x271f);
      direct = reference;
      direct.DirectReadRam = test_ram;
      callback_calls = 0;
      active_cpu = &reference;
      s32 expected_cycles = C68k_Exec(&reference, 1);
      unsigned reference_calls = callback_calls;
      callback_calls = 0;
      active_cpu = &direct;
      s32 actual_cycles = C68k_Exec(&direct, 1);
      removed_calls += reference_calls - callback_calls;
      direct.DirectReadRam = NULL;
      if (actual_cycles != expected_cycles || memcmp(&direct, &reference, sizeof(direct))) {
        YuiMsg("c68k_read_test_failed op=%04x address=%08x", instructions[op], addresses[a]);
        free(test_ram); test_ram = NULL; return -1;
      }
      ++cases;
    }
  }
  free(test_ram); test_ram = NULL;
#ifdef VITA_C68K_RAM_READS
  if (!removed_calls) { YuiMsg("c68k_read_test_failed fast_path_unused"); return -1; }
#endif
  YuiMsg("c68k_read_test_callbacks removed=%u", removed_calls);
  YuiMsg("c68k_read_test_pass cases=%u", cases);
  return 0;
}
