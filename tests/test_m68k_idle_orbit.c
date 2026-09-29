/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exact 68000 idle-orbit deferral against chunked reference execution on the
 * real C68K interpreter. The program mirrors a polling sound-driver main loop:
 * an ADDQ.W pass counter, a queue compare, a subroutine call (stack writes),
 * NOPs and an SCSP SCIPD timer-A poll; the tick handler clears the flag via
 * SCIRE, bumps state and does register/memory work. Both runs use 256-cycle
 * chunks with overshoot carry and the same timer model; the orbit run defers
 * whole chunks and materializes before a visible timer step and at each frame
 * end, exactly like scsp.c. Compared at every frame end: CPU state, carried
 * overshoot, all of sound RAM and the chunk-stamped SCSP write log.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/c68k/c68k.h"
#include "../src/core/c68k/idle_orbit.h"

static u8 ram[0x80000];
static u16 scipd, scieb;
static unsigned chunk_index, log_used;
static u32 write_log[4096][3];

static u16 RamWOf(const u8 *m, u32 a) { u16 v; memcpy(&v, m + (a & 0x7FFFE), 2); return v; }
static u16 RamW(u32 a) { u16 v; memcpy(&v, ram + (a & 0x7FFFE), 2); return v; }
static void RamSetW(u32 a, u16 v) { memcpy(ram + (a & 0x7FFFE), &v, 2); }
static u32 FASTCALL ReadB(const u32 a) {
  if (a < 0x80000) return ram[a ^ 1];
  if (a >= 0x100000) return (a & 1) ? (scipd & 0xFF) : (scipd >> 8); /* only SCIPD modeled */
  return 0;
}
static u32 FASTCALL ReadW(const u32 a) {
  if (a < 0x80000) return RamW(a);
  if (a >= 0x100000) return (a & 0xFFE) == 0x420 ? scipd : 0x1234;
  return 0;
}
static void Log(u32 a, u32 v) {
  assert(log_used < 4096);
  write_log[log_used][0] = chunk_index; write_log[log_used][1] = a; write_log[log_used++][2] = v;
}
static void FASTCALL WriteB(const u32 a, u32 v) {
  if (a < 0x80000) ram[a ^ 1] = (u8)v;
  else if (a >= 0x100000) Log(a, v & 0xFF);
}
static void FASTCALL WriteW(const u32 a, u32 v) {
  if (a < 0x80000) RamSetW(a, (u16)v);
  else if (a >= 0x100000) { Log(a, v & 0xFFFF); if ((a & 0xFFE) == 0x422) scipd &= (u16)~v; }
}
static s32 FASTCALL IntCb(s32 level) { (void)level; return C68K_INT_ACK_AUTOVECTOR; }

/* Timer model: timer A overflows every `period` chunks (sets SCIPD bit 6);
 * the 1-sample bit 10 is set on every step. */
static unsigned timer_count, timer_period;
static int TimerStepVisible(void) { return timer_count + 1 >= timer_period || !(scipd & 0x400) || (scieb & 0x400); }
static void TimerStep(void) {
  if (++timer_count >= timer_period) { timer_count = 0; scipd |= 0x40; }
  scipd |= 0x400;
}

static void Put(u32 *pc, u16 w) { RamSetW(*pc, w); *pc += 2; }
static void Patch(u32 at, u32 target) { RamSetW(at, (u16)(target - at)); }
static void Assemble(unsigned variant) {
  u32 pc = 0x1000, bne, bsr, beq, dbf, bra, sub;
  const u32 L = pc;
  if (variant != 1) { Put(&pc, 0x546E); Put(&pc, 0x1864); }  /* addq.w #2,$1864(a6) */
  if (variant == 2) { Put(&pc, 0x5C6E); Put(&pc, 0x1866); }  /* addq.w #6,$1866(a6) */
  Put(&pc, 0x7000);                                         /* moveq #0,d0 */
  Put(&pc, 0x102E); Put(&pc, 0x1842);                       /* move.b $1842(a6),d0 */
  Put(&pc, 0xB02E); Put(&pc, 0x1840);                       /* cmp.b $1840(a6),d0 */
  Put(&pc, 0x6600); bne = pc; Put(&pc, 0);                  /* bne.w busy */
  Put(&pc, 0x6100); bsr = pc; Put(&pc, 0);                  /* bsr.w sub */
  Put(&pc, 0x4E71); Put(&pc, 0x4E71); Put(&pc, 0x4E71);     /* nop x3 */
  Put(&pc, 0x302D); Put(&pc, 0x0420);                       /* move.w $420(a5),d0 */
  Put(&pc, 0x0240); Put(&pc, 0x0040);                       /* andi.w #$40,d0 */
  Put(&pc, 0x6700); beq = pc; Put(&pc, 0); Patch(beq, L);   /* beq.w L */
  Put(&pc, 0x3B7C); Put(&pc, 0x0040); Put(&pc, 0x0422);     /* move.w #$40,$422(a5) */
  Put(&pc, 0x522E); Put(&pc, 0x1827);                       /* addq.b #1,$1827(a6) */
  Put(&pc, 0x7214);                                         /* moveq #20,d1 */
  const u32 loop = pc;
  Put(&pc, 0xD481);                                         /* add.l d1,d2 */
  Put(&pc, 0x51C9); dbf = pc; Put(&pc, 0); Patch(dbf, loop); /* dbf d1,loop */
  Put(&pc, 0x2D42); Put(&pc, 0x1900);                       /* move.l d2,$1900(a6) */
  Put(&pc, 0x3B42); Put(&pc, 0x0418);                       /* move.w d2,$418(a5) */
  Put(&pc, 0x6000); bra = pc; Put(&pc, 0); Patch(bra, L);   /* bra.w L */
  Patch(bne, pc);                                           /* busy: */
  Put(&pc, 0x1D40); Put(&pc, 0x1840);                       /* move.b d0,$1840(a6) */
  Put(&pc, 0x6000); bra = pc; Put(&pc, 0); Patch(bra, L);
  sub = pc; Patch(bsr, sub);
  Put(&pc, 0x382E); Put(&pc, 0x186A);                       /* move.w $186A(a6),d4 */
  Put(&pc, 0xB86E); Put(&pc, 0x1868);                       /* cmp.w $1868(a6),d4 */
  Put(&pc, 0x4E75);                                         /* rts */
}

typedef struct { c68k_struc cpu; s32 saved; u8 ram[0x80000]; unsigned log_used; } Snapshot;
static c68k_struc *live;
static s32 FASTCALL Exec(s32 cycles) { return C68k_Exec(live, cycles); }

static void Setup(c68k_struc *cpu, unsigned variant, u16 counter) {
  memset(ram, 0, sizeof(ram));
  for (unsigned i = 0; i < 0x400; i += 2) RamSetW(i, 0x4E71);
  Assemble(variant);
  RamSetW(0x7864, counter); RamSetW(0x7866, (u16)(counter ^ 0x8000));
  ram[0x7842 ^ 1] = ram[0x7840 ^ 1] = 3;
  C68k_Init(cpu, IntCb);
  C68k_Set_ReadB(cpu, ReadB); C68k_Set_ReadW(cpu, ReadW);
  C68k_Set_WriteB(cpu, WriteB); C68k_Set_WriteW(cpu, WriteW);
  C68k_Set_Fetch(cpu, 0, 0x7FFFF, (pointer)ram);
  C68k_Reset(cpu);
  C68k_Set_AReg(cpu, 5, 0x100000); C68k_Set_AReg(cpu, 6, 0x6000); C68k_Set_AReg(cpu, 7, 0xA000);
  C68k_Set_SR(cpu, 0x2700); C68k_Set_PC(cpu, 0x1000);
  scipd = 0x400; scieb = 0; timer_count = 0; chunk_index = 0; log_used = 0;
}

static unsigned total_skipped, total_deferred, total_revalidated;
static void Run(unsigned variant, u16 counter, unsigned period, unsigned frames, int use_orbit,
                Snapshot *out) {
  static c68k_struc cpu;
  C68kIdleOrbit orbit = {0};
  static C68kOrbitProbeEnv envs[2];
  int rec = 0;              /* envs[rec] holds the last closed pass */
  s32 saved = 0;
  Setup(&cpu, variant, counter);
  live = &cpu; timer_period = period;
  memset(envs, 0, sizeof(envs));
  for (int i = 0; i < 2; ++i) { envs[i].ram = ram; envs[i].read_b = ReadB; envs[i].read_w = ReadW; }
  for (unsigned f = 0; f < frames; ++f) {
    for (unsigned c = 0; c < 735; ++c, ++chunk_index) {
      int deferred = 0;
      if (use_orbit) {
        if (orbit.active) { orbit.deferred += 256; deferred = 1; ++total_deferred; }
        else if (C68kOrbitEligible(&cpu)) {
          s32 p = C68kOrbitRevalidate(&cpu, &envs[rec], ram, scipd);
          if (p > 0) ++total_revalidated;
          else if ((p = C68kOrbitProbe(&cpu, &envs[1 - rec], 4096)) > 0) rec = 1 - rec;
          if (p > 0) { C68kOrbitEnter(&orbit, &envs[rec], p, saved, 256); deferred = 1; ++total_deferred; }
        }
      }
      if (!deferred) {
        s32 n = saved - 256;
        if (n < 0) n += C68k_Exec(&cpu, -n);
        saved = n;
      }
      if (orbit.active && TimerStepVisible()) saved = C68kOrbitMaterialize(&orbit, ram, Exec, &total_skipped);
      TimerStep();
    }
    if (orbit.active) saved = C68kOrbitMaterialize(&orbit, ram, Exec, &total_skipped);
    out[f].cpu = cpu; out[f].saved = saved; memcpy(out[f].ram, ram, sizeof(ram)); out[f].log_used = log_used;
  }
}
static int SameCpu(const c68k_struc *a, const c68k_struc *b) {
  /* Architectural state (raw flag fields carry unrelated result bits). */
  return a->PC - a->BasePC == b->PC - b->BasePC && !memcmp(a->D, b->D, 32) && !memcmp(a->A, b->A, 32) &&
    C68kOrbitCcr(a) == C68kOrbitCcr(b) && a->flag_I == b->flag_I &&
    a->flag_S == b->flag_S && a->USP == b->USP && a->Status == b->Status && a->IRQLine == b->IRQLine;
}

int main(void) {
  enum { FRAMES = 6 };
  static Snapshot ref[FRAMES], opt[FRAMES];
  static u32 ref_log[4096][3];
  static const u16 counters[] = {0x0000, 0x7F00, 0x7FFE, 0xFF00, 0xFFFE, 0x1235};
  static const unsigned periods[] = {3, 37, 97, 173, 500, 2000};
  unsigned cases = 0;
  C68k_Init(&(c68k_struc){0}, IntCb); /* build the jump table */
  for (unsigned variant = 0; variant < 3; ++variant)
    for (unsigned ci = 0; ci < sizeof(counters) / sizeof(counters[0]); ++ci)
      for (unsigned pi = 0; pi < sizeof(periods) / sizeof(periods[0]); ++pi) {
        Run(variant, counters[ci], periods[pi], FRAMES, 0, ref);
        const unsigned ref_used = log_used;
        memcpy(ref_log, write_log, sizeof(write_log));
        Run(variant, counters[ci], periods[pi], FRAMES, 1, opt);
        for (unsigned f = 0; f < FRAMES; ++f) {
          if (!SameCpu(&ref[f].cpu, &opt[f].cpu) || ref[f].saved != opt[f].saved ||
              memcmp(ref[f].ram, opt[f].ram, sizeof(ram)) || ref[f].log_used != opt[f].log_used) {
            fprintf(stderr, "mismatch variant=%u counter=%04x period=%u frame=%u pc=%x/%x saved=%d/%d log=%u/%u\n",
              variant, counters[ci], periods[pi], f,
              (unsigned)(ref[f].cpu.PC - ref[f].cpu.BasePC), (unsigned)(opt[f].cpu.PC - opt[f].cpu.BasePC),
              ref[f].saved, opt[f].saved, ref[f].log_used, opt[f].log_used);
            for (unsigned i = 0; i < 8; ++i)
              if (ref[f].cpu.D[i] != opt[f].cpu.D[i] || ref[f].cpu.A[i] != opt[f].cpu.A[i])
                fprintf(stderr, "  D%u %08x/%08x A%u %08x/%08x\n", i, ref[f].cpu.D[i], opt[f].cpu.D[i],
                        i, ref[f].cpu.A[i], opt[f].cpu.A[i]);
            fprintf(stderr, "  flags C%x/%x V%x/%x Z%x/%x N%x/%x X%x/%x\n", ref[f].cpu.flag_C, opt[f].cpu.flag_C,
              ref[f].cpu.flag_V, opt[f].cpu.flag_V, ref[f].cpu.flag_notZ, opt[f].cpu.flag_notZ,
              ref[f].cpu.flag_N, opt[f].cpu.flag_N, ref[f].cpu.flag_X, opt[f].cpu.flag_X);
            for (unsigned a = 0; a < 0x80000; a += 2)
              if (memcmp(ref[f].ram + a, opt[f].ram + a, 2))
                fprintf(stderr, "  ram %05x %04x/%04x\n", a, RamWOf(ref[f].ram, a), RamWOf(opt[f].ram, a));
            return 1;
          }
        }
        assert(ref_used == log_used && !memcmp(ref_log, write_log, ref_used * sizeof(write_log[0])));
        ++cases;
      }
  printf("68K idle orbit: %u configurations x %u frames identical; %u chunks deferred, %u periods skipped, %u re-entries without a probe\n",
         cases, FRAMES, total_deferred, total_skipped, total_revalidated);
  if (!total_deferred || !total_skipped || !total_revalidated) { fprintf(stderr, "orbit never engaged\n"); return 1; }
  return 0;
}
