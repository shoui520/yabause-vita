/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux ARM/QEMU test of the actual MAC.W template against a model of the
 * callback template's semantics (build with and without VITA_SH2_MACW_WRAM:
 * both must pass). Cached high work RAM (0x060xxxxx) is backed by `high`,
 * which the callback also returns, as memGetWord does; other addresses return
 * an address-derived pattern. Records every callback address.
 * S=1 overflow is not exercised: both templates dereference the sum's high
 * word there. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern unsigned char prologue[], epilogue[];
extern unsigned char x86_MAC_W[];
extern const unsigned short MAC_W_size;
extern const unsigned char MAC_W_src, MAC_W_dest;
#ifdef VITA_SH2_MACW_WRAM
extern void sh2_macw_saturate(void);
#endif

static uint8_t high[0x100000] __attribute__((aligned(4)));
static uint32_t calls[4], ncalls;

static uint16_t read_model(uint32_t a) {
  if ((a & 0xdff00000u) == 0x06000000u) return *(uint16_t *)(high + (a & 0xfffff));
  return (uint16_t)(a * 0x9e37u ^ (a >> 16));
}
static uint32_t get_word(uint32_t a) {
  assert(ncalls < 4); calls[ncalls++] = a;
  return read_model(a);
}

static uint32_t rng = 12345;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t address(void) {
  static const uint32_t bases[] = {0x06000000u, 0x060ffff0u, 0x26000000u, 0x00200000u,
                                   0x05a00000u, 0x06040000u, 0x07f00000u, 0x16000000u};
  return (bases[next() & 7] + (next() & 0xffe)) + ((next() & 31) == 0 ? 1 : 0);
}

int main(void) {
  for (unsigned i = 0; i < sizeof(high); ++i) high[i] = (uint8_t)(next() >> 3);
  unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(code != MAP_FAILED);
  assert(MAC_W_size == 42 * 4);
  unsigned checks = 0, inline_reads = 0;
  for (unsigned m = 0; m < 16; ++m)
    for (unsigned n = 0; n < 16; ++n) {
      memcpy(code, prologue, 16);
      memcpy(code + 16, x86_MAC_W, MAC_W_size);
      code[16 + MAC_W_src] = m * 4;
      code[16 + MAC_W_dest] = n * 4;
      memcpy(code + 16 + MAC_W_size, epilogue, 12);
      __builtin___clear_cache((char *)code, (char *)code + 28 + MAC_W_size);
      for (unsigned it = 0; it < 400; ++it) {
        uint32_t state[48], expected[48];
        for (unsigned j = 0; j < 48; ++j) state[j] = next();
        state[m] = address(); state[n] = m == n ? state[m] : address();
        const unsigned s_bit = (it & 3) == 3;
        state[16] = (state[16] & ~2u) | (s_bit << 1);
        state[26] = (uint32_t)(uintptr_t)get_word;       /* getmemword #104 */
        state[34] = (uint32_t)(uintptr_t)high;           /* spec_high #136 */
#ifdef VITA_SH2_MACW_WRAM
        state[41] = (uint32_t)(uintptr_t)sh2_macw_saturate; /* #164 */
#endif
        if (it & 1) state[20] = (uint32_t)(int32_t)(int16_t)next(); /* small MACL */
        /* Model of the callback template. */
        memcpy(expected, state, sizeof(state));
        const uint16_t a = read_model(expected[m]); expected[m] += 2;
        const uint16_t b = read_model(expected[n]); expected[n] += 2;
        const int32_t prod = (int16_t)a * (int16_t)b;
        const int64_t sum = (int64_t)(int32_t)expected[20] + prod;
        if (s_bit) {
          if (sum < INT32_MIN || sum > INT32_MAX) continue; /* see header */
          expected[20] = (uint32_t)sum;
        } else {
          expected[20] = (uint32_t)sum; expected[19] = (uint32_t)((uint64_t)sum >> 32);
        }
        ncalls = 0;
        ((void (*)(uint32_t *))code)(state);
        /* Callback addresses: every read without the inline path; with it,
         * exactly the reads outside 0x060xxxxx. */
        const uint32_t ra = state[m] - 2 - (m == n ? 2 : 0), rb = state[n] - 2;
        unsigned want = 0; uint32_t w[2];
#ifdef VITA_SH2_MACW_WRAM
        if ((ra >> 20) != 0x060) w[want++] = ra; else ++inline_reads;
        if ((rb >> 20) != 0x060) w[want++] = rb; else ++inline_reads;
#else
        w[want++] = ra; w[want++] = rb;
#endif
        assert(ncalls == want);
        for (unsigned k = 0; k < want; ++k) assert(calls[k] == w[k]);
        for (unsigned j = 0; j < 48; ++j)
          if (state[j] != expected[j]) {
            fprintf(stderr, "m=%u n=%u it=%u word %u: %08x != %08x\n", m, n, it, j, state[j], expected[j]);
            return 1;
          }
        ++checks;
      }
    }
  printf("MAC.W template%s: %u cases passed (%u inline reads)\n",
#ifdef VITA_SH2_MACW_WRAM
         " (inline WRAM)",
#else
         "",
#endif
         checks, inline_reads);
  return 0;
}
