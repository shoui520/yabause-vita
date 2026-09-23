/* SPDX-License-Identifier: GPL-2.0-or-later */
/* r0=m byte offset, r1=n byte offset, r7=canonical register state.
 * SH-2 manual pp.87-88: shift/store Rn BEFORE reading Rm (including m==n).
 * Add when old Q != M; subtract otherwise. ARM carry is NOT-borrow for
 * subtraction. Convert it before calculating Q = old_sign XOR M XOR carry.
 * r0-r4/ip are scratch; PC and cycle registers are untouched. */
.macro SH2_DIV1_A9
  ldr r3, [r7, #64]
  ldr r4, [r7, r1]
  and r2, r3, #1
  orr r2, r2, r4, lsl #1
  str r2, [r7, r1]
  ldr r0, [r7, r0]
  eor ip, r3, r3, lsr #1
  ubfx ip, ip, #8, #1
  sub ip, ip, #1
  eor r0, r0, ip
  lsrs ip, ip, #1
  adcs r2, r2, r0
  str r2, [r7, r1]
  mov r0, #0
  adc r0, r0, #0
  eor r0, r0, ip
  eor r0, r0, r4, lsr #31
  eor r0, r0, r3, lsr #9
  and r0, r0, #1
  bic r3, r3, #256
  bic r3, r3, #1
  orr r3, r3, r0, lsl #8
  eor r0, r0, r3, lsr #9
  and r0, r0, #1
  eor r0, r0, #1
  orr r3, r3, r0
  str r3, [r7, #64]
.endm
