/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C68K_NATIVE_OPCODES_H
#define C68K_NATIVE_OPCODES_H
#include <stdint.h>
/* Single admission definition shared by the C dispatcher and A32 compiler. */
static inline int C68kNativeOpcodeSupported(uint16_t op) {
  if ((op & 0xf100) == 0x7000 || op == 0x4e71) return 1;
  if ((op & 0xf1f0) == 0x2000 || (op & 0xf1f0) == 0x2040) return 1;
  if ((op & 0xf0f0) == 0x5080) return 1;
  switch (op & 0xf1f8) {
  case 0xd080: case 0x9080: case 0xc080: case 0x8080:
  case 0xb180: case 0xb080: case 0xd1c0: case 0x91c0: return 1;
  }
  switch (op & 0xfff8) {
  case 0x4280: case 0x4480: case 0x4680: case 0x4a80: return 1;
  }
  return 0;
}
#endif
