/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
extern void YuiMsg(const char *, ...);
extern int main(void);

/* Keep the failing frame live for the Vita's coredump writer. This function is
 * intentionally not stack-protected itself. Log both runtime anchors so the
 * frozen ELF can resolve the caller even if older return slots were damaged. */
__attribute__((noreturn, noinline)) void __wrap___stack_chk_fail(void) {
  YuiMsg("stack_corruption caller=%08x main=%08x",
    (unsigned)(uintptr_t)__builtin_return_address(0), (unsigned)(uintptr_t)&main);
  __builtin_trap();
}
