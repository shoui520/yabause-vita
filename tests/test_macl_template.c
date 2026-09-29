/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test of the actual MAC.L template: build once with and once
 * without VITA_SH2_MACL_WRAM (tools/test_macl_template.sh) and compare the
 * printed digests. Operands come from cached high work RAM (inline in the
 * new template), cache-through and low work RAM and other areas (callback in
 * both); the callback returns T2ReadLong of `high` for 0x060xxxxx, as
 * memGetLong does. The digest covers every final register file (MACH/MACL
 * with S=0 and S=1, saturation included) and every callback address outside
 * 0x060xxxxx. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern unsigned char prologue[], epilogue[];
extern unsigned char x86_MAC_L[];
extern const unsigned short MAC_L_size;
extern const unsigned char MAC_L_src, MAC_L_dest;
#ifdef VITA_SH2_MACL_WRAM
extern void sh2_macl_saturate(void);
#endif

static uint8_t high[0x100000] __attribute__((aligned(4)));
static uint64_t digest = 1469598103934665603ull;
static void mix(uint32_t v) { for (int i = 0; i < 4; ++i) digest = (digest ^ ((v >> (8 * i)) & 255)) * 1099511628211ull; }
static unsigned inline_region_calls;

static uint32_t get_long(uint32_t a) {
  if ((a & 0xdff00000u) == 0x06000000u) {
    if (!(a & 0x20000000u)) ++inline_region_calls;
    const uint32_t w = *(uint32_t *)(high + (a & 0xfffff));
    return w >> 16 | w << 16;
  }
  mix(a);
  return a * 0x9e3779b1u ^ (a >> 13);
}

static uint32_t rng = 99;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t address(void) {
  static const uint32_t bases[] = {0x06000000u, 0x060ffff0u, 0x26000000u, 0x00200000u,
                                   0x05a00000u, 0x06040000u, 0x20200000u, 0x06080000u};
  return bases[next() & 7] + (next() & 0xffc);
}

int main(void) {
  for (unsigned i = 0; i < sizeof(high); ++i) high[i] = (uint8_t)next();
  unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(code != MAP_FAILED);
  assert(MAC_L_size == 52 * 4);
  unsigned cases = 0;
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n) {
      memcpy(code, prologue, 16);
      memcpy(code + 16, x86_MAC_L, MAC_L_size);
      code[16 + MAC_L_src] = m * 4;
      code[16 + MAC_L_dest] = n * 4;
      memcpy(code + 16 + MAC_L_size, epilogue, 12);
      __builtin___clear_cache((char *)code, (char *)code + 28 + MAC_L_size);
      for (unsigned it = 0; it < 400; ++it) {
        uint32_t state[48];
        for (unsigned j = 0; j < 48; ++j) state[j] = next();
        state[m] = address(); state[n] = m == n ? state[m] : address();
        state[16] = (state[16] & ~2u) | ((it & 1) << 1);                 /* S bit */
        if (it & 2) { state[19] = (uint32_t)((int32_t)next() >> 16); }   /* MACH near 48-bit range */
        state[27] = (uint32_t)(uintptr_t)get_long;                       /* getmemlong #108 */
        state[34] = (uint32_t)(uintptr_t)high;                           /* spec_high #136 */
#ifdef VITA_SH2_MACL_WRAM
        state[42] = (uint32_t)(uintptr_t)sh2_macl_saturate;              /* #168 */
#else
        state[42] = 0;
#endif
        ((void (*)(uint32_t *))code)(state);
        state[27] = state[34] = state[42] = 0;
        for (unsigned j = 0; j < 48; ++j) mix(state[j]);
        ++cases;
      }
    }
  printf("MAC.L template%s: %u cases digest %016llx\n",
#ifdef VITA_SH2_MACL_WRAM
         " (inline WRAM)",
#else
         "",
#endif
         cases, (unsigned long long)digest);
  fprintf(stderr, "callback reads of cached high work RAM: %u\n", inline_region_calls);
  return 0;
}
