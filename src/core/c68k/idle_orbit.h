/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exact 68000 idle-orbit deferral core, shared by the SCSP worker and tests.
 *
 * Probe: dry-run a COPY of the CPU one instruction at a time until it
 * returns to the exact starting state (PC, D/A, CCR/X/I/S, USP, status,
 * interrupt line). The copy may read sound RAM, the unmapped window below
 * 0x100000 and the one allowed side-effect-free register (SCIPD). Its sound
 * RAM writes go to a private overlay that its later reads see; the orbit is
 * accepted only if, when the state closes, every overlaid word equals sound
 * RAM's current contents (one pass leaves memory unchanged), except affine
 * counters: a word accessed only by one ADDQ.W #q,<mem> in the pass whose
 * add neither carries nor changes sign. Every later pass then computes the
 * same flags (C=X=V=Z=0, same N) and nothing else observes the value, so k
 * passes add k*q; materialization never skips past the sign/carry limit.
 * Anything else, including an interrupt acknowledge, aborts. The period returned is the
 * exact cycle count of one pass; live CPU, sound RAM and SCSP are untouched.
 *
 * Deferral: while the inputs of the pass cannot change (caller's duty),
 * chunk budgets are summed instead of executed. Materialization runs the sum
 * from the orbit start: whole periods leave the state identical, so only the
 * remainder executes. The caller's executor must carry each chunk's
 * overshoot (C68K's savedcycles), which makes one execution of the summed
 * budget stop at the same instruction and overshoot as chunked executions,
 * given that no interrupt can be taken at the intermediate chunk entries.
 */
#ifndef C68K_IDLE_ORBIT_H
#define C68K_IDLE_ORBIT_H
#include <string.h>
#include "c68k.h"

enum { C68K_ORBIT_OVERLAY = 32, C68K_ORBIT_READS = 512, C68K_ORBIT_AFFINE = 4,
       C68K_ORBIT_WRITES = 96 };
/* Architectural register state at an instruction boundary. */
typedef struct { u32 D[8], A[8], ccr, I, S, USP; } C68kOrbitArch;
typedef struct {
  int active;
  s32 period, entry_saved, deferred;
  unsigned affine_used;
  u32 affine_addr[C68K_ORBIT_AFFINE];   /* word-aligned sound RAM address */
  u16 affine_q[C68K_ORBIT_AFFINE];
} C68kIdleOrbit;

typedef struct {
  const u8 *ram;               /* 512 KiB T2-layout sound RAM */
  unsigned overlay_used;
  u32 overlay_addr[C68K_ORBIT_OVERLAY];  /* word-aligned sound RAM address */
  u16 overlay_value[C68K_ORBIT_OVERLAY];
  u32 overlay_pc[C68K_ORBIT_OVERLAY];    /* instruction that last wrote the word */
  u16 overlay_writes[C68K_ORBIT_OVERLAY], overlay_step[C68K_ORBIT_OVERLAY];
  unsigned step, reads_used;             /* sound RAM read log: word, step */
  u32 read_addr[C68K_ORBIT_READS];
  u16 read_step[C68K_ORBIT_READS];
  u16 read_value[C68K_ORBIT_READS];      /* sound RAM word when read from RAM */
  u8 read_real[C68K_ORBIT_READS];        /* 1: from RAM, 0: from the overlay */
  unsigned writes_used;                  /* write timeline: step, word, value after */
  u32 write_addr[C68K_ORBIT_WRITES];
  u16 write_step[C68K_ORBIT_WRITES], write_value[C68K_ORBIT_WRITES];
  int scipd_seen; u16 scipd_value;
  unsigned steps; s32 period;            /* of the recorded closed pass */
  C68kOrbitArch step_state[C68K_ORBIT_READS];
  unsigned affine_used;
  u32 affine_addr[C68K_ORBIT_AFFINE];
  u16 affine_q[C68K_ORBIT_AFFINE];
  u32 step_pc[C68K_ORBIT_READS];         /* instruction addresses of the pass */
  const c68k_struc *copy;
  C68K_READ *read_b, *read_w;  /* the live callbacks (side-effect free for allowed ranges) */
  int abort;
  u32 abort_addr;              /* diagnostics: access that aborted the probe */
} C68kOrbitProbeEnv;

static C68kOrbitProbeEnv *c68k_orbit_env;
static inline int C68kOrbitScipd(u32 adr) { return adr >= 0x100000 && (adr & 0xFFE) == 0x420; }
static inline u16 C68kOrbitRamWord(const u8 *ram, u32 adr) {
  u16 v; memcpy(&v, ram + (adr & 0x7FFFE), 2); return v; /* T2: host-order halfwords */
}
/* Overlay slot for a word address, or -1. */
static inline int C68kOrbitFind(const C68kOrbitProbeEnv *e, u32 word) {
  for (unsigned i = 0; i < e->overlay_used; ++i) if (e->overlay_addr[i] == word) return (int)i;
  return -1;
}
static inline void C68kOrbitStore(C68kOrbitProbeEnv *e, u32 adr, u32 data, int width) {
  const u32 word = adr & 0x7FFFE;
  int i = C68kOrbitFind(e, word);
  if (i < 0) {
    if (e->overlay_used == C68K_ORBIT_OVERLAY) { e->abort = 8; e->abort_addr = adr; return; }
    i = (int)e->overlay_used++;
    e->overlay_addr[i] = word; e->overlay_value[i] = C68kOrbitRamWord(e->ram, word);
    e->overlay_writes[i] = 0;
  }
  e->overlay_pc[i] = (u32)(e->copy->PC - e->copy->BasePC);
  ++e->overlay_writes[i]; e->overlay_step[i] = (u16)e->step;
  if (width == 2) e->overlay_value[i] = (u16)data;
  else if (adr & 1) e->overlay_value[i] = (u16)((e->overlay_value[i] & 0xFF00) | (data & 0xFF));
  else e->overlay_value[i] = (u16)((e->overlay_value[i] & 0x00FF) | ((data & 0xFF) << 8));
  if (e->writes_used == C68K_ORBIT_WRITES) { e->abort = 11; e->abort_addr = adr; return; }
  e->write_addr[e->writes_used] = word; e->write_step[e->writes_used] = (u16)e->step;
  e->write_value[e->writes_used++] = e->overlay_value[i];
}
static inline void C68kOrbitLogRead(C68kOrbitProbeEnv *e, u32 adr) {
  if (e->reads_used == C68K_ORBIT_READS) { e->abort = 10; e->abort_addr = adr; return; }
  const u32 word = adr & 0x7FFFE;
  const int in_overlay = C68kOrbitFind(e, word) >= 0;
  e->read_addr[e->reads_used] = word; e->read_step[e->reads_used] = (u16)e->step;
  e->read_real[e->reads_used] = (u8)!in_overlay;
  e->read_value[e->reads_used++] = in_overlay ? 0 : C68kOrbitRamWord(e->ram, word);
}
static inline void C68kOrbitSeeScipd(C68kOrbitProbeEnv *e, u32 adr) {
  const u16 v = (u16)e->read_w((adr & ~1u));
  if (!e->scipd_seen) { e->scipd_seen = 1; e->scipd_value = v; }
  else if (e->scipd_value != v) { e->abort = 12; e->abort_addr = adr; }
}
static inline u32 FASTCALL C68kOrbitReadB(const u32 adr) {
  if (adr < 0x80000) {
    C68kOrbitLogRead(c68k_orbit_env, adr);
    const int i = C68kOrbitFind(c68k_orbit_env, adr & 0x7FFFE);
    if (i >= 0) return (adr & 1) ? c68k_orbit_env->overlay_value[i] & 0xFF : c68k_orbit_env->overlay_value[i] >> 8;
  }
  if (C68kOrbitScipd(adr)) C68kOrbitSeeScipd(c68k_orbit_env, adr);
  if (adr < 0x100000 || C68kOrbitScipd(adr)) return c68k_orbit_env->read_b(adr);
  c68k_orbit_env->abort = 1; c68k_orbit_env->abort_addr = adr; return 0;
}
static inline u32 FASTCALL C68kOrbitReadW(const u32 adr) {
  if (adr < 0x80000) {
    C68kOrbitLogRead(c68k_orbit_env, adr);
    const int i = C68kOrbitFind(c68k_orbit_env, adr & 0x7FFFE);
    if (i >= 0) return c68k_orbit_env->overlay_value[i];
  }
  if (C68kOrbitScipd(adr)) C68kOrbitSeeScipd(c68k_orbit_env, adr);
  if (adr < 0x100000 || C68kOrbitScipd(adr)) return c68k_orbit_env->read_w(adr);
  c68k_orbit_env->abort = 2; c68k_orbit_env->abort_addr = adr; return 0;
}
/* Writes to 0x80000-0xFFFFF are ignored by the live bus (c68k_*_write). */
static inline void FASTCALL C68kOrbitWriteB(const u32 adr, u32 data) {
  if (adr >= 0x100000) { c68k_orbit_env->abort = 3; c68k_orbit_env->abort_addr = adr; return; }
  if (adr < 0x80000) C68kOrbitStore(c68k_orbit_env, adr, data, 1);
}
static inline void FASTCALL C68kOrbitWriteW(const u32 adr, u32 data) {
  if (adr >= 0x100000) { c68k_orbit_env->abort = 4; c68k_orbit_env->abort_addr = adr; return; }
  if (adr < 0x80000) C68kOrbitStore(c68k_orbit_env, adr, data, 2);
}
static inline s32 FASTCALL C68kOrbitInterrupt(s32 level) {
  (void)level; c68k_orbit_env->abort = 5; return C68K_INT_ACK_AUTOVECTOR;
}
/* Architectural CCR, as C68K's own GET_CCR (c68kmac.inc) extracts it: the
 * flag_* fields hold un-normalized results whose other bits are not state. */
static inline u32 C68kOrbitCcr(const c68k_struc *c) {
  return ((c->flag_C >> C68K_SR_C_SFT) & 1) | (((c->flag_V >> C68K_SR_V_SFT) & 1) << 1) |
         ((!c->flag_notZ) << 2) | (((c->flag_N >> C68K_SR_N_SFT) & 1) << 3) |
         (((c->flag_X >> C68K_SR_X_SFT) & 1) << 4);
}
static inline void C68kOrbitArchOf(const c68k_struc *c, C68kOrbitArch *a) {
  memcpy(a->D, c->D, sizeof(a->D)); memcpy(a->A, c->A, sizeof(a->A));
  a->ccr = C68kOrbitCcr(c); a->I = c->flag_I; a->S = c->flag_S; a->USP = c->USP;
}
static inline int C68kOrbitArchIs(const c68k_struc *c, const C68kOrbitArch *a) {
  return !memcmp(a->D, c->D, sizeof(a->D)) && !memcmp(a->A, c->A, sizeof(a->A)) &&
    a->ccr == C68kOrbitCcr(c) && a->I == c->flag_I && a->S == c->flag_S && a->USP == c->USP;
}
static inline int C68kOrbitSameState(const c68k_struc *a, const c68k_struc *b) {
  return a->PC == b->PC && !memcmp(a->D, b->D, sizeof(a->D)) && !memcmp(a->A, b->A, sizeof(a->A)) &&
    C68kOrbitCcr(a) == C68kOrbitCcr(b) && a->flag_I == b->flag_I &&
    a->flag_S == b->flag_S && a->USP == b->USP && a->Status == b->Status && a->IRQLine == b->IRQLine;
}
/* A changed word is an affine counter when exactly one instruction, an
 * ADDQ.W #q,<memory ea> (0101 qqq0 01 mmm rrr, mmm >= 2), wrote it once and
 * every read of it in the pass came from that same instruction, and its add
 * neither carried nor changed the sign. SH-1-independent 68000 semantics:
 * M68000 PRM ADDQ sets X=C=carry, V=overflow, N, Z from the result. */
static inline int C68kOrbitAffine(C68kOrbitProbeEnv *env, unsigned i, u16 before, u16 after) {
  const u32 pc = env->overlay_pc[i], word = env->overlay_addr[i];
  if (env->overlay_writes[i] != 1 || pc >= 0x80000 || env->affine_used == C68K_ORBIT_AFFINE) return 0;
  for (unsigned r = 0; r < env->reads_used; ++r)
    if (env->read_addr[r] == word && env->read_step[r] != env->overlay_step[i]) return 0;
  const u16 op = C68kOrbitRamWord(env->ram, pc);
  if ((op & 0xF1C0) != 0x5040 || ((op >> 3) & 7) < 2) return 0;
  const u16 q = ((op >> 9) & 7) ? (u16)((op >> 9) & 7) : 8;
  if ((u32)before + q != after || ((before ^ after) & 0x8000)) return 0;
  env->affine_addr[env->affine_used] = word; env->affine_q[env->affine_used++] = q;
  return 1;
}
/* Idle, no pending interrupt line, not stopped/halted/faulted/executing. */
static inline int C68kOrbitEligible(const c68k_struc *cpu) {
  return cpu->IRQLine == 0 &&
    !(cpu->Status & (C68K_RUNNING | C68K_HALTED | C68K_WAITING | C68K_DISABLE | C68K_FAULTED));
}
/* Returns the period in cycles, or 0 when no qualifying orbit closes
 * within max_cycles. */
static inline s32 C68kOrbitProbe(const c68k_struc *live, C68kOrbitProbeEnv *env, s32 max_cycles) {
  c68k_struc copy = *live;
  s32 total = 0;
  if (!C68kOrbitEligible(live)) return 0;
  copy.DirectReadRam = NULL; /* every data read goes through the guards */
  copy.Read_Byte = C68kOrbitReadB; copy.Read_Word = C68kOrbitReadW;
  copy.Write_Byte = C68kOrbitWriteB; copy.Write_Word = C68kOrbitWriteW;
  copy.Interrupt_CallBack = C68kOrbitInterrupt;
  env->abort = 0;
  env->overlay_used = 0; env->reads_used = 0; env->affine_used = 0;
  env->writes_used = 0; env->scipd_seen = 0; env->steps = 0; env->period = 0;
  env->copy = &copy;
  c68k_orbit_env = env;
  for (env->step = 0; total < max_cycles; ++env->step) {
    if (env->step == C68K_ORBIT_READS) { env->abort = 7; return 0; }
    env->step_pc[env->step] = (u32)(copy.PC - copy.BasePC);
    C68kOrbitArchOf(&copy, &env->step_state[env->step]);
    const s32 used = C68k_Exec(&copy, 1);
    if (!env->abort && (used <= 0 || used > 1000)) env->abort = 6;
    if (env->abort) return 0;
    total += used;
    if (C68kOrbitSameState(&copy, live)) {
      for (unsigned i = 0; i < env->overlay_used; ++i) {
        const u16 before = C68kOrbitRamWord(env->ram, env->overlay_addr[i]);
        const u16 after = env->overlay_value[i];
        if (after == before) continue;
        if (!C68kOrbitAffine(env, i, before, after)) {
          env->abort = 9; env->abort_addr = env->overlay_pc[i]; return 0;
        }
      }
      env->steps = env->step + 1; env->period = total;
      return total;
    }
  }
  env->abort = 7; /* no return to the start within max_cycles */
  env->abort_addr = (u32)(copy.PC - copy.BasePC);
  return 0;
}

/* Re-entry into a recorded closed pass without a dry run. The pass from
 * step k is a deterministic function of the architectural state at k, the
 * sound RAM it reads and SCIPD. Accept only if the live state equals the
 * recorded state at some step k with the live address, every word the pass
 * reads from RAM without writing it holds its recorded value, every word it
 * writes holds the value the pass leaves in it at k (its last write before k,
 * cyclically), except affine counters (bounded again at materialization),
 * and SCIPD is unchanged. Then running from here replays the recorded pass.
 * Returns the period, or 0. */
static inline s32 C68kOrbitRevalidate(const c68k_struc *live, const C68kOrbitProbeEnv *rec,
                                      const u8 *ram, u16 scipd_now) {
  unsigned k;
  if (!rec->steps || !C68kOrbitEligible(live)) return 0;
  const u32 pc = (u32)(live->PC - live->BasePC);
  for (k = 0; k < rec->steps; ++k)
    if (rec->step_pc[k] == pc && C68kOrbitArchIs(live, &rec->step_state[k])) break;
  if (k == rec->steps) return 0;
  if (rec->scipd_seen && scipd_now != rec->scipd_value) return 0;
  for (unsigned i = 0; i < rec->writes_used; ++i) {
    const u32 w = rec->write_addr[i];
    int affine = 0, last = -1, last_any = -1;
    for (unsigned a = 0; a < rec->affine_used; ++a) affine |= rec->affine_addr[a] == w;
    if (affine) continue;
    for (unsigned j = 0; j < rec->writes_used; ++j) {
      if (rec->write_addr[j] != w) continue;
      last_any = (int)j;
      if (rec->write_step[j] < k) last = (int)j;
    }
    if (C68kOrbitRamWord(ram, w) != rec->write_value[last >= 0 ? last : last_any]) return 0;
  }
  for (unsigned r = 0; r < rec->reads_used; ++r) {
    if (!rec->read_real[r]) continue;
    int written = 0;
    for (unsigned i = 0; i < rec->writes_used && !written; ++i) written = rec->write_addr[i] == rec->read_addr[r];
    if (!written && C68kOrbitRamWord(ram, rec->read_addr[r]) != rec->read_value[r]) return 0;
  }
  return rec->period;
}

static inline void C68kOrbitEnter(C68kIdleOrbit *o, const C68kOrbitProbeEnv *env,
                                  s32 period, s32 saved, s32 cycles) {
  o->active = 1; o->period = period; o->entry_saved = saved; o->deferred = cycles;
  o->affine_used = env->affine_used;
  memcpy(o->affine_addr, env->affine_addr, sizeof(o->affine_addr));
  memcpy(o->affine_q, env->affine_q, sizeof(o->affine_q));
}
/* Executes the deferred budget exactly; returns the new carried overshoot.
 * *skipped receives the number of whole periods not executed. */
static inline s32 C68kOrbitMaterialize(C68kIdleOrbit *o, u8 *ram, s32 (FASTCALL *exec)(s32),
                                       u32 *skipped) {
  const s32 need = o->deferred - o->entry_saved;
  s32 passes, rem;
  o->active = 0;
  if (need <= 0) return -need;
  passes = need / o->period;
  for (unsigned i = 0; i < o->affine_used; ++i) {
    /* Stay inside the probed regime: no carry out of 0xFFFF, no crossing
     * of 0x7FFF/0x8000 (unchanged N, V clear), exactly as probed. */
    const u16 v = C68kOrbitRamWord(ram, o->affine_addr[i]);
    const u32 limit = v < 0x8000 ? 0x7FFF : 0xFFFF;
    const s32 allowed = (s32)((limit - v) / o->affine_q[i]);
    if (allowed < passes) passes = allowed;
  }
  for (unsigned i = 0; i < o->affine_used; ++i) {
    const u16 v = (u16)(C68kOrbitRamWord(ram, o->affine_addr[i]) + passes * o->affine_q[i]);
    memcpy(ram + o->affine_addr[i], &v, 2);
  }
  rem = need - passes * o->period;
  *skipped += (u32)passes;
  return rem ? exec(rem) - rem : 0;
}
#endif
