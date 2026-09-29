/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test: VITA_SCU_DSP_FAST vs the reference DSP loop in the real
 * ScuExec. The reference loop runs whenever a code breakpoint is registered;
 * one at an unreachable address (>= 256) selects it with no behavioural
 * effect. Random programs (all instruction classes incl. DMA, jumps, loops,
 * END) and random states; compares the whole DSP register file, data RAM,
 * memory traffic (reads/writes logged in order) and DSP-end interrupts. */
#include "../src/core/scu.h"
#include "../src/core/sh2core.h"
#include "../src/core/yabause.h"
#include "../src/vita/telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern scudspregs_struct *ScuDsp;
extern scubp_struct *ScuBP;
yabsys_struct yabsys;
SH2_struct *MSH2, *SSH2;
u8 *HighWram;
static Scu scu;
static scudspregs_struct dsp;
static scubp_struct bp;
int VitaTelemetryMode = VT_OFF;
void *VitaTelemetryEnter(VitaTelemetryPhase p) { (void)p; return NULL; }
void VitaTelemetryLeave(VitaTelemetryPhase p) { (void)p; }
void VitaTelemetryLeaveSample(void *s) { (void)s; abort(); }

/* Memory model: reads are a pure function of the address; every access is
 * appended to a log that both runs must reproduce exactly. */
static u32 log_buf[1 << 16]; static unsigned log_n;
static void note(u32 kind, u32 a, u32 v) {
  if (log_n + 3 <= sizeof(log_buf) / sizeof(log_buf[0])) { log_buf[log_n++] = kind; log_buf[log_n++] = a; log_buf[log_n++] = v; }
}
static u32 mem_value(u32 a) { u32 x = a * 2654435761u; return x ^ (x >> 13); }
u8 MappedMemoryReadByte(u32 a, u32 *c) { if (c) *c = 0; note(1, a, 0); return (u8)mem_value(a); }
u16 MappedMemoryReadWord(u32 a, u32 *c) { if (c) *c = 0; note(2, a, 0); return (u16)mem_value(a); }
u32 MappedMemoryReadLong(u32 a, u32 *c) { if (c) *c = 0; note(3, a, 0); return mem_value(a); }
void MappedMemoryWriteByte(u32 a, u8 v, u32 *c) { if (c) *c = 0; note(4, a, v); }
void MappedMemoryWriteWord(u32 a, u16 v, u32 *c) { if (c) *c = 0; note(5, a, v); }
void MappedMemoryWriteLong(u32 a, u32 v, u32 *c) { if (c) *c = 0; note(6, a, v); }
void SH2WriteNotify(u32 a, u32 n) { note(7, a, n); }
void SH2SendInterrupt(SH2_struct *s, u8 v, u8 l) { (void)s; note(8, v, l); }
#include <stdarg.h>
void YuiMsg(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }

static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8 | (seed << 24); }

static u32 random_instruction(void) {
  const u32 r = rnd(), k = rnd() % 100;
  if (k < 55) return r & 0x3fffffff;                                 // operation commands
  if (k < 75) return 0x80000000u | (r & 0x3fffffff);                 // MVI (plain/conditional)
  if (k < 83) return 0xC0000000u | (r & 0x0fff83ff);                 // DMA, immediate count (bits 10-14 clear)
  if (k < 91) { static const u32 cond[] = {0x00, 0x41, 0x42, 0x43, 0x44, 0x48, 0x61, 0x62, 0x63, 0x64, 0x68, 0x40, 0x7f, 0x21};
                return 0xD0000000u | (cond[rnd() % 14] << 19) | (r & 0xff); } // JMP
  if (k < 97) return 0xE0000000u | (r & 0x08000000);                 // LPS / BTM
  return 0xF0000000u | (r & 0x08000000);                             // END / ENDI
}

/* Second phase: AD2-heavy programs from edge accumulators (48-bit carry,
 * sign bit 47, values beyond 48 bits after AD2 chains). */
static int ad2_phase;
static s64 edge_value(void) {
  static const s64 edges[] = {0, -1, 1, 0x7FFFFFFFFFFFLL, 0x800000000000LL, -0x800000000000LL,
                              0xFFFFFFFFFFFFLL, 0x1000000000000LL, 0x7FFFFFFFLL, -0x80000000LL,
                              0xFFFFFFFFLL, 0x100000000LL, 0x7FFFFFFFFFFFFFFFLL, (s64)0x8000000000000000ULL};
  const s64 v = edges[rnd() % (sizeof(edges) / sizeof(edges[0]))];
  return (rnd() & 1) ? v : v + (s64)(s32)(rnd() % 7) - 3;
}
static void randomize_state(scudspregs_struct *d) {
  memset(d, 0, sizeof(*d));
  for (unsigned i = 0; i < 256; ++i) {
    d->ProgramRam[i] = random_instruction();
    if (ad2_phase && (d->ProgramRam[i] >> 30) == 0 && (rnd() & 1))
      d->ProgramRam[i] = (d->ProgramRam[i] & 0x03ffffffu) | (6u << 26);
  }
  /* Small values half the time: MD words also serve as DMA counts. */
  const int small = rnd() & 1;
  for (unsigned b = 0; b < 4; ++b) for (unsigned i = 0; i < 64; ++i) d->MD[b][i] = small ? (rnd() & 0x3f) : rnd();
  d->ProgControlPort.all = rnd() & 0x007C0000u;                      // flags Z/S/C/V/T0 randomly
  if (rnd() & 1) d->ProgControlPort.part.T0 = 0;
  d->ProgControlPort.part.EX = 1;
  d->PC = (u8)rnd(); d->TOP = (u8)rnd(); d->LOP = (u16)(rnd() & 0xfff);
  d->jmpaddr = (rnd() % 8) ? -1 : (s32)(rnd() & 0xff); d->delayed = rnd() & 1;
  for (unsigned b = 0; b < 4; ++b) d->CT[b] = (u8)(rnd() & 0x3f);
  d->RX = (s32)rnd(); d->RY = (s32)rnd(); d->RA0 = rnd() & 0x1ffffff; d->WA0 = rnd() & 0x1ffffff;
  /* 48-bit sign-extended values, built without shifting negatives. */
  d->AC.all = (s64)((u64)(s64)(s32)rnd() * 65536u) ^ rnd();
  d->P.all = (s64)((u64)(s64)(s32)rnd() * 65536u) ^ rnd();
  if (ad2_phase) { d->AC.all = edge_value(); d->P.all = edge_value(); }
  d->ALU.all = rnd(); d->RA0M = rnd() & 0x1ffffff; d->WA0M = rnd() & 0x1ffffff;
  d->dsp_dma_wait = (rnd() % 4 == 0) ? 1 + rnd() % 2 : 0;
  d->dsp_dma_instruction = 0xC0000000u | (rnd() & 0x0fff83ff); d->dsp_dma_size = rnd() % 16;
}

static u8 *wram_init;
static void run(const scudspregs_struct *start, int reference, u32 timing, scudspregs_struct *out,
                u32 *log_out, unsigned *log_len) {
  memcpy(HighWram, wram_init, 0x100000);
#ifdef VITA_SCU_DSP_JIT
  { extern u32 scu_dsp_prog_gen; ++scu_dsp_prog_gen; }   /* program RAM written directly */
#endif
  memset(&scu, 0, sizeof(scu)); memset(&bp, 0, sizeof(bp));
  ScuRegs = &scu; ScuBP = &bp; ScuDsp = &dsp;
  dsp = *start;
  if (reference) { bp.numcodebreakpoints = 1; bp.codebreakpoint[0].addr = 0x1000; }
  log_n = 0;
  ScuExec(timing);
  *out = dsp;
  memcpy(log_out, log_buf, log_n * sizeof(u32)); *log_len = log_n;
}

int main(void) {
  static u32 log_a[1 << 16], log_b[1 << 16];
  unsigned cases = 0;
  HighWram = malloc(0x100000); wram_init = malloc(0x100000);
  u8 *wram_a = malloc(0x100000);
  for (unsigned i = 0; i < 0x100000; ++i) wram_init[i] = (u8)rnd();
  for (int t = 0; t < 8000; ++t) {
    ad2_phase = t >= 5000;
    scudspregs_struct start, a, b; unsigned na, nb;
    randomize_state(&start);
    const u32 timing = 1 + rnd() % 400;
    run(&start, 1, timing, &a, log_a, &na);
    memcpy(wram_a, HighWram, 0x100000);
    run(&start, 0, timing, &b, log_b, &nb);
    if (memcmp(wram_a, HighWram, 0x100000)) { printf("FAIL case %d: HighWram differs\n", t); return 1; }
    if (memcmp(&a, &b, sizeof(a)) || na != nb || memcmp(log_a, log_b, na * sizeof(u32))) {
      printf("FAIL case %d timing %u (state %s, log %s)\n", t, timing, memcmp(&a, &b, sizeof(a)) ? "differs" : "same",
             (na != nb || memcmp(log_a, log_b, na * sizeof(u32))) ? "differs" : "same");
      return 1;
    }
    ++cases;
  }
  printf("SCU DSP fast loop: %u random programs identical to the reference loop\n", cases);
#ifdef SCU_DSP_JIT_STATS
  { extern unsigned scu_dsp_jit_stats[2]; printf("jit runs=%u fallback-to-fast runs=%u\n", scu_dsp_jit_stats[1], scu_dsp_jit_stats[0]); }
#endif
  return 0;
}
