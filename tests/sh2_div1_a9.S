/* SPDX-License-Identifier: GPL-2.0-or-later */
.syntax unified
.arch armv7-a
.arm
.include "sh2_dynarec/div1_a9.inc"
.text
.global sh2_div1_a9
.type sh2_div1_a9, %function
sh2_div1_a9:
  push {r4, r7, lr}
  mov r7, r0
  mov r0, r1
  mov r1, r2
  SH2_DIV1_A9
  pop {r4, r7, pc}
.size sh2_div1_a9, .-sh2_div1_a9
.section .note.GNU-stack,"",%progbits
