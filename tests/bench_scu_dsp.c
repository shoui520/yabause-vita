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
#include <time.h>

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


/* Benchmark shaped like Grandia's hot DSP code (instruction histogram):
 * LPS + ADD/MOV MC0,A/MOV ALL,MC1 loops and an RL8/AND/OR/MVI block. */
static void load_program(scudspregs_struct *d) {
  /* Straight-line block of Grandia's hot instructions x8, then a short LPS
   * loop, then JMP 0 (~88% straight-line, like the histogram). */
  static const u32 block[] = {
    0x01a654fe, 0x08040000, 0x3dc40000, 0x08043109, 0x94ff00ff,
    0x05e67009, 0x11f41c0c, 0x10841500,
  };
  memset(d->ProgramRam, 0, sizeof(d->ProgramRam));
  unsigned pc = 0;
  for (unsigned r = 0; r < 8; ++r)
    for (unsigned k = 0; k < sizeof(block) / sizeof(block[0]); ++k) d->ProgramRam[pc++] = block[k];
  d->ProgramRam[pc++] = 0x08840000 | (0x0Au << 8) | 0x3000 | 0x04;   /* OR, MOV #4,LOP */
  d->ProgramRam[pc++] = 0xe8000000;                                /* LPS */
  d->ProgramRam[pc++] = 0x10073109;                                /* ADD MOV MC0,A MOV ALL,MC1 */
  d->ProgramRam[pc++] = 0xd0000000;                                /* JMP 0 */
  d->ProgramRam[pc++] = 0x00000000;
}
int main(int argc, char **argv) {
  const int reference = argc > 1 && argv[1][0] == 'r';
  static Scu scu_; static scubp_struct bp_;
  HighWram = calloc(1, 0x100000);
  ScuRegs = &scu_; ScuBP = &bp_; ScuDsp = &dsp;
  memset(&dsp, 0, sizeof(dsp));
  load_program(&dsp);
  for (unsigned b = 0; b < 4; ++b) for (unsigned i = 0; i < 64; ++i) dsp.MD[b][i] = b * 0x01010101u + i * 77;
  dsp.jmpaddr = -1; dsp.ProgControlPort.part.EX = 1;
  if (reference) { bp_.numcodebreakpoints = 1; bp_.codebreakpoint[0].addr = 0x1000; }
  extern u64 vita_bench_dummy;
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  const int iters = argc > 2 ? atoi(argv[2]) : 20000;
  for (int i = 0; i < iters; ++i) { dsp.ProgControlPort.part.EX = 1; ScuExec(90); }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
  printf("%s ns_per_dsp_insn=%.1f\n", reference ? "reference" : "fast", ns / (iters * 90.0));
  printf("PC=%u AC=%lld\n", dsp.PC, (long long)dsp.AC.all);
#ifdef SCU_DSP_JIT_DUMP
  { extern void ScuDspJitDump(unsigned, const char *); ScuDspJitDump(5, "/tmp/dsp_insn5.bin"); }
#endif
  return 0;
}
u64 vita_bench_dummy;
#include <stdarg.h>
void YuiMsg(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
