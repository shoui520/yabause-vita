/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SCU DSP -> ARMv7 A32 translator (VITA_SCU_DSP_JIT). Included by scu.c after
 * the fast loop, whose single step (dsp_fast_step) is both the semantic
 * reference for every native sequence below and the fallback for the
 * instructions this translator does not lower (DMA commands, END, invalid
 * opcodes).
 *
 * Each of the 256 program words becomes a straight-line sequence with every
 * field decoded at translation time. Registers: r4 = DSP registers, r5 =
 * remaining clock, r6 = D1 value across a DMA completion call, r7 = entry
 * table. Nothing else is kept in registers across a helper call: DSP DMA
 * helpers read and write the register file (CT, MD, RA0/WA0, T0...).
 *
 * Order per instruction exactly as dsp_fast_step: T0 DMA step; ALU = AC;
 * ALU command on the OLD AC/P; X bus (P, then RX); Y bus (RY, then A);
 * D1 bus; pending MC post-increments; PC++; delayed-jump bookkeeping;
 * clock--. A bus read of M0-3/MC0-3 and every D1/immediate write first
 * complete a pending DSP DMA (dsp_dma_wait > 0), as the reference helpers do.
 *
 * Not used (the fast loop runs instead) while the program or the pending DMA
 * contains a dma03/dma07-family transfer: those write program RAM (stale
 * translation) and, for selectors 5-7, stray past MD into the registers. */
#ifndef SCU_DSP_JIT_H
#define SCU_DSP_JIT_H
#include <stddef.h>
#include <stdint.h>

extern u32 scu_dsp_prog_gen;          /* bumped by every program RAM writer (scu.c) */

#ifdef VITA
unsigned char *VitaDspCodeArena(size_t *capacity);
void VitaDspCodeWriteBegin(void *p, size_t n);
void VitaDspCodeWriteEnd(void *p, size_t n);
#else
#include <sys/mman.h>
static unsigned char *VitaDspCodeArena(size_t *capacity) {
  static unsigned char *mem;
  *capacity = 256 * 1024;
  if (!mem) {
    void *p = mmap(NULL, *capacity, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mem = p == MAP_FAILED ? NULL : (unsigned char *)p;
  }
  return mem;
}
static void VitaDspCodeWriteBegin(void *p, size_t n) { (void)p; (void)n; }
static void VitaDspCodeWriteEnd(void *p, size_t n) {
  __builtin___clear_cache((char *)p, (char *)p + n);
}
#endif

/* ---- A32 emitter ------------------------------------------------------- */
typedef struct { u32 *base, *cur, *end; int overflow; } DspEmit;
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, IP, SP, LR, PCR };
enum { EQ = 0x0u << 28, NE = 0x1u << 28, CS = 0x2u << 28, CC = 0x3u << 28, GT = 0xCu << 28,
       LE = 0xDu << 28, AL = 0xEu << 28 };

static void e32(DspEmit *e, u32 w) { if (e->cur < e->end) *e->cur++ = w; else e->overflow = 1; }
/* Modified immediate, or -1. */
static int arm_imm(u32 v) {
  for (unsigned rot = 0; rot < 16; ++rot) {
    const u32 r = (v << (2 * rot)) | (rot ? v >> (32 - 2 * rot) : 0);
    if (r <= 0xFF) return (int)((rot << 8) | r);
  }
  return -1;
}
enum { OP_AND = 0, OP_EOR = 1, OP_SUB = 2, OP_ADD = 4, OP_TST = 8, OP_CMP = 10, OP_CMN = 11,
       OP_ORR = 12, OP_MOV = 13, OP_BIC = 14, OP_MVN = 15 };
static void dp_imm(DspEmit *e, u32 cond, unsigned op, int s, unsigned rn, unsigned rd, u32 imm) {
  const int enc = arm_imm(imm);
  if (enc < 0) { e->overflow = 2; return; }
  e32(e, cond | (1u << 25) | (op << 21) | ((u32)s << 20) | (rn << 16) | (rd << 12) | (u32)enc);
}
/* shift types: 0 LSL, 1 LSR, 2 ASR, 3 ROR */
static void dp_reg(DspEmit *e, u32 cond, unsigned op, int s, unsigned rn, unsigned rd, unsigned rm,
                   unsigned type, unsigned amount) {
  e32(e, cond | (op << 21) | ((u32)s << 20) | (rn << 16) | (rd << 12) | ((amount & 31) << 7) | (type << 5) | rm);
}
static void mov_const(DspEmit *e, unsigned rd, u32 v) {
  if (arm_imm(v) >= 0) { dp_imm(e, AL, OP_MOV, 0, 0, rd, v); return; }
  if (arm_imm(~v) >= 0) { dp_imm(e, AL, OP_MVN, 0, 0, rd, ~v); return; }
  e32(e, AL | 0x03000000u | ((v & 0xF000) << 4) | (rd << 12) | (v & 0xFFF));            /* MOVW */
  if (v >> 16) e32(e, AL | 0x03400000u | ((v >> 12) & 0xF0000) | (rd << 12) | ((v >> 16) & 0xFFF)); /* MOVT */
}
/* LDR/STR word or byte, positive imm12 */
static void mem_imm(DspEmit *e, int load, int byte, unsigned rt, unsigned rn, unsigned off) {
  if (off > 4095) { e->overflow = 3; return; }
  e32(e, AL | 0x05800000u | ((u32)byte << 22) | ((u32)load << 20) | (rn << 16) | (rt << 12) | off);
}
#define LDR(e, rt, rn, off) mem_imm(e, 1, 0, rt, rn, off)
#define STR(e, rt, rn, off) mem_imm(e, 0, 0, rt, rn, off)
#define LDRB(e, rt, rn, off) mem_imm(e, 1, 1, rt, rn, off)
#define STRB(e, rt, rn, off) mem_imm(e, 0, 1, rt, rn, off)
/* LDRH/STRH through IP = r4 + off (imm8 form limited to 255) */
static void mem_half(DspEmit *e, int load, unsigned rt, unsigned off) {
  dp_imm(e, AL, OP_ADD, 0, R4, IP, off & ~0xFFu);
  e32(e, AL | 0x01C000B0u | ((u32)load << 20) | (IP << 16) | (rt << 12) | ((off & 0xF0) << 4) | (off & 0xF));
}
static void call_abs(DspEmit *e, const void *fn) {
  mov_const(e, IP, (u32)(uintptr_t)fn);
  e32(e, AL | 0x012FFF30u | IP);                                                        /* BLX ip */
}
/* Branches: emit placeholder, patch later. */
static u32 *branch_here(DspEmit *e, u32 cond) { u32 *at = e->cur; e32(e, cond | 0x0A000000u); return at; }
static void branch_patch(u32 *at, const u32 *target) {
  if (!at) return;
  *at = (*at & 0xFF000000u) | ((u32)((int32_t)(target - at) - 2) & 0xFFFFFFu);
}
static void branch_to(DspEmit *e, u32 cond, const u32 *target) { u32 *at = branch_here(e, cond); branch_patch(at, target); }

/* ---- translation -------------------------------------------------------- */
#define DOFF(f) ((unsigned)offsetof(scudspregs_struct, f))
/* AC (r8:r9) and P (r10:r11) live in registers inside translated code. */
static void mem_half(DspEmit *e, int load, unsigned rt, unsigned off);
static void emit_flush(DspEmit *e) {
  STR(e, R8, R4, DOFF(AC)); STR(e, R9, R4, DOFF(AC) + 4);
  STR(e, R10, R4, DOFF(P)); STR(e, R11, R4, DOFF(P) + 4);
#ifdef VITA_SCU_DSP_JIT_PCPREG
  STR(e, LR, R4, DOFF(ProgControlPort));
#endif
#ifdef VITA_SCU_DSP_JIT_CTREG
  /* CT[0..3] live in r7 (byte lanes, raw values). Clobbers r1 and ip. */
  mem_half(e, 0, R7, DOFF(CT));
  dp_reg(e, AL, OP_MOV, 0, 0, R1, R7, 1, 16);
  mem_half(e, 0, R1, DOFF(CT) + 2);
#endif
}
static void emit_load(DspEmit *e) {
  LDR(e, R8, R4, DOFF(AC)); LDR(e, R9, R4, DOFF(AC) + 4);
  LDR(e, R10, R4, DOFF(P)); LDR(e, R11, R4, DOFF(P) + 4);
#ifdef VITA_SCU_DSP_JIT_PCPREG
  LDR(e, LR, R4, DOFF(ProgControlPort));
#endif
#ifdef VITA_SCU_DSP_JIT_CTREG
  mem_half(e, 1, R7, DOFF(CT));
  mem_half(e, 1, IP, DOFF(CT) + 2);
  dp_reg(e, AL, OP_ORR, 0, R7, R7, IP, 0, 16);
#endif
}
typedef struct {
  u32 zm, sm, cm, t0m;                  /* ProgControlPort masks */
  u32 *entry[256];                      /* native entry per PC */
  u32 *dispatch, *dispatch_flush, *exit_store, *exit_nostore;
  u32 *load_stub[256];                  /* table targets: load AC/P, enter body */
  u32 gen; int valid, usable;
} DspJit;
static DspJit dsp_jit;
static void *dsp_jit_table[256];

static void dsp_jit_masks(void) {
  scudspregs_struct t;
  memset(&t, 0, sizeof(t)); t.ProgControlPort.part.Z = 1; dsp_jit.zm = t.ProgControlPort.all;
  memset(&t, 0, sizeof(t)); t.ProgControlPort.part.S = 1; dsp_jit.sm = t.ProgControlPort.all;
  memset(&t, 0, sizeof(t)); t.ProgControlPort.part.C = 1; dsp_jit.cm = t.ProgControlPort.all;
  memset(&t, 0, sizeof(t)); t.ProgControlPort.part.T0 = 1; dsp_jit.t0m = t.ProgControlPort.all;
}

static s32 dsp_jit_fallback(scudspregs_struct *d, s32 counter) { return dsp_fast_step(d, counter); }

/* Shared cold stubs, called with BL<cond> from the hot path (LR is saved by
 * the translator's prologue; the stubs keep SP 8-byte aligned). */
static u32 *dsp_jit_stub_sync, *dsp_jit_stub_t0;
static void bl_to(DspEmit *e, u32 cond, const u32 *target) {
  u32 *at = e->cur;
  e32(e, cond | 0x0B000000u | ((u32)((int32_t)(target - at) - 2) & 0xFFFFFFu));
}
/* Within one instruction, once a completion check has run, dsp_dma_wait is
 * <= 0 (it only ever holds 2, 1 or 0, and only DMA commands, which are
 * fallback instructions, raise it), so later checks are omitted. */
static int dsp_jit_synced;
/* Host registers currently equal to ALU.L / ALU.H within the instruction
 * being translated (-1: memory only). ALU is always stored as well; these
 * only let ALU reads skip the reload. */
static int dsp_alu_lo, dsp_alu_hi;
static void alu_clobber(int r) {
  if (dsp_alu_lo == r) dsp_alu_lo = -1;
  if (dsp_alu_hi == r) dsp_alu_hi = -1;
}
/* Bank b when r1 holds CT[b] & 63 from an MD access in this instruction, or
 * -1. The post-increment ((CT & 63) + 1) & 63 equals (CT + 1) & 63. */
static int dsp_ct_in_r1;
#if defined(VITA_SCU_DSP_JIT_PCPREG) && !defined(VITA_SCU_DSP_JIT_ENTRY_T0)
#error "lr is free in native DSP code only without the per-instruction BL stubs"
#endif
/* ProgControlPort in a host register (lr) inside native code, or memory. */
static unsigned pcp_get(DspEmit *e, unsigned scratch) {
#ifdef VITA_SCU_DSP_JIT_PCPREG
  (void)e; (void)scratch; return LR;
#else
  LDR(e, scratch, R4, DOFF(ProgControlPort)); return scratch;
#endif
}
static void pcp_put(DspEmit *e, unsigned r) {
#ifdef VITA_SCU_DSP_JIT_PCPREG
  (void)e; (void)r;
#else
  STR(e, r, R4, DOFF(ProgControlPort));
#endif
}
/* rd = CT[b] & 63 */
static void emit_ct_read(DspEmit *e, unsigned b, unsigned rd) {
#ifdef VITA_SCU_DSP_JIT_CTREG
  e32(e, AL | 0x07E00050u | (5u << 16) | (rd << 12) | ((8 * b) << 7) | R7);        /* UBFX rd, r7, #8b, #6 */
#else
  LDRB(e, rd, R4, DOFF(CT) + b);
  dp_imm(e, AL, OP_AND, 0, rd, rd, 0x3F);
#endif
}
#ifdef VITA_SCU_DSP_JIT_CTREG
/* CT[b] = (CT[b] + 1) & 63 within lane b (raw lane values may exceed 63). */
static void emit_ct_inc_lane(DspEmit *e, unsigned b) {
  dp_imm(e, AL, OP_BIC, 0, R7, R7, 0xC0u << (8 * b));
  dp_imm(e, AL, OP_ADD, 0, R7, R7, 1u << (8 * b));
  dp_imm(e, AL, OP_BIC, 0, R7, R7, 0x40u << (8 * b));
}
#endif
/* if (dsp_dma_wait > 0) { dsp_dma_wait = 0; step_dsp_dma(d); } */
static void emit_dma_sync(DspEmit *e) {
#ifdef VITA_SCU_DSP_JIT_ENTRY_T0
  /* Native code runs only with T0 == 0 and dsp_dma_wait == 0 (checked by the
   * load stubs); only DMA commands, which are fallback steps, change them. */
  (void)e; return;
#endif
  if (dsp_jit_synced) return;
  dsp_jit_synced = 1;
  LDR(e, R0, R4, DOFF(dsp_dma_wait));
  dp_imm(e, AL, OP_CMP, 1, R0, 0, 0);
  bl_to(e, GT, dsp_jit_stub_sync);
  alu_clobber(R2); alu_clobber(R3);                    /* the stub's call clobbers r1-r3 */
  dsp_ct_in_r1 = -1;
}
/* readgensrc(num) -> r2; records the MC post-increment in *inc. */
/* readgensrc(num) -> rd (r2, or r6 for the D1 bus). */
static void emit_read_to(DspEmit *e, unsigned num, unsigned *inc, unsigned rd) {
  if (num <= 7) {
    const unsigned b = num & 3;
    *inc |= ((num >> 2) & 1u) << b;
    emit_dma_sync(e);
    emit_ct_read(e, b, R1);
    dp_reg(e, AL, OP_ADD, 0, R4, IP, R1, 0, 2);
    LDR(e, rd, IP, DOFF(MD) + b * 256);
    alu_clobber((int)rd);
    dsp_ct_in_r1 = (int)b;
  } else if (num == 0x9) {
    if (dsp_alu_lo < 0) { LDR(e, rd, R4, DOFF(ALU)); dsp_alu_lo = (int)rd; }
    else if (dsp_alu_lo != (int)rd) dp_reg(e, AL, OP_MOV, 0, 0, rd, (unsigned)dsp_alu_lo, 0, 0);
  } else if (num == 0xA) {                             /* (u32)(ALU.all >> 16) */
    if (dsp_alu_lo >= 0 && dsp_alu_hi >= 0 && dsp_alu_hi != (int)rd) {
      dp_reg(e, AL, OP_MOV, 0, 0, rd, (unsigned)dsp_alu_lo, 1, 16);
      dp_reg(e, AL, OP_ORR, 0, rd, rd, (unsigned)dsp_alu_hi, 0, 16);
      alu_clobber((int)rd);
    } else {
      alu_clobber((int)rd); alu_clobber(R3);
      LDR(e, rd, R4, DOFF(ALU));
      LDR(e, R3, R4, DOFF(ALU) + 4);
      dp_reg(e, AL, OP_MOV, 0, 0, rd, rd, 1, 16);
      dp_reg(e, AL, OP_ORR, 0, rd, rd, R3, 0, 16);
    }
  } else {
    mov_const(e, rd, 0xFFFFFFFFu);
    alu_clobber((int)rd);
  }
}
static void emit_read(DspEmit *e, unsigned num, unsigned *inc) { emit_read_to(e, num, inc, R2); }
static void emit_ct_inc(DspEmit *e, unsigned inc) {
  for (unsigned b = 0; b < 4; ++b) if (inc & (1u << b)) {
#ifdef VITA_SCU_DSP_JIT_CTREG
    emit_ct_inc_lane(e, b); continue;
#endif
    if (dsp_ct_in_r1 == (int)b) dp_imm(e, AL, OP_ADD, 0, R1, R0, 1);
    else {
      LDRB(e, R0, R4, DOFF(CT) + b);
      dp_imm(e, AL, OP_ADD, 0, R0, R0, 1);
    }
    dp_imm(e, AL, OP_AND, 0, R0, R0, 0x3F);
    STRB(e, R0, R4, DOFF(CT) + b);
  }
}
/* writed1busdest(num, r6) */
static void emit_d1write(DspEmit *e, unsigned num, unsigned *inc) {
  emit_dma_sync(e);
  switch (num) {
    case 0x0: case 0x1: case 0x2: case 0x3:
      emit_ct_read(e, num, R1);
      dp_reg(e, AL, OP_ADD, 0, R4, IP, R1, 0, 2);
      STR(e, R6, IP, DOFF(MD) + num * 256);
      dsp_ct_in_r1 = (int)num;
      *inc |= 1u << num; return;
    case 0x4: STR(e, R6, R4, DOFF(RX)); return;
    case 0x5: dp_reg(e, AL, OP_MOV, 0, 0, R10, R6, 0, 0); dp_reg(e, AL, OP_MOV, 0, 0, R11, R6, 2, 31); return;
    case 0x6: STR(e, R6, R4, DOFF(RA0)); return;
    case 0x7: STR(e, R6, R4, DOFF(WA0)); return;
    case 0xA: mem_half(e, 0, R6, DOFF(LOP)); return;                 /* (u16)val */
    case 0xB: STRB(e, R6, R4, DOFF(TOP)); return;
    case 0xC: case 0xD: case 0xE: case 0xF:
#ifdef VITA_SCU_DSP_JIT_CTREG
      e32(e, AL | 0x07C00010u | ((8 * (num & 3) + 7) << 16) | (R7 << 12) | ((8 * (num & 3)) << 7) | R6); /* BFI r7, r6, #8b, #8 */
#else
      STRB(e, R6, R4, DOFF(CT) + (num & 3));
#endif
      *inc &= ~(1u << (num & 3));
      if (dsp_ct_in_r1 == (int)(num & 3)) dsp_ct_in_r1 = -1;
      return;
    default: return;
  }
}
/* Branch to *skip when the MVI/JMP condition (reference codes) is false.
 * Returns 0 for codes the reference never takes. */
static int emit_cond_false(DspEmit *e, unsigned c, u32 **skip) {
  u32 mask; int sense_true;
  switch (c) {
    case 0x01: mask = dsp_jit.zm; sense_true = 0; break;
    case 0x02: mask = dsp_jit.sm; sense_true = 0; break;
    case 0x03: mask = dsp_jit.zm | dsp_jit.sm; sense_true = 0; break;    /* Z==0 && S==0 */
    case 0x04: mask = dsp_jit.cm; sense_true = 0; break;
    case 0x08: mask = dsp_jit.t0m; sense_true = 0; break;
    case 0x21: mask = dsp_jit.zm; sense_true = 1; break;
    case 0x22: mask = dsp_jit.sm; sense_true = 1; break;
    case 0x23: mask = dsp_jit.zm | dsp_jit.sm; sense_true = 1; break;    /* Z || S */
    case 0x24: mask = dsp_jit.cm; sense_true = 1; break;
    case 0x28: mask = dsp_jit.t0m; sense_true = 1; break;
    default: return 0;
  }
  const unsigned pr = pcp_get(e, R0);
  if (arm_imm(mask) >= 0) dp_imm(e, AL, OP_TST, 1, pr, 0, mask);
  else { mov_const(e, R1, mask); dp_reg(e, AL, OP_TST, 1, pr, 0, R1, 0, 0); }
  *skip = branch_here(e, sense_true ? EQ : NE);
  return 1;
}
static int dsp_jit_native(u32 ins) {
  switch (ins >> 30) {
    case 0: return 1;
    case 2: return 1;
    case 3: return ((ins >> 28) & 0xF) == 0xD || ((ins >> 28) & 0xF) == 0xE;
    default: return 0;
  }
}
/* Instructions that can leave jmpaddr pending for the next one: JMP, LPS/BTM,
 * MVI to PC, and (conservatively) every fallback instruction. */
static int dsp_jit_sets_jump(u32 ins) {
  if (!dsp_jit_native(ins)) return 1;
  switch (ins >> 30) {
    case 2: return ((ins >> 26) & 0xF) == 0xC;
    case 3: return 1;
    default: return 0;
  }
}
static u32 *dsp_jit_stub_pending;
/* ALU in memory already equals AC (r8:r9) / its high word equals AC.H at the
 * start of the instruction that follows `prev` by fall-through, so the
 * reference's "ALU = AC" store is redundant. Every other entry (load stubs,
 * taken jumps) stores ALU = AC explicitly. */
static int dsp_jit_alu32(unsigned aluop) {
  return aluop == 1 || aluop == 2 || aluop == 3 || aluop == 4 || aluop == 5 ||
         aluop == 8 || aluop == 9 || aluop == 10 || aluop == 11 || aluop == 15;
}
static void dsp_jit_alu_after(u32 prev, int *all, int *high) {
  *all = 1; *high = 1;
  if (!dsp_jit_native(prev) || (prev >> 30) != 0) return;   /* fallback: re-entered via a load stub */
  const unsigned aluop = prev >> 26, abus = (prev >> 17) & 3;
  if (abus == 2) return;                                    /* AC = ALU */
  if (abus == 1 || abus == 3) { *all = 0; *high = 0; return; }
  if (aluop == 6) { *all = 0; *high = 0; }
  else if (dsp_jit_alu32(aluop)) *all = 0;
}
static int dsp_jit_forbidden_dma(u32 ins) {           /* dma03 / dma07 family */
  return (ins >> 28) == 0xC && ((ins >> 11) & 7) == 4;
}

/* Direct branches to entries not yet emitted, patched after translation. */
typedef struct { u32 *at; unsigned target; } DspFix;
static DspFix dsp_jit_fix[512]; static unsigned dsp_jit_nfix;
static void branch_entry(DspEmit *e, u32 cond, unsigned target) {
  if (dsp_jit.entry[target]) { branch_to(e, cond, dsp_jit.entry[target]); return; }
  u32 *at = branch_here(e, cond);
  if (dsp_jit_nfix < sizeof(dsp_jit_fix) / sizeof(dsp_jit_fix[0])) dsp_jit_fix[dsp_jit_nfix++] = (DspFix){at, target};
  else e->overflow = 4;
}
/* Jump target the previous instruction may have set (it takes effect after
 * this one): LPS -> its own PC, JMP -> imm, MVI to PC -> imm. -1 if none. */
static int dsp_jit_static_target(unsigned prev_pc, u32 prev) {
  if ((prev >> 28) == 0xE && (prev & 0x8000000)) return (int)prev_pc;          /* LPS */
  if ((prev >> 28) == 0xD) return (int)(prev & 0xFF);                          /* JMP */
  if ((prev >> 30) == 2 && ((prev >> 26) & 0xF) == 0xC) return (int)(prev & 0xFF); /* MVI imm,PC */
  return -1;
}
static void emit_instruction(DspEmit *e, DspEmit *c, unsigned pc, u32 ins, u32 prev) {
  const unsigned next = (pc + 1) & 0xFF;
  dsp_jit.entry[pc] = e->cur;
  if (!dsp_jit_native(ins)) {
    emit_flush(e);
    mov_const(e, R0, pc); STRB(e, R0, R4, DOFF(PC));
    dp_reg(e, AL, OP_MOV, 0, 0, R0, R4, 0, 0);
    dp_reg(e, AL, OP_MOV, 0, 0, R1, R5, 0, 0);
    call_abs(e, (const void *)dsp_jit_fallback);
    dp_reg(e, AL, OP_MOV, 1, 0, R5, R0, 0, 0);        /* MOVS r5, r0 */
    branch_to(e, LE, dsp_jit.exit_nostore);
    LDRB(e, R0, R4, DOFF(PC));
    branch_to(e, AL, dsp_jit.dispatch);
    return;
  }
  unsigned inc = 0;
  dsp_jit_synced = 0;
  /* T0: complete one DSP DMA step before the fetch. */
#ifdef VITA_SCU_DSP_JIT_ENTRY_T0
  /* T0 is 0 here (see emit_dma_sync); AD2 below takes r0 = ProgControlPort. */
#ifndef VITA_SCU_DSP_JIT_PCPREG
  if ((ins >> 26) == 6) LDR(e, R0, R4, DOFF(ProgControlPort));      /* type 0, AD2 */
#endif
#else
  LDR(e, R0, R4, DOFF(ProgControlPort));
  if (arm_imm(dsp_jit.t0m) >= 0) dp_imm(e, AL, OP_TST, 1, R0, 0, dsp_jit.t0m);
  else { mov_const(e, R1, dsp_jit.t0m); dp_reg(e, AL, OP_TST, 1, R0, 0, R1, 0, 0); }
  bl_to(e, NE, dsp_jit_stub_t0);
#endif
  /* ALU = AC (AC lives in r8:r9, P in r10:r11), except the words the ALU
   * command below overwrites: all of ALU for AD2, ALU.L for 32-bit commands. */
  const unsigned type = ins >> 30;
  const unsigned aluop0 = type == 0 ? ins >> 26 : 0;
  const int alu32 = dsp_jit_alu32(aluop0);
  int alu_all, alu_high;
  dsp_jit_alu_after(prev, &alu_all, &alu_high);
  if (aluop0 != 6) {
    if (!alu32 && !alu_all) STR(e, R8, R4, DOFF(ALU));
    if (!alu_all && !alu_high) STR(e, R9, R4, DOFF(ALU) + 4);
  }
  dsp_alu_lo = R8; dsp_alu_hi = R9;
  dsp_ct_in_r1 = -1;
  if (type == 0) {
    const unsigned aluop = ins >> 26;
    if (aluop == 6) {                                  /* AD2: ALU = AC + P (64-bit) */
      /* C = carry out of the 48-bit add of the low 48 bits: the high words'
       * low halves shifted up, with 0xFFFF below one of them so the low
       * word's carry reaches bit 48. Z on all 64 bits, S = bit 47.
       * r0 = ProgControlPort from the T0 check (the T0 stub reloads it). */
      dp_reg(e, AL, OP_MOV, 0, 0, R1, R9, 0, 16);
      dp_imm(e, AL, OP_ORR, 0, R1, R1, 0xFF);
      dp_imm(e, AL, OP_ORR, 0, R1, R1, 0xFF00);
      dp_reg(e, AL, OP_MOV, 0, 0, IP, R11, 0, 16);
      dp_reg(e, AL, OP_ADD, 1, R8, R2, R10, 0, 0);     /* ADDS r2, r8, r10 */
      e32(e, AL | 0x00A00000u | (R9 << 16) | (R3 << 12) | R11);           /* ADC  r3, r9, r11 */
      e32(e, AL | 0x00B00000u | (R1 << 16) | (R1 << 12) | IP);            /* ADCS r1, r1, ip */
#ifdef VITA_SCU_DSP_JIT_PCPREG
      const unsigned pr = LR;
#else
      const unsigned pr = R0;
#endif
      dp_imm(e, AL, OP_BIC, 0, pr, pr, 0x700000);
      dp_imm(e, CS, OP_ORR, 0, pr, pr, 0x100000);
      dp_reg(e, AL, OP_ORR, 1, R2, R1, R3, 0, 0);      /* ORRS r1, r2, r3 */
      dp_imm(e, EQ, OP_ORR, 0, pr, pr, 0x200000);
      dp_imm(e, AL, OP_TST, 1, R3, 0, 0x8000);
      dp_imm(e, NE, OP_ORR, 0, pr, pr, 0x400000);
      pcp_put(e, pr);
      STR(e, R2, R4, DOFF(ALU)); STR(e, R3, R4, DOFF(ALU) + 4);
      dsp_alu_lo = R2; dsp_alu_hi = R3;
    } else if (dsp_jit_alu32(aluop)) {
      /* Operands AC.L = r8, P.L = r10 (old values). The value goes to r6 when
       * the D1 bus moves ALL (saves the copy), else r2.
       * S/Z/C = N/Z/C of the flag-setting A32 op where they coincide with the
       * reference (MRS APSR >> 9 lands N,Z,C on bits 22,21,20). */
      const unsigned rv = (((ins >> 12) & 3) == 3 && (ins & 0xF) == 0x9) ? R6 : R2;
      int apsr = 1; u32 flip = 0;
      switch (aluop) {
        case 1: dp_reg(e, AL, OP_AND, 1, R8, rv, R10, 0, 0); break;        /* C from ANDS = old C: cleared below */
        case 2: dp_reg(e, AL, OP_ORR, 1, R8, rv, R10, 0, 0); break;
        case 3: dp_reg(e, AL, OP_EOR, 1, R8, rv, R10, 0, 0); break;
        case 4: dp_reg(e, AL, OP_ADD, 1, R8, rv, R10, 0, 0); break;
        case 5: dp_reg(e, AL, OP_SUB, 1, R8, rv, R10, 0, 0); flip = 0x100000; break; /* borrow = !C */
        case 8: dp_reg(e, AL, OP_MOV, 1, 0, rv, R8, 2, 1); break;          /* ASR #1: C = bit0 */
        case 9: dp_reg(e, AL, OP_MOV, 1, 0, rv, R8, 3, 1); break;          /* ROR #1: C = bit0 */
        case 10: dp_reg(e, AL, OP_MOV, 1, 0, rv, R8, 0, 1); break;         /* LSL #1: C = bit31 */
        default: apsr = 0; break;
      }
      if (apsr) {
        STR(e, rv, R4, DOFF(ALU));                     /* ALU.L = value, ALU.H/unused = AC's */
        e32(e, AL | 0x010F0000u | (R3 << 12));          /* MRS r3, APSR */
        const unsigned pr = pcp_get(e, R1);
        dp_imm(e, AL, OP_AND, 0, R3, R3, (aluop <= 3) ? 0xC0000000u : 0xE0000000u);
        dp_imm(e, AL, OP_BIC, 0, pr, pr, 0x700000);
        dp_reg(e, AL, OP_ORR, 0, pr, pr, R3, 1, 9);
        if (flip) dp_imm(e, AL, OP_EOR, 0, pr, pr, flip);
        pcp_put(e, pr);
      } else {                                         /* RL, RL8: explicit carry */
        if (aluop == 11) { dp_reg(e, AL, OP_MOV, 0, 0, R3, R8, 1, 31); dp_reg(e, AL, OP_MOV, 0, 0, rv, R8, 3, 31); }
        else { dp_reg(e, AL, OP_MOV, 0, 0, R3, R8, 1, 24); dp_imm(e, AL, OP_AND, 0, R3, R3, 1);
               dp_reg(e, AL, OP_MOV, 0, 0, rv, R8, 3, 24); }
        STR(e, rv, R4, DOFF(ALU));
        const unsigned pr = pcp_get(e, R1);
        dp_imm(e, AL, OP_BIC, 0, pr, pr, 0x700000);
        dp_reg(e, AL, OP_ORR, 0, pr, pr, R3, 0, 20);
        dp_imm(e, AL, OP_CMP, 1, rv, 0, 0);
        dp_imm(e, EQ, OP_ORR, 0, pr, pr, 0x200000);
        dp_imm(e, AL, OP_TST, 1, rv, 0, 0x80000000u);
        dp_imm(e, NE, OP_ORR, 0, pr, pr, 0x400000);
        pcp_put(e, pr);
      }
      dsp_alu_lo = (int)rv;                            /* ALU.H stays AC.H = r9 */
    }
    dsp_ct_in_r1 = -1;                                 /* the ALU command uses r1 */
    /* X bus: P first (MUL uses the old RX/RY), then RX. */
    switch ((ins >> 23) & 3) {
      case 2:
        dsp_ct_in_r1 = -1;
        LDR(e, R0, R4, DOFF(RX)); LDR(e, R1, R4, DOFF(RY));
        e32(e, AL | 0x00C00090u | (R11 << 16) | (R10 << 12) | (R1 << 8) | R0); /* SMULL r10, r11, r0, r1 */
        break;
      case 3:
        emit_read(e, (ins >> 20) & 7, &inc);
        dp_reg(e, AL, OP_MOV, 0, 0, R10, R2, 0, 0); dp_reg(e, AL, OP_MOV, 0, 0, R11, R2, 2, 31);
        break;
      default: break;
    }
    if ((ins >> 23) & 4) { emit_read(e, (ins >> 20) & 7, &inc); STR(e, R2, R4, DOFF(RX)); }
    if ((ins >> 17) & 4) { emit_read(e, (ins >> 14) & 7, &inc); STR(e, R2, R4, DOFF(RY)); }
    switch ((ins >> 17) & 3) {
      case 1: dp_imm(e, AL, OP_MOV, 0, 0, R8, 0); dp_imm(e, AL, OP_MOV, 0, 0, R9, 0);
              alu_clobber(R8); alu_clobber(R9); break;
      case 2:
        if (dsp_alu_lo >= 0 && dsp_alu_hi >= 0) {      /* lo is r2/r8, hi is r3/r9 */
          if (dsp_alu_lo != R8) dp_reg(e, AL, OP_MOV, 0, 0, R8, (unsigned)dsp_alu_lo, 0, 0);
          if (dsp_alu_hi != R9) dp_reg(e, AL, OP_MOV, 0, 0, R9, (unsigned)dsp_alu_hi, 0, 0);
        } else { LDR(e, R8, R4, DOFF(ALU)); LDR(e, R9, R4, DOFF(ALU) + 4); }
        dsp_alu_lo = R8; dsp_alu_hi = R9;
        break;
      case 3: emit_read(e, (ins >> 14) & 7, &inc);
              dp_reg(e, AL, OP_MOV, 0, 0, R8, R2, 0, 0); dp_reg(e, AL, OP_MOV, 0, 0, R9, R2, 2, 31);
              alu_clobber(R8); alu_clobber(R9); break;
      default: break;
    }
    switch ((ins >> 12) & 3) {
      case 1:                                           /* MOV SImm,[d]: pending increments first */
        emit_ct_inc(e, inc); inc = 0;
        mov_const(e, R6, (u32)(s32)(signed char)(ins & 0xFF));
        emit_d1write(e, (ins >> 8) & 0xF, &inc);
        break;
      case 3:
        emit_read_to(e, ins & 0xF, &inc, R6);
        emit_d1write(e, (ins >> 8) & 0xF, &inc);
        break;
      default: break;
    }
  } else if (type == 2) {                              /* MVI */
    const unsigned dest = (ins >> 26) & 0xF;
    u32 value; u32 *skip = NULL; int emit = 1;
    if ((ins >> 25) & 1) {
      value = (ins & 0x7FFFF) | ((ins & 0x40000) ? 0xFFF80000u : 0);
      emit = emit_cond_false(e, (ins >> 19) & 0x3F, &skip);
    } else {
      value = ins & 0x1FFFFFF; if (value & 0x1000000) value |= 0xfe000000u;
    }
    if (emit) {
      emit_dma_sync(e);
      switch (dest) {
        case 0x0: case 0x1: case 0x2: case 0x3:
          mov_const(e, R6, value);
          emit_ct_read(e, dest, R1);
          dp_reg(e, AL, OP_ADD, 0, R4, R2, R1, 0, 2);
          STR(e, R6, R2, DOFF(MD) + dest * 256);
#ifdef VITA_SCU_DSP_JIT_CTREG
          emit_ct_inc_lane(e, dest);
          break;
#endif
          /* Post-increment of the stored CT (not CT&63), as at the end of the reference step;
           * nothing later in an MVI reads CT. */
          LDRB(e, R1, R4, DOFF(CT) + dest);
          dp_imm(e, AL, OP_ADD, 0, R1, R1, 1);
          dp_imm(e, AL, OP_AND, 0, R1, R1, 0x3F);
          STRB(e, R1, R4, DOFF(CT) + dest);
          break;
        case 0x4: mov_const(e, R6, value); STR(e, R6, R4, DOFF(RX)); break;
        case 0x5: mov_const(e, R10, value); mov_const(e, R11, (s32)value < 0 ? 0xFFFFFFFFu : 0); break;
        case 0x6: mov_const(e, R6, value & 0x1FFFFFF); STR(e, R6, R4, DOFF(RA0)); break;
        case 0x7: mov_const(e, R6, value & 0x1FFFFFF); STR(e, R6, R4, DOFF(WA0)); break;
        case 0xA: mov_const(e, R6, value & 0x0FFF); mem_half(e, 0, R6, DOFF(LOP)); break;
        case 0xC:
          mov_const(e, R6, next); STRB(e, R6, R4, DOFF(TOP));
          mov_const(e, R6, value); STR(e, R6, R4, DOFF(jmpaddr));
          dp_imm(e, AL, OP_MOV, 0, 0, R6, 0); STR(e, R6, R4, DOFF(delayed));
          break;
        default: break;
      }
      branch_patch(skip, e->cur);
    }
  } else {                                             /* type 3: JMP (0xD) or LPS/BTM (0xE) */
    if (((ins >> 28) & 0xF) == 0xD) {
      LDR(e, R0, R4, DOFF(jmpaddr));
      dp_imm(e, AL, OP_CMN, 1, R0, 0, 1);
      u32 *busy = branch_here(e, NE);
      const unsigned c = (ins >> 19) & 0x7F;
      u32 *skip = NULL; int take = 1;
      if (c != 0x00) take = (c & 0x40) ? emit_cond_false(e, c & 0x3F, &skip) : 0;
      if (take) {
        mov_const(e, R1, ins & 0xFF); STR(e, R1, R4, DOFF(jmpaddr));
        dp_imm(e, AL, OP_MOV, 0, 0, R1, 0); STR(e, R1, R4, DOFF(delayed));
      }
      branch_patch(skip, e->cur);
      branch_patch(busy, e->cur);
    } else {
      mem_half(e, 1, R0, DOFF(LOP));
      dp_imm(e, AL, OP_CMP, 1, R0, 0, 0);
      u32 *skip = branch_here(e, EQ);
      if (ins & 0x8000000) mov_const(e, R1, pc);      /* LPS: jmpaddr = PC */
      else LDRB(e, R1, R4, DOFF(TOP));                 /* BTM: jmpaddr = TOP */
      STR(e, R1, R4, DOFF(jmpaddr));
      dp_imm(e, AL, OP_MOV, 0, 0, R1, 0); STR(e, R1, R4, DOFF(delayed));
      dp_imm(e, AL, OP_SUB, 0, R0, R0, 1);
      mem_half(e, 0, R0, DOFF(LOP));
      branch_patch(skip, e->cur);
    }
  }
  emit_ct_inc(e, inc);
  /* PC++ and the delayed-jump bookkeeping, then clock--. Hot path falls
   * through to entry[next]; jump and exit handling live in the cold region. */
  /* jmpaddr can only be pending here if this or the previous instruction set
   * it: every other way in (dispatch) passes the load stubs, which single-step
   * pending jumps through the fallback, and taken jumps clear it. */
  u32 *jump = NULL;
  if (dsp_jit_sets_jump(ins) || dsp_jit_sets_jump(prev)) {
    LDR(e, R0, R4, DOFF(jmpaddr));
    dp_imm(e, AL, OP_CMN, 1, R0, 0, 1);
    jump = branch_here(e, NE);
  }
  u32 *cont = e->cur;
  dp_imm(e, AL, OP_SUB, 1, R5, R5, 1);
  u32 *done = branch_here(e, LE);
  if (next == 0) branch_entry(e, AL, 0);
  if (jump) {
  /* cold: jmpaddr pending */
  branch_patch(jump, c->cur);
  LDR(c, R1, R4, DOFF(delayed));
  dp_imm(c, AL, OP_CMP, 1, R1, 0, 0);
  u32 *now = branch_here(c, NE);
  dp_imm(c, AL, OP_MOV, 0, 0, R1, 1); STR(c, R1, R4, DOFF(delayed));
  branch_to(c, AL, cont);
  branch_patch(now, c->cur);                           /* PC = (u8)jmpaddr; clock unchanged */
  dp_imm(c, AL, OP_AND, 0, R0, R0, 0xFF);
  mov_const(c, R1, 0xFFFFFFFFu); STR(c, R1, R4, DOFF(jmpaddr));
  STR(c, R8, R4, DOFF(ALU)); STR(c, R9, R4, DOFF(ALU) + 4);   /* entry invariant: ALU = AC */
  { const int t = dsp_jit_static_target((pc - 1) & 0xFF, prev);
    if (t >= 0) { dp_imm(c, AL, OP_CMP, 1, R0, 0, (u32)t); branch_entry(c, EQ, (unsigned)t); } }
  branch_to(c, AL, dsp_jit.dispatch_flush);
  }
  branch_patch(done, c->cur);                          /* clock spent: PC = next */
  mov_const(c, R0, next);
  branch_to(c, AL, dsp_jit.exit_store);
}

#ifdef SCU_DSP_JIT_STATS
unsigned scu_dsp_jit_stats[2];
#endif
#ifdef SCU_DSP_JIT_PERFMAP
static u32 *dsp_jit_hot_end, *dsp_jit_cold, *dsp_jit_cold_end;
/* Profiling only: perf map lines for the current translation (one symbol per
 * DSP PC, plus the prologue and cold regions). */
void ScuDspJitPerfMap(FILE *f) {
  if (!dsp_jit.usable || !dsp_jit.entry[0]) return;
  for (unsigned pc = 0; pc < 256; ++pc) {
    const u32 *a = dsp_jit.entry[pc], *b = pc < 255 ? dsp_jit.entry[pc + 1] : dsp_jit_hot_end;
    fprintf(f, "%lx %lx dsp_%02x\n", (unsigned long)(uintptr_t)a, (unsigned long)((b - a) * 4), pc);
  }
  fprintf(f, "%lx %lx dsp_cold\n", (unsigned long)(uintptr_t)dsp_jit_cold,
          (unsigned long)((dsp_jit_cold_end - dsp_jit_cold) * 4));
}
#endif
typedef void (*DspJitFn)(scudspregs_struct *d, s32 counter);
static DspJitFn dsp_jit_fn;

static int dsp_jit_compile(const scudspregs_struct *d) {
  size_t cap; unsigned char *mem = VitaDspCodeArena(&cap);
  if (!mem) return 0;
  if (!dsp_jit.zm) dsp_jit_masks();
  DspEmit e = {(u32 *)mem, (u32 *)mem, (u32 *)(mem + cap), 0};
  VitaDspCodeWriteBegin(mem, cap);
  /* Prologue: push {r4-r8, lr}; r4 = d; r5 = clock; r7 = table; dispatch on d->PC. */
  u32 *start = e.cur;
  e32(&e, AL | 0x092D4FF8u);                                     /* PUSH {r3-r11, lr} */
  dp_reg(&e, AL, OP_MOV, 0, 0, R4, R0, 0, 0);
  dp_reg(&e, AL, OP_MOV, 0, 0, R5, R1, 0, 0);
#ifndef VITA_SCU_DSP_JIT_CTREG
  mov_const(&e, R7, (u32)(uintptr_t)dsp_jit_table);
#endif
  LDRB(&e, R0, R4, DOFF(PC));
  dsp_jit.dispatch = e.cur;                                      /* r0 = PC; AC/P in memory */
#ifdef VITA_SCU_DSP_JIT_CTREG
  u32 *dispatch_table = e.cur;                                   /* r7 holds CT: table via ip */
  mov_const(&e, IP, (u32)(uintptr_t)dsp_jit_table);
  e32(&e, AL | 0x07900100u | (IP << 16) | (PCR << 12) | R0);     /* LDR pc, [ip, r0, lsl #2] */
  dsp_jit.dispatch_flush = e.cur;                                /* r0 = PC; AC/P in registers */
  emit_flush(&e);
  branch_to(&e, AL, dispatch_table);
#else
  e32(&e, AL | 0x07900100u | (R7 << 16) | (PCR << 12) | R0);     /* LDR pc, [r7, r0, lsl #2] */
  dsp_jit.dispatch_flush = e.cur;                                /* r0 = PC; AC/P in registers */
  emit_flush(&e);
  e32(&e, AL | 0x07900100u | (R7 << 16) | (PCR << 12) | R0);
#endif
  dsp_jit.exit_store = e.cur;                                    /* r0 = PC; AC/P in registers */
  emit_flush(&e);
  STRB(&e, R0, R4, DOFF(PC));
  dsp_jit.exit_nostore = e.cur;                                  /* AC/P in memory */
  e32(&e, AL | 0x08BD8FF8u);                                     /* POP {r3-r11, pc} */
  /* Hot code first half, cold paths second half of the region. */
  DspEmit c = {(u32 *)(mem + cap / 2), (u32 *)(mem + cap / 2), (u32 *)(mem + cap), 0};
  e.end = (u32 *)(mem + cap / 2);
  memset(dsp_jit.entry, 0, sizeof(dsp_jit.entry));
  dsp_jit_nfix = 0;
  dsp_jit_stub_sync = c.cur;                                     /* wait = 0; step_dsp_dma(d) */
  e32(&c, AL | 0x092D4001u);                                     /* PUSH {r0, lr} */
  emit_flush(&c);
  dp_imm(&c, AL, OP_MOV, 0, 0, R0, 0);
  STR(&c, R0, R4, DOFF(dsp_dma_wait));
  dp_reg(&c, AL, OP_MOV, 0, 0, R0, R4, 0, 0);
  call_abs(&c, (const void *)step_dsp_dma);
  emit_load(&c);
  e32(&c, AL | 0x08BD8001u);                                     /* POP {r0, pc} */
  dsp_jit_stub_t0 = c.cur;                                       /* step_dsp_dma(d); r0 = new PCP */
  e32(&c, AL | 0x092D4001u);
  emit_flush(&c);
  dp_reg(&c, AL, OP_MOV, 0, 0, R0, R4, 0, 0);
  call_abs(&c, (const void *)step_dsp_dma);
  emit_load(&c);
  e32(&c, AL | 0x08BD4001u);                                     /* POP {r0, lr} */
  LDR(&c, R0, R4, DOFF(ProgControlPort));
  e32(&c, AL | 0x012FFF1Eu);                                     /* BX lr */
  dsp_jit_stub_pending = c.cur;                                  /* r0 = PC, jmpaddr pending, AC/P in memory */
  STRB(&c, R0, R4, DOFF(PC));
  dp_reg(&c, AL, OP_MOV, 0, 0, R0, R4, 0, 0);
  dp_reg(&c, AL, OP_MOV, 0, 0, R1, R5, 0, 0);
  call_abs(&c, (const void *)dsp_jit_fallback);                  /* one reference step */
  dp_reg(&c, AL, OP_MOV, 1, 0, R5, R0, 0, 0);                    /* MOVS r5, r0 */
  branch_to(&c, LE, dsp_jit.exit_nostore);
  LDRB(&c, R0, R4, DOFF(PC));
  branch_to(&c, AL, dsp_jit.dispatch);
  for (unsigned pc = 0; pc < 256; ++pc)
    emit_instruction(&e, &c, pc, d->ProgramRam[pc], d->ProgramRam[(pc - 1) & 0xFF]);
#ifdef SCU_DSP_JIT_PERFMAP
  dsp_jit_hot_end = e.cur; dsp_jit_cold = (u32 *)(mem + cap / 2); dsp_jit_cold_end = c.cur;
#endif
  for (unsigned f = 0; f < dsp_jit_nfix; ++f) branch_patch(dsp_jit_fix[f].at, dsp_jit.entry[dsp_jit_fix[f].target]);
  for (unsigned pc = 0; pc < 256; ++pc) {                        /* dispatch targets */
    dsp_jit.load_stub[pc] = c.cur;
    emit_load(&c);
    STR(&c, R8, R4, DOFF(ALU)); STR(&c, R9, R4, DOFF(ALU) + 4);  /* entry invariant: ALU = AC */
    LDR(&c, R1, R4, DOFF(jmpaddr));
    dp_imm(&c, AL, OP_CMN, 1, R1, 0, 1);
    branch_to(&c, NE, dsp_jit_stub_pending);
#ifdef VITA_SCU_DSP_JIT_ENTRY_T0
    /* DMA in progress or pending: reference steps until it completes. */
    LDR(&c, R1, R4, DOFF(ProgControlPort));
    if (arm_imm(dsp_jit.t0m) >= 0) dp_imm(&c, AL, OP_TST, 1, R1, 0, dsp_jit.t0m);
    else { mov_const(&c, IP, dsp_jit.t0m); dp_reg(&c, AL, OP_TST, 1, R1, 0, IP, 0, 0); }
    branch_to(&c, NE, dsp_jit_stub_pending);
    LDR(&c, R1, R4, DOFF(dsp_dma_wait));
    dp_imm(&c, AL, OP_CMP, 1, R1, 0, 0);
    branch_to(&c, NE, dsp_jit_stub_pending);
#endif
    branch_to(&c, AL, dsp_jit.entry[pc]);
  }
  VitaDspCodeWriteEnd(mem, cap);
  if (e.overflow || c.overflow) return 0;
  for (unsigned pc = 0; pc < 256; ++pc) dsp_jit_table[pc] = dsp_jit.load_stub[pc];
  dsp_jit_fn = (DspJitFn)(void *)start;
  return 1;
}

static void ScuDspRunJit(s32 counter) {
  scudspregs_struct *const d = ScuDsp;
  if (!dsp_jit.valid || dsp_jit.gen != scu_dsp_prog_gen) {
    dsp_jit.gen = scu_dsp_prog_gen;
    dsp_jit.usable = 1;
    for (unsigned pc = 0; pc < 256; ++pc) if (dsp_jit_forbidden_dma(d->ProgramRam[pc])) dsp_jit.usable = 0;
    dsp_jit.valid = 1;
    if (dsp_jit.usable) dsp_jit.usable = dsp_jit_compile(d);
  }
  if (!dsp_jit.usable || (d->ProgControlPort.part.T0 && dsp_jit_forbidden_dma(d->dsp_dma_instruction | 0xC0000000u))) {
#ifdef SCU_DSP_JIT_STATS
    ++scu_dsp_jit_stats[0];
#endif
    ScuDspRunFast(counter);
    return;
  }
#ifdef SCU_DSP_JIT_STATS
  ++scu_dsp_jit_stats[1];
#endif
  dsp_jit_fn(d, counter);
}
/* Startup check on the device: random DMA- and END-free programs through the
 * translator and the fast loop (itself differentially tested against the
 * reference); the whole register file must match. Restores the live DSP. */
int ScuDspJitSelfTest(void) {
  scudspregs_struct *const d = ScuDsp;
  static scudspregs_struct saved, start, expect;
  saved = *d;
  u32 seed = 0x5C0D5B;
  #define JR() (seed = seed * 1664525u + 1013904223u, (seed >> 8) | (seed << 24))
  int bad = -1;
  for (int t = 0; t < 300 && bad < 0; ++t) {
    memset(&start, 0, sizeof(start));
    for (unsigned i = 0; i < 256; ++i) {
      const u32 r = JR(), k = JR() % 100;
      static const u32 cond[] = {0x00, 0x41, 0x42, 0x43, 0x44, 0x48, 0x61, 0x62, 0x63, 0x64, 0x68, 0x40};
      start.ProgramRam[i] = k < 60 ? (r & 0x3fffffff) & ~(0xFu << 26) | ((JR() % 16) << 26)
                          : k < 82 ? 0x80000000u | (r & 0x3fffffff)
                          : k < 92 ? 0xD0000000u | (cond[JR() % 12] << 19) | (r & 0xff)
                          : 0xE0000000u | (r & 0x08000000);
    }
    for (unsigned b = 0; b < 4; ++b) for (unsigned i = 0; i < 64; ++i) start.MD[b][i] = JR();
    start.ProgControlPort.all = JR() & 0x00700000u;
    start.ProgControlPort.part.EX = 1;
    start.PC = (u8)JR(); start.TOP = (u8)JR(); start.LOP = (u16)(JR() & 0xfff);
    start.jmpaddr = -1; start.delayed = 1;
    for (unsigned b = 0; b < 4; ++b) start.CT[b] = (u8)(JR() & 0x3f);
    start.RX = (s32)JR(); start.RY = (s32)JR();
    start.AC.all = (s64)(s32)JR(); start.P.all = (s64)(s32)JR();
    const s32 timing = 1 + (s32)(JR() % 300);
    *d = start; ScuDspRunFast(timing); expect = *d;
    *d = start; ++scu_dsp_prog_gen; ScuDspRunJit(timing);
    if (memcmp(&expect, d, sizeof(expect))) bad = t;
  }
  #undef JR
  *d = saved; ++scu_dsp_prog_gen;
  if (bad >= 0) { YuiMsg("scu_dsp_jit_test_failed case=%d", bad); return -1; }
  YuiMsg("scu_dsp_jit_test_pass cases=300 native=%d", dsp_jit.usable);
  return 0;
}
#ifdef SCU_DSP_JIT_DUMP
void ScuDspJitDump(unsigned pc, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  const u32 *a = dsp_jit.entry[pc], *b = dsp_jit.entry[(pc + 1) & 0xFF];
  fwrite(a, 4, (size_t)(b - a), f);
  fclose(f);
}
#endif
#endif
