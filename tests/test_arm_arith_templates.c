/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux ARM/QEMU test of the two-register arithmetic ARM templates against
 * the SH7604 hardware manual's operation descriptions (ADD, ADDC, ADDV, SUB,
 * SUBC, SUBV, NEG, NEGC). Every Rm/Rn pair is patched, including Rm == Rn.
 * This does not test the complete SH-2 compiler or the A9 register regions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern unsigned char prologue[], epilogue[];
#define DECLARE(name) extern unsigned char x86_##name[]; \
  extern const unsigned short name##_size; \
  extern const unsigned char name##_src, name##_dest;
DECLARE(ADD) DECLARE(ADDC) DECLARE(ADDV)
DECLARE(SUB) DECLARE(SUBC) DECLARE(SUBV)
DECLARE(NEG) DECLARE(NEGC)

enum { OP_ADD, OP_ADDC, OP_ADDV, OP_SUB, OP_SUBC, OP_SUBV, OP_NEG, OP_NEGC };

struct operation {
  const char *name;
  const unsigned char *code;
  const unsigned short *size;
  const unsigned char *src, *dest;
  unsigned kind;
};
#define OP(name) {#name, x86_##name, &name##_size, &name##_src, &name##_dest, OP_##name}

/* Rn and T after "op Rm,Rn" (SH7604 manual, section 8). */
static void reference(unsigned kind, uint32_t m, uint32_t n, uint32_t t,
                      uint32_t *rn, uint32_t *tn) {
  uint32_t r = n, tmp;
  *tn = t;
  switch (kind) {
  case OP_ADD: r = n + m; break;
  case OP_ADDC: tmp = n + m; r = tmp + t; *tn = (n > tmp) | (tmp > r); break;
  case OP_ADDV: r = n + m; *tn = (~(n ^ m) & (n ^ r)) >> 31; break;
  case OP_SUB: r = n - m; break;
  case OP_SUBC: tmp = n - m; r = tmp - t; *tn = (n < tmp) | (tmp < r); break;
  case OP_SUBV: r = n - m; *tn = ((n ^ m) & (n ^ r)) >> 31; break;
  case OP_NEG: r = 0 - m; break;
  case OP_NEGC: tmp = 0 - m; r = tmp - t; *tn = (0 < tmp) | (tmp < r); break;
  }
  *rn = r;
}

int main(void) {
  const struct operation ops[] = {
    OP(ADD), OP(ADDC), OP(ADDV), OP(SUB), OP(SUBC), OP(SUBV), OP(NEG), OP(NEGC),
  };
  const uint32_t inputs[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff,
                             0x12345678, 0xfedcba98};
  const unsigned count = sizeof(inputs) / sizeof(inputs[0]);
  unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(code != MAP_FAILED);
  unsigned failures = 0;
  for (unsigned op = 0; op < sizeof(ops) / sizeof(ops[0]); ++op) {
    const struct operation *s = &ops[op];
    unsigned checks = 0, failed = 0;
    assert(*s->src < *s->size && *s->dest < *s->size);
    for (unsigned rm = 0; rm < 16; ++rm) {
      for (unsigned rn = 0; rn < 16; ++rn) {
        memcpy(code, prologue, 16);
        memcpy(code + 16, s->code, *s->size);
        code[16 + *s->src] = rm * 4;
        code[16 + *s->dest] = rn * 4;
        memcpy(code + 16 + *s->size, epilogue, 12);
        __builtin___clear_cache((char *)code, (char *)code + 28 + *s->size);
        for (unsigned i = 0; i < count; ++i) {
          for (unsigned j = 0; j < count; ++j) {
            for (unsigned t = 0; t < 2; ++t) {
              uint32_t state[33], expected[33], m, n, result, tn;
              for (unsigned k = 0; k < 33; ++k) state[k] = 0x1000 + k;
              state[rn] = inputs[j];
              state[rm] = inputs[i]; /* Rm == Rn: both operands are Rm */
              state[16] = 0x3f2 | t;
              m = state[rm], n = state[rn];
              memcpy(expected, state, sizeof(state));
              reference(s->kind, m, n, t, &result, &tn);
              expected[rn] = result;
              expected[16] = (state[16] & ~1u) | tn;
              ((void (*)(uint32_t *))code)(state);
              ++checks;
              if (memcmp(state, expected, sizeof(state)) != 0) {
                if (failed++ == 0)
                  printf("%s r%u,r%u m=%08x n=%08x T=%u: Rn=%08x T=%u, "
                         "expected Rn=%08x T=%u\n", s->name, rm, rn, m, n, t,
                         state[rn], state[16] & 1, result, tn);
              }
            }
          }
        }
      }
    }
    printf("%-4s %u/%u cases passed\n", s->name, checks - failed, checks);
    failures += failed;
  }
  assert(munmap(code, 4096) == 0);
  printf("ARM arithmetic templates: %s\n", failures ? "FAIL" : "PASS");
  return failures != 0;
}
