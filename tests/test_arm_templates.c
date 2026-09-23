/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux ARM/QEMU test of the actual ARM templates, not a C reimplementation.
 * SH7604 hardware manual, instruction summary: SHLL/SHLR 2/8/16 leave T alone.
 * This does not test the complete SH-2 compiler or Vita VM publication. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern unsigned char prologue[], epilogue[];
extern unsigned char seperator_normal[], PageFlip[];
#define DECLARE(name) extern unsigned char x86_##name[]; \
  extern const unsigned short name##_size; \
  extern const unsigned char name##_dest;
DECLARE(SHLL2) DECLARE(SHLL8) DECLARE(SHLL16)
DECLARE(SHLR2) DECLARE(SHLR8) DECLARE(SHLR16)
DECLARE(SHL) DECLARE(SHLR) DECLARE(SHAR)
DECLARE(BT) DECLARE(BF)

struct operation {
  const unsigned char *code;
  const unsigned short *size;
  const unsigned char *dest;
  unsigned shift, right;
};
#define OP(name, shift, right) {x86_##name, &name##_size, &name##_dest, shift, right}

int main(void) {
  const struct operation ops[] = {
    OP(SHLL2, 2, 0), OP(SHLL8, 8, 0), OP(SHLL16, 16, 0),
    OP(SHLR2, 2, 1), OP(SHLR8, 8, 1), OP(SHLR16, 16, 1),
    OP(SHL, 1, 0), OP(SHLR, 1, 1), OP(SHAR, 1, 2),
  };
  const uint32_t inputs[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff,
                             0x12345678, 0xaaaa5555};
  unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(code != MAP_FAILED);
  unsigned checks = 0;
  for (unsigned op = 0; op < sizeof(ops)/sizeof(ops[0]); ++op) {
    const struct operation *s = &ops[op];
    for (unsigned reg = 0; reg < 16; ++reg) {
      memcpy(code, prologue, 16);
      memcpy(code + 16, s->code, *s->size);
      assert(*s->dest < *s->size);
      code[16 + *s->dest] = reg * 4;
      memcpy(code + 16 + *s->size, epilogue, 12);
      __builtin___clear_cache((char *)code, (char *)code + 28 + *s->size);
      for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        for (unsigned t = 0; t < 2; ++t) {
          uint32_t state[33], expected[33];
          for (unsigned j = 0; j < 33; ++j) state[j] = 0x1000 + j;
          state[reg] = inputs[i];
          state[16] = 0x3f2 | t;
          memcpy(expected, state, sizeof(state));
          expected[reg] = s->right ? inputs[i] >> s->shift : inputs[i] << s->shift;
          if (s->right == 2) expected[reg] |= inputs[i] & 0x80000000u;
          if (s->shift == 1) {
            const unsigned bit = s->right ? inputs[i] & 1 : inputs[i] >> 31;
            expected[16] = (state[16] & ~1u) | bit;
          }
          ((void (*)(uint32_t *))code)(state);
          assert(memcmp(state, expected, sizeof(state)) == 0);
          ++checks;
        }
      }
    }
  }
  const unsigned char *branches[] = {x86_BT, x86_BF};
  const unsigned sizes[] = {BT_size, BF_size};
  for (unsigned branch = 0; branch < 2; ++branch) {
    memcpy(code, prologue, 16);
    memcpy(code + 16, branches[branch], sizes[branch]);
    code[16] = 1; // displacement +1 -> current PC + 6
    memcpy(code + 16 + sizes[branch], seperator_normal, 8);
    code[16 + sizes[branch] + 4] = 3; // compiler's branch cycle charge
    memcpy(code + 24 + sizes[branch], PageFlip, 32);
    __builtin___clear_cache((char *)code, (char *)code + 56 + sizes[branch]);
    for (unsigned t = 0; t < 2; ++t) {
      uint32_t state[33] = {0}, expected[33];
      state[16] = 0x3f2 | t;
      state[22] = 0x060ff006;
      memcpy(expected, state, sizeof(state));
      const unsigned taken = branch == 0 ? t : !t;
      expected[22] += taken ? 6 : 2;
      expected[23] = taken ? 3 : 1;
      ((void (*)(uint32_t *))code)(state);
      assert(memcmp(state, expected, sizeof(state)) == 0);
      ++checks;
    }
  }
  assert(munmap(code, 4096) == 0);
  printf("ARM shift/branch templates: %u cases passed\n", checks);
  return 0;
}
