/* SPDX-License-Identifier: GPL-2.0-or-later */
.syntax unified
.arch armv7-a
.arm
.text
.align 2
.global VitaSh2RunCached
.type VitaSh2RunCached,%function
VitaSh2RunCached:
  push {r4-r11,r12,lr}
  sub sp,sp,#24
  // Stack: context, last block, start count, completed, cycle sum low/high.
  str r0,[sp]
  mov r1,#0
  str r1,[sp,#4]
  str r1,[sp,#12]
  str r1,[sp,#16]
  str r1,[sp,#20]
  ldr r7,[r0]
.Lchain_lookup:
  ldr r0,[sp]
  ldr r9,[r7,#92]
  ldr r1,[r0,#20]
  cmp r9,r1
  movhs r0,#0
  bhs .Lchain_exit
  ldr r1,[sp,#12]
  ldr r2,[r0,#28]
  cmp r1,r2
  movhs r0,#1
  bhs .Lchain_exit
  ldr r8,[r7,#88]
  and r1,r8,#0xff000000
  cmp r1,#0xc0000000
  beq .Lchain_mapping
  and r1,r8,#0x0ff00000
  cmp r1,#0
  beq .Lchain_rom
  cmp r1,#0x00200000
  ldreq r3,[r0,#8]
  beq .Lchain_table
  cmp r1,#0x06000000
  bne .Lchain_mapping
  ldr r3,[r0,#12]
  b .Lchain_table
.Lchain_rom:
  ldr r1,[r0,#24]
  cmp r1,#0
  bne .Lchain_mapping
  ldr r3,[r0,#4]
.Lchain_table:
  ubfx r2,r8,#1,#19
  ldr r3,[r3,r2,lsl #2]
  cmp r3,#0
  moveq r0,#2
  beq .Lchain_exit
  // Loop recognition must stay in C++, after interrupt handling. Do not run
  // these blocks here: the fallback will execute them exactly once.
  ldr r1,[r3,#16]
  tst r1,#1
  movne r0,#4
  bne .Lchain_exit
  str r3,[sp,#4]
  str r9,[sp,#8]
  ldr r0,[r3]
  add r0,r0,#28
  adr r11,.Lchain_return
  bx r0
.Lchain_return:
  // Only r7 and r11 survive by private contract; all loop locals are on stack.
  ldr r2,[r7,#92]
  ldr r1,[sp,#8]
  sub r2,r2,r1
  ldr r3,[sp,#16]
  adds r3,r3,r2
  str r3,[sp,#16]
  ldr r1,[sp,#20]
  adc r1,r1,#0
  str r1,[sp,#20]
  ldr r1,[sp,#12]
  add r1,r1,#1
  str r1,[sp,#12]
  ldr r1,[r7,#64]
  and r1,r1,#0xf0
  ldr r2,[r7,#96]
  cmp r1,r2
  movlo r0,#3
  blo .Lchain_exit
  ldr r1,[sp,#4]
  ldr r1,[r1,#16]
  tst r1,#1
  movne r0,#6
  bne .Lchain_exit
  // Interrupt handling precedes memory-cycle accounting in ExecuteCount.
  // On interrupt exit leave that final block's memory cycles for C++.
  ldr r0,[sp]
  ldr r1,[r0,#16]
  ldr r2,[r1]
  ldr r3,[r7,#92]
  add r3,r3,r2
  str r3,[r7,#92]
  mov r2,#0
  str r2,[r1]
  b .Lchain_lookup
.Lchain_mapping:
  mov r0,#5
.Lchain_exit:
  mov r2,r0
  ldr r0,[sp]
  ldr r1,[sp,#12]
  str r1,[r0,#32]
  ldr r1,[sp,#16]
  str r1,[r0,#36]
  ldr r1,[sp,#20]
  str r1,[r0,#40]
  ldr r1,[sp,#4]
  str r1,[r0,#44]
  str r2,[r0,#48]
  add sp,sp,#24
  pop {r4-r11,r12,pc}
.size VitaSh2RunCached,.-VitaSh2RunCached
.section .note.GNU-stack,"",%progbits
