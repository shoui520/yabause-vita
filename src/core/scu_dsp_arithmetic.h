/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCU_DSP_ARITHMETIC_H
#define SCU_DSP_ARITHMETIC_H
#include <stdint.h>

/* ACL/PL operations wrap at 32 bits. Avoid signed overflow and avoid using
 * 64-bit arithmetic merely to extract carry on the 32-bit Cortex-A9.
 * Signed conversion is explicit: even its intermediate is representable. */
static inline int32_t ScuDspSigned32(uint32_t value) {
  return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}
static inline int32_t ScuDspAdd32(uint32_t a, uint32_t b) {
  return ScuDspSigned32(a + b);
}
static inline int32_t ScuDspSub32(uint32_t a, uint32_t b) {
  return ScuDspSigned32(a - b);
}
static inline unsigned ScuDspCarry32(uint32_t a, uint32_t b) {
  return a + b < a;
}
/* Retain the reference core's subtraction C convention (borrow). ARM SUBS
 * reports NOT-borrow, so a future native lowering must invert that flag. */
static inline unsigned ScuDspBorrow32(uint32_t a, uint32_t b) {
  return a < b;
}

/* 32-bit ALU commands preserve ACH and V. Pack Z/S/C together, rather than
 * repeatedly read/modify/writing three bitfields on the ARM memory operand.
 * AD2 is the separate 48-bit operation; NOP/reserved opcodes leave flags alone.
 * All shifts operate on unsigned values, including the left-shift input. */
static inline uint32_t ScuDspAlu32(unsigned op, uint32_t ac, uint32_t p,
                                 uint32_t control, uint32_t *result) {
  uint32_t value = ac, carry = 0;
  switch (op) {
    case 1: value = ac & p; break;
    case 2: value = ac | p; break;
    case 3: value = ac ^ p; break;
    case 4: value = ac + p; carry = value < ac; break;
    case 5: value = ac - p; carry = ac < p; break;
    case 8: value = (ac >> 1) | (ac & UINT32_C(0x80000000)); carry = ac & 1; break;
    case 9: value = (ac >> 1) | (ac << 31); carry = ac & 1; break;
    case 10: value = ac << 1; carry = ac >> 31; break;
    case 11: value = (ac << 1) | (ac >> 31); carry = ac >> 31; break;
    case 15: value = (ac << 8) | (ac >> 24); carry = (ac >> 24) & 1; break;
    default: *result = ac; return control;
  }
  *result = value;
  return (control & ~UINT32_C(0x700000)) | (carry << 20)
       | ((uint32_t)(value == 0) << 21) | ((value >> 31) << 22);
}
#endif
