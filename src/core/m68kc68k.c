/*  Copyright 2007 Guillaume Duhamel

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file m68kc68k.c
    \brief C68K 68000 interface.
*/

#include "m68kc68k.h"
#include "c68k/c68k.h"
#include "c68k/native_guard.h"
#include "c68k/native_source.h"
#include "memory.h"
#include "yabause.h"
#ifdef VITA_M68K_IDLE_ORBIT
#include "scsp.h" /* SoundRam */
#endif

/**
 * PROFILE_68K: Perform simple profiling of the 68000 emulation, reporting
 * the average time per 68000 clock cycle.  (Realtime execution would be
 * around 88.5 nsec/cycle.)
 */
// #define PROFILE_68K


static u8 *SoundDummy=NULL;

static int M68KC68KInit(void) {
	int i;
	C68kNativeSourceBind(NULL);

	// Setup a 64k buffer filled with invalid 68k instructions to serve
	// as a default map
	if ((SoundDummy = T2MemoryInit(0x10000)) != NULL)
		memset(SoundDummy, 0xFF, 0x10000);

	C68k_Init(&C68K, NULL); // not sure if I need the int callback or not

	for (i = 0x10; i < 0x100; i++)
		M68K->SetFetch(i << 16, (i << 16) + 0xFFFF, (pointer)SoundDummy);

	return 0;
}

static void M68KC68KDeInit(void) {
	C68kNativeSourceBind(NULL);
	if (SoundDummy)
		T2MemoryDeInit(SoundDummy);
	SoundDummy = NULL;
}

static void M68KC68KReset(void) {
	C68k_Reset(&C68K);
}

#include "../vita/telemetry.h"
static s32 FASTCALL M68KC68KExec(s32 cycle) {
   VT_SCOPE(VT_M68K);
#ifdef PROFILE_68K
    static u32 tot_cycles = 0, tot_usec = 0, tot_ticks = 0, last_report = 0;
    u32 start, end;
    start = (u32) YabauseGetTicks();
    int retval = C68k_Exec(&C68K, cycle);
    end = (u32) YabauseGetTicks();
    tot_cycles += cycle;
    tot_ticks += end - start;
    if (tot_cycles/1000000 > last_report) {
        tot_usec += (u64)tot_ticks * 1000000 / yabsys.tickfreq;
        tot_ticks = 0;
        fprintf(stderr, "%ld cycles in %.3f sec = %.3f nsec/cycle\n",
                (long)tot_cycles, (double)tot_usec/1000000,
                ((double)tot_usec / (double)tot_cycles) * 1000);
        last_report = tot_cycles/1000000;
    }
    return retval;
#else
	return C68k_Exec(&C68K, cycle);
#endif
}

#ifdef VITA_STACK_PROFILE
u32 M68K_GetPC_Diag(void) { return C68k_Get_PC(&C68K); }
#endif

#ifdef VITA_M68K_IDLE_ORBIT
#include "c68k/idle_orbit.h"
/* See c68k/idle_orbit.h and scsp.c: probes a copy of the live CPU. */
int M68KC68KOrbitEligible(void) { return C68kOrbitEligible(&C68K); }
u32 g_orbit_abort[16], g_orbit_abort_addr[16];
static C68kOrbitProbeEnv envs[2];
static int rec;                  /* envs[rec]: last closed pass; probes use the other */
#define env envs[rec]
/* Probe policy data only (never affects exactness): instruction addresses of
 * passes that closed before, as candidate starting points. */
static u32 orbit_pcs[1024]; /* open addressing, value pc + 1, 0 empty */
static s32 orbit_pc_period[1024]; /* longest closed period through that address */
static int OrbitPcSlot(u32 pc) {
  u32 h = (pc * 2654435761u) >> 22;
  for (unsigned n = 0; n < 1024; ++n, h = (h + 1) & 1023)
    if (!orbit_pcs[h] || orbit_pcs[h] == pc + 1) return (int)h;
  return -1;
}
/* Period of a known orbit through the current address, or 0. */
s32 M68KC68KOrbitCandidate(void) {
  const int h = OrbitPcSlot(C68k_Get_PC(&C68K));
  return h >= 0 && orbit_pcs[h] ? orbit_pc_period[h] : 0;
}
void M68KC68KOrbitEnter(C68kIdleOrbit *o, s32 period, s32 saved, s32 cycles) {
  C68kOrbitEnter(o, &env, period, saved, cycles);
  for (unsigned i = 0; i < env.step; ++i) {
    const int h = OrbitPcSlot(env.step_pc[i]);
    if (h < 0) continue;
    orbit_pcs[h] = env.step_pc[i] + 1;
    if (orbit_pc_period[h] < period) orbit_pc_period[h] = period;
  }
}
s32 M68KC68KOrbitRevalidate(void) {
  return C68kOrbitRevalidate(&C68K, &env, SoundRam, (u16)C68K.Read_Word(0x100420));
}
s32 M68KC68KOrbitProbe(s32 max_cycles) {
  C68kOrbitProbeEnv *scratch = &envs[1 - rec];
  scratch->ram = SoundRam; scratch->read_b = C68K.Read_Byte; scratch->read_w = C68K.Read_Word;
  const s32 period = C68kOrbitProbe(&C68K, scratch, max_cycles);
  ++g_orbit_abort[scratch->abort & 15]; g_orbit_abort_addr[scratch->abort & 15] = scratch->abort_addr;
  if (period > 0) rec = 1 - rec;
  return period;
}
#endif
static void M68KC68KSync(void) {
}

static u32 M68KC68KGetDReg(u32 num) {
	return C68k_Get_DReg(&C68K, num);
}

static u32 M68KC68KGetAReg(u32 num) {
	return C68k_Get_AReg(&C68K, num);
}

static u32 M68KC68KGetPC(void) {
	return C68k_Get_PC(&C68K);
}

static u32 M68KC68KGetSR(void) {
	return C68k_Get_SR(&C68K);
}

static u32 M68KC68KGetUSP(void) {
	return C68k_Get_USP(&C68K);
}

static u32 M68KC68KGetMSP(void) {
	return C68k_Get_MSP(&C68K);
}

static void M68KC68KSetDReg(u32 num, u32 val) {
	C68k_Set_DReg(&C68K, num, val);
}

static void M68KC68KSetAReg(u32 num, u32 val) {
	C68k_Set_AReg(&C68K, num, val);
}

static void M68KC68KSetPC(u32 val) {
	C68k_Set_PC(&C68K, val);
}

static void M68KC68KSetSR(u32 val) {
	C68k_Set_SR(&C68K, val);
}

static void M68KC68KSetUSP(u32 val) {
	C68k_Set_USP(&C68K, val);
}

static void M68KC68KSetMSP(u32 val) {
	C68k_Set_MSP(&C68K, val);
}

static void M68KC68KSetFetch(u32 low_adr, u32 high_adr, pointer fetch_adr) {
	C68k_Set_Fetch(&C68K, low_adr, high_adr, fetch_adr);
}

static void FASTCALL M68KC68KSetIRQ(s32 level) {
	C68k_Set_IRQ(&C68K, level);
}

static void FASTCALL M68KC68KWriteNotify(u32 address, u32 size) {
	C68kNativeSourceChanged(address, size);
}

static void M68KC68KSetReadB(M68K_READ *Func) {
	C68k_Set_ReadB(&C68K, Func);
}

static void M68KC68KSetReadW(M68K_READ *Func) {
	C68k_Set_ReadW(&C68K, Func);
}

static void M68KC68KSetWriteB(M68K_WRITE *Func) {
	C68k_Set_WriteB(&C68K, Func);
}

static void M68KC68KSetWriteW(M68K_WRITE *Func) {
	C68k_Set_WriteW(&C68K, Func);
}

static void C68k_Save_State(c68k_struc *mcpu, FILE * fp)
{
   C68K_NATIVE_GUARD;
   IOCheck_struct check = { 0, 0 };
   int i = 0;
   u32 pc = 0;

   for (i = 0; i < 8; i++)
      ywrite(&check, (void *)&mcpu->D[i], sizeof(u32), 1, fp);

   for (i = 0; i < 8; i++)
      ywrite(&check, (void *)&mcpu->A[i], sizeof(u32), 1, fp);

   ywrite(&check, (void *)&mcpu->flag_C, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->flag_V, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->flag_notZ, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->flag_N, sizeof(u32), 1, fp);

   ywrite(&check, (void *)&mcpu->flag_X, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->flag_I, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->flag_S, sizeof(u32), 1, fp);

   ywrite(&check, (void *)&mcpu->USP, sizeof(u32), 1, fp);

   pc = C68k_Get_PC(&C68K);

   ywrite(&check, (void *)&pc, sizeof(u32), 1, fp);

   ywrite(&check, (void *)&mcpu->Status, sizeof(u32), 1, fp);
   ywrite(&check, (void *)&mcpu->IRQLine, sizeof(s32), 1, fp);

   ywrite(&check, (void *)&mcpu->CycleToDo, sizeof(s32), 1, fp);
   ywrite(&check, (void *)&mcpu->CycleIO, sizeof(s32), 1, fp);
   ywrite(&check, (void *)&mcpu->CycleSup, sizeof(s32), 1, fp);
   ywrite(&check, (void *)&mcpu->dirty1, sizeof(u32), 1, fp);
}

static void M68KC68KSaveState(FILE *fp) {
   C68k_Save_State(&C68K, fp);
}

static void C68k_Load_State(c68k_struc *mcpu, FILE * fp)
{
   C68K_NATIVE_GUARD;
   IOCheck_struct check = { 0, 0 };
   int i = 0;
   u32 pc = 0;

   for (i = 0; i < 8; i++)
      yread(&check, (void *)&mcpu->D[i], sizeof(u32), 1, fp);

   for (i = 0; i < 8; i++)
      yread(&check, (void *)&mcpu->A[i], sizeof(u32), 1, fp);

   yread(&check, (void *)&mcpu->flag_C, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->flag_V, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->flag_notZ, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->flag_N, sizeof(u32), 1, fp);

   yread(&check, (void *)&mcpu->flag_X, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->flag_I, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->flag_S, sizeof(u32), 1, fp);

   yread(&check, (void *)&mcpu->USP, sizeof(u32), 1, fp);

   yread(&check, (void *)&pc, sizeof(u32), 1, fp);

   C68k_Set_PC(&C68K, pc);

   yread(&check, (void *)&mcpu->Status, sizeof(u32), 1, fp);
   yread(&check, (void *)&mcpu->IRQLine, sizeof(s32), 1, fp);

   yread(&check, (void *)&mcpu->CycleToDo, sizeof(s32), 1, fp);
   yread(&check, (void *)&mcpu->CycleIO, sizeof(s32), 1, fp);
   yread(&check, (void *)&mcpu->CycleSup, sizeof(s32), 1, fp);
   yread(&check, (void *)&mcpu->dirty1, sizeof(u32), 1, fp);
}

static void M68KC68KLoadState(FILE *fp) {
   C68k_Load_State(&C68K, fp);
}

M68K_struct M68KC68K = {
	1,
	"C68k Interface",
	M68KC68KInit,
	M68KC68KDeInit,
	M68KC68KReset,
	M68KC68KExec,
	M68KC68KSync,
	M68KC68KGetDReg,
	M68KC68KGetAReg,
	M68KC68KGetPC,
	M68KC68KGetSR,
	M68KC68KGetUSP,
	M68KC68KGetMSP,
	M68KC68KSetDReg,
	M68KC68KSetAReg,
	M68KC68KSetPC,
	M68KC68KSetSR,
	M68KC68KSetUSP,
	M68KC68KSetMSP,
	M68KC68KSetFetch,
	M68KC68KSetIRQ,
	M68KC68KWriteNotify,
	M68KC68KSetReadB,
	M68KC68KSetReadW,
	M68KC68KSetWriteB,
	M68KC68KSetWriteW,
   M68KC68KSaveState,
   M68KC68KLoadState
};
