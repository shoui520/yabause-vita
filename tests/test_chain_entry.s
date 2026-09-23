.syntax unified
.arch armv7-a
.arm
.text
.global TestChainEntry
.type TestChainEntry,%function
// r0=state, r1=first private body, r2=second private body.
TestChainEntry:
  push {r4-r11,r12,lr}
  sub sp,sp,#8
  str r2,[sp]
  mov r7,r0
  ldr r8,[r7,#88]
  ldr r9,[r7,#92]
  adr r11,1f
  bx r1
1:
  ldr r1,[sp]
  // Reload architectural PC/count just as a future dispatcher must, since
  // branch templates may write a different PC directly into state.
  ldr r8,[r7,#88]
  ldr r9,[r7,#92]
  adr r11,2f
  bx r1
2:
  add sp,sp,#8
  pop {r4-r11,r12,pc}
.size TestChainEntry,.-TestChainEntry
.global TestChainPreservation
.type TestChainPreservation,%function
// r0=entry function, r1=state, r2=private body (if required by entry).
TestChainPreservation:
  push {r4-r11,r12,lr}
  sub sp,sp,#8
  str r0,[sp]
  mov r0,r1
  mov r1,r2
  mov r4,#4
  mov r5,#5
  mov r6,#6
  mov r7,#7
  mov r8,#8
  mov r9,#9
  mov r10,#10
  mov r11,#11
  ldr r3,[sp]
  blx r3
  mov r0,#1
  cmp r4,#4
  cmpeq r5,#5
  cmpeq r6,#6
  cmpeq r7,#7
  cmpeq r8,#8
  cmpeq r9,#9
  cmpeq r10,#10
  cmpeq r11,#11
  moveq r0,#0
  add sp,sp,#8
  pop {r4-r11,r12,pc}
.size TestChainPreservation,.-TestChainPreservation
.section .note.GNU-stack,"",%progbits
