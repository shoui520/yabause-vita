/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the real core, not a copy of its instruction decoder. */
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
static unsigned allow_dma, dma_reads;

void *VitaTelemetryEnter(VitaTelemetryPhase p) { (void)p; return NULL; }
void VitaTelemetryLeave(VitaTelemetryPhase p) { (void)p; }
void VitaTelemetryLeaveSample(void *s) { (void)s; abort(); }
/* Operation-only tests must not accidentally exercise an unmodelled device. */
u16 MappedMemoryReadWord(u32 a, u32 *c) { (void)a; (void)c; abort(); }
u32 MappedMemoryReadLong(u32 a, u32 *c) {
  (void)c;
  assert(allow_dma && a == 0x02000000 && dsp.RX == -7);
  ++dma_reads;
  return 0x12345678;
}
void MappedMemoryWriteWord(u32 a, u16 v, u32 *c) { (void)a; (void)v; (void)c; abort(); }
void MappedMemoryWriteLong(u32 a, u32 v, u32 *c) { (void)a; (void)v; (void)c; abort(); }
void SH2WriteNotify(u32 a, u32 n) { (void)a; (void)n; abort(); }
void SH2SendInterrupt(SH2_struct *s, u8 v, u8 l) { (void)s; (void)v; (void)l; abort(); }

static void reset(void) {
  allow_dma = dma_reads = 0;
  memset(&scu, 0, sizeof(scu));
  memset(&dsp, 0, sizeof(dsp));
  memset(&bp, 0, sizeof(bp));
  ScuRegs = &scu; ScuDsp = &dsp; ScuBP = &bp;
  dsp.ProgControlPort.part.EX = 1;
  dsp.jmpaddr = UINT32_MAX;
  dsp.AC.all = 17;
  dsp.P.all = 31;
  dsp.RX = -7;
  dsp.RY = 11;
  for (unsigned b = 0; b < 4; ++b)
    for (unsigned i = 0; i < 64; ++i)
      dsp.MD[b][i] = 0x80000100u + b * 64 + i;
}

int main(void) {
  unsigned cases = 0;
  const u32 edges[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678};
  const unsigned ops[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 15};
  for (unsigned opi = 0; opi < sizeof(ops) / sizeof(*ops); ++opi)
    for (unsigned ai = 0; ai < sizeof(edges) / sizeof(*edges); ++ai)
      for (unsigned pi = 0; pi < sizeof(edges) / sizeof(*edges); ++pi)
        for (unsigned flags = 0; flags < 16; ++flags) {
          reset();
          const unsigned op = ops[opi];
          const u32 a = edges[ai], p = edges[pi];
          dsp.AC.all = 0x123400000000LL | a;
          dsp.P.all = p;
          dsp.ProgControlPort.all |= flags << 19;
          const u32 old_control = dsp.ProgControlPort.all;
          u32 value = a, carry = 0;
          switch (op) {
            case 1: value = a & p; break;
            case 2: value = a | p; break;
            case 3: value = a ^ p; break;
            case 4: value = a + p; carry = ((u64)a + p) >> 32; break;
            case 5: value = a - p; carry = a < p; break;
            case 8: value = a / 2 | (a & 0x80000000); carry = a % 2; break;
            case 9: value = a / 2 | ((a % 2) * 0x80000000); carry = a % 2; break;
            case 10: value = a * 2; carry = a >> 31; break;
            case 11: value = a * 2 | (a >> 31); carry = a >> 31; break;
            case 15: value = (u32)((u64)a * 256) | (a >> 24); carry = (a >> 24) & 1; break;
          }
          dsp.ProgramRam[0] = (op << 26) | (2u << 17);
          ScuExec(1);
          assert((u32)dsp.ALU.part.L == value);
          assert(dsp.ALU.part.H == 0x1234 && dsp.AC.all == dsp.ALU.all);
          const u32 expected_flags = (carry << 20) | ((value == 0) << 21)
                                   | ((value >> 31) << 22);
          assert(dsp.ProgControlPort.all == (op
            ? (old_control & ~0x700000u) | expected_flags : old_control));
          ++cases;
        }
  /* ADD + MOV MUL,P + MOV MCx,X + MOV MCy,Y + MOV ALU,A.
   * The multiply reads OLD RX/RY; ADD reads OLD P/AC. Repeated MC reads
   * increment a bank only once, and CT wraps after all reads. SCU manual
   * operation command format p.91; X/Y bus commands pp.107 onward. */
  for (unsigned x = 0; x < 8; ++x)
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned ct = 0; ct < 64; ++ct) {
        reset();
        for (unsigned b = 0; b < 4; ++b) dsp.CT[b] = ct;
        const s32 rx = (s32)dsp.MD[x & 3][ct];
        const s32 ry = (s32)dsp.MD[y & 3][ct];
        dsp.ProgramRam[0] = (4u << 26) | (6u << 23) | (x << 20)
                         | (6u << 17) | (y << 14);
        ScuExec(1);
        assert(dsp.P.all == -77);
        assert(dsp.ALU.all == 48 && dsp.AC.all == 48);
        assert(dsp.RX == rx && dsp.RY == ry && dsp.PC == 1);
        for (unsigned b = 0; b < 4; ++b) {
          const unsigned inc = ((x & 4) && (x & 3) == b)
                             || ((y & 4) && (y & 3) == b);
          assert(dsp.CT[b] == ((ct + inc) & 63));
        }
        ++cases;
      }
  /* D1 immediate commits pending MC increments BEFORE its destination write;
   * register D1 reads/writes share the old CT and commit once afterwards.
   * Explicit D1 writes to CT cancel that bank's pending increment. */
  for (unsigned bank = 0; bank < 4; ++bank)
    for (unsigned ct = 0; ct < 64; ++ct)
      for (unsigned mode = 0; mode < 3; ++mode) {
        reset();
        dsp.CT[bank] = ct;
        const u32 before = dsp.MD[bank][ct];
        const unsigned dest = mode == 2 ? 12 + bank : bank;
        const unsigned source = mode == 1 ? 4 + bank : 0xfe;
        dsp.ProgramRam[0] = (4u << 23) | ((4 + bank) << 20)
                         | ((mode == 1 ? 3u : 1u) << 12) | (dest << 8) | source;
        ScuExec(1);
        assert((u32)dsp.RX == before);
        if (mode == 0) {
          assert(dsp.MD[bank][(ct + 1) & 63] == 0xfffffffe);
          assert(dsp.CT[bank] == ((ct + 2) & 63));
        } else if (mode == 1) {
          assert(dsp.MD[bank][ct] == before);
          assert(dsp.CT[bank] == ((ct + 1) & 63));
        } else assert(dsp.CT[bank] == 0xfe);
        ++cases;
      }
  /* With T0 set and wait > 1, the entry step alone does not complete DMA.
   * An MD operand must force completion, then use the updated bank pointer. */
  for (unsigned bank = 0; bank < 4; ++bank)
    for (unsigned ct = 0; ct < 64; ++ct) {
      reset();
      allow_dma = 1;
      dsp.CT[bank] = ct;
      const u32 expected = dsp.MD[bank][(ct + 1) & 63];
      dsp.ProgControlPort.part.T0 = 1;
      dsp.dsp_dma_wait = 5;
      dsp.dsp_dma_instruction = (bank << 8) | 1;
      dsp.RA0M = 0x02000000 / 4;
      dsp.ProgramRam[0] = (4u << 23) | (bank << 20);
      ScuExec(1);
      assert(dma_reads == 1 && dsp.MD[bank][ct] == 0x12345678);
      assert((u32)dsp.RX == expected && dsp.CT[bank] == ((ct + 1) & 63));
      assert(dsp.ProgControlPort.part.T0 == 0 && dsp.dsp_dma_wait == 0);
      ++cases;
    }
  printf("SCU DSP complete operation words: %u cases passed\n", cases);
}
