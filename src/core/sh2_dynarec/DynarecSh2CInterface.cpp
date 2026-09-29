/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "../core.h"
#include <stdio.h>
#include <string.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <stdint.h>
#include "../sh2core.h"
#include "DynarecSh2.h"
#include "../debug.h"
#include "yabause.h"


#define SH2CORE_DYNAMIC             3
#define SH2CORE_DYNAMIC_DEBUG             4

extern "C" {
int SH2DynInit(void);
void SH2DynDeInit(void);
void SH2DynReset(SH2_struct *context);
void SH2DynDebugReset(SH2_struct *context);
void FASTCALL SH2DynExec(SH2_struct *context, u32 cycles);
void SH2DynGetRegisters(SH2_struct *context, sh2regs_struct *regs);
u32 SH2DynGetGPR(SH2_struct *context, int num);
u32 SH2DynGetSR(SH2_struct *context);
u32 SH2DynGetGBR(SH2_struct *context);
u32 SH2DynGetVBR(SH2_struct *context);
u32 SH2DynGetMACH(SH2_struct *context);
u32 SH2DynGetMACL(SH2_struct *context);
u32 SH2DynGetPR(SH2_struct *context);
u32 SH2DynGetPC(SH2_struct *context);
void SH2DynSetRegisters(SH2_struct *context, const sh2regs_struct *regs);
void SH2DynSetGPR(SH2_struct *context, int num, u32 value);
void SH2DynSetSR(SH2_struct *context, u32 value);
void SH2DynSetGBR(SH2_struct *context, u32 value);
void SH2DynSetVBR(SH2_struct *context, u32 value);
void SH2DynSetMACH(SH2_struct *context, u32 value);
void SH2DynSetMACL(SH2_struct *context, u32 value);
void SH2DynSetPR(SH2_struct *context, u32 value);
void SH2DynSetPC(SH2_struct *context, u32 value);
void SH2DynOnFrame(SH2_struct *context);
void SH2DynSendInterrupt(SH2_struct *context, u8 level, u8 vector);
void SH2DynRemoveInterrupt(SH2_struct *context, u8 level, u8 vector);
int SH2DynGetInterrupts(SH2_struct *context, interrupt_struct interrupts[MAX_INTERRUPTS]);
void SH2DynSetInterrupts(SH2_struct *context, int num_interrupts, const interrupt_struct interrupts[MAX_INTERRUPTS]);
void SH2DynWriteNotify(u32 start, u32 length);
void SH2DynAddCycle(SH2_struct *context, u32 value);

SH2Interface_struct SH2Dyn = {
  SH2CORE_DYNAMIC,
  "SH2 Dynamic Recompiler",

  SH2DynInit,
  SH2DynDeInit,
  SH2DynReset,
  SH2DynExec,

  SH2DynGetRegisters,
  SH2DynGetGPR,
  SH2DynGetSR,
  SH2DynGetGBR,
  SH2DynGetVBR,
  SH2DynGetMACH,
  SH2DynGetMACL,
  SH2DynGetPR,
  SH2DynGetPC,

  SH2DynSetRegisters,
  SH2DynSetGPR,
  SH2DynSetSR,
  SH2DynSetGBR,
  SH2DynSetVBR,
  SH2DynSetMACH,
  SH2DynSetMACL,
  SH2DynSetPR,
  SH2DynSetPC,
  SH2DynOnFrame,

  SH2DynSendInterrupt,
  SH2DynRemoveInterrupt,
  SH2DynGetInterrupts,
  SH2DynSetInterrupts,
  SH2DynWriteNotify,
  SH2DynAddCycle
};

SH2Interface_struct SH2DynDebug = {
  SH2CORE_DYNAMIC_DEBUG,
  "SH2 Dynamic Recompiler Debug",

  SH2DynInit,
  SH2DynDeInit,
  SH2DynDebugReset,
  SH2DynExec,

  SH2DynGetRegisters,
  SH2DynGetGPR,
  SH2DynGetSR,
  SH2DynGetGBR,
  SH2DynGetVBR,
  SH2DynGetMACH,
  SH2DynGetMACL,
  SH2DynGetPR,
  SH2DynGetPC,

  SH2DynSetRegisters,
  SH2DynSetGPR,
  SH2DynSetSR,
  SH2DynSetGBR,
  SH2DynSetVBR,
  SH2DynSetMACH,
  SH2DynSetMACL,
  SH2DynSetPR,
  SH2DynSetPC,
  SH2DynOnFrame,

  SH2DynSendInterrupt,
  SH2DynRemoveInterrupt,
  SH2DynGetInterrupts,
  SH2DynSetInterrupts,
  SH2DynWriteNotify,
  SH2DynAddCycle
};

int SH2DynInit(void) {
  try {
    CompileBlocks::getInstance();
  } catch (const std::bad_alloc &) {
    return -1;
  }
  return 0;
}

void SH2DynDeInit(void){
  if (MSH2) {
    delete static_cast<DynarecSh2 *>(MSH2->ext);
    MSH2->ext = nullptr;
  }
  if (SSH2) {
    delete static_cast<DynarecSh2 *>(SSH2->ext);
    SSH2->ext = nullptr;
  }
  DynarecSh2::CurrentContext = nullptr;
}

void SH2DynReset(SH2_struct *context) {

  if (context->ext == NULL) {
    DynarecSh2 * pctx = new DynarecSh2();
    context->ext = (void*)pctx;
    pctx->SetContext(context);
  }

  DynarecSh2 * pctx = (DynarecSh2*)context->ext;
  if (context->isslave) {
    pctx->SetSlave(true);
  }
  else {
    pctx->SetSlave(false);
  }
  CompileBlocks * block = CompileBlocks::getInstance();
  block->Init();
  block->SetDebugMode(false);
  pctx->ResetCPU();
}

void SH2DynDebugReset(SH2_struct *context) {

  if (context->ext == NULL) {
    DynarecSh2 * pctx = new DynarecSh2();
    context->ext = (void*)pctx;
    pctx->SetContext(context);
  }

  DynarecSh2 * pctx = (DynarecSh2*)context->ext;
  if (context->isslave) {
    pctx->SetSlave(true);
  }
  else {
    pctx->SetSlave(false);
  }
  CompileBlocks * block = CompileBlocks::getInstance();
  block->Init();
  block->SetDebugMode(true);
  pctx->ResetCPU();
}

void FASTCALL SH2DynExec(SH2_struct *context, u32 cycles){
  DynarecSh2* pctx = ((DynarecSh2*)context->ext);
  pctx->SetCurrentContext();
  pctx->ExecuteCount(cycles);
}

void SH2DynSendInterrupt(SH2_struct *context, u8 vector, u8 level){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->AddInterrupt(vector, level);
}

void SH2DynRemoveInterrupt(SH2_struct *context, u8 vector, u8 level) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->RemoveInterrupt(vector, level);
}


int SH2DynGetInterrupts(SH2_struct *context, interrupt_struct interrupts[MAX_INTERRUPTS]){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  dlstIntct::iterator itr = pctx->m_IntruptTbl.begin();
  int i = 0;
  while (itr != pctx->m_IntruptTbl.end()) {
    interrupts[i].level = itr->level;
    interrupts[i].vector = itr->Vector;
    itr++;
    i++;
  }

  return 0;
}

void SH2DynSetInterrupts(SH2_struct *context, int num_interrupts, const interrupt_struct interrupts[MAX_INTERRUPTS]){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->m_IntruptTbl.clear();
  for (int i = 0; i < num_interrupts; i++) {
    dIntcTbl tmp;
    tmp.level = interrupts[i].level;
    tmp.Vector = interrupts[i].vector;
    pctx->AddInterrupt(interrupts[i].vector, interrupts[i].level );
  }
  return;
}

void SH2DynGetRegisters(SH2_struct *context, sh2regs_struct *regs) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  memcpy(regs->R, pctx->GetGenRegPtr(), sizeof(regs->R));
  regs->GBR = pctx->GET_GBR();
  regs->VBR = pctx->GET_VBR();
  regs->SR.all = pctx->GET_SR();
  regs->MACH = pctx->GET_MACH();
  regs->MACL = pctx->GET_MACL();
  regs->PC = pctx->GET_PC();
  regs->PR = pctx->GET_PR();
}

u32 SH2DynGetGPR(SH2_struct *context, int num){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GetGenRegPtr()[num];
}

u32 SH2DynGetSR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_SR();
}

u32 SH2DynGetGBR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_GBR();
}

u32 SH2DynGetVBR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_VBR();
}

u32 SH2DynGetMACH(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_MACH();
}

u32 SH2DynGetMACL(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_MACL();
}

u32 SH2DynGetPR(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_PR();
}

u32 SH2DynGetPC(SH2_struct *context){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  return pctx->GET_PC();
}

void SH2DynSetRegisters(SH2_struct *context, const sh2regs_struct *regs){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  memcpy(pctx->GetGenRegPtr(), regs->R , sizeof(regs->R));
  pctx->SET_GBR(regs->GBR);
  pctx->SET_VBR(regs->VBR);
  pctx->SET_SR(regs->SR.all);
  pctx->SET_MACH(regs->MACH);
  pctx->SET_MACL(regs->MACL);
  pctx->SET_PC(regs->PC);
  pctx->SET_PR(regs->PR);
}

void SH2DynSetGPR(SH2_struct *context, int num, u32 value) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->GetGenRegPtr()[num] = value;
}

void SH2DynSetSR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_SR(value);
}

void SH2DynSetGBR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_GBR(value);
}

void SH2DynSetVBR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_VBR(value);
}

void SH2DynSetMACH(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_MACH(value);
}

void SH2DynSetMACL(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_MACL(value);
}
void SH2DynSetPR(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_PR(value);
}
void SH2DynSetPC(SH2_struct *context, u32 value){
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_PC(value);
}

void SH2DynAddCycle(SH2_struct *context, u32 value) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->AddCycle(value);
}


void SH2DynWriteNotify(u32 start, u32 length){
  if (!length) return;
  CompileBlocks * block = CompileBlocks::getInstance();
  
  switch (start & 0x0FF00000){
    // ROM
  case 0x00000000:
      block->LookupTableRom[ (start&0x000FFFFF)>>1 ] = NULL;
    break;

  // Low Memory
  case 0x00200000:
    block->InvalidateLow(start, length);
    break;
    // High Memory
  case 0x06000000:
    // An odd-start write may overlap two SH-2 instruction words. Widen the
    // endpoint before addition so a wrapped address cannot suppress the loop.
    for (u64 addr = start & ~1u; addr < static_cast<u64>(start) + length; addr += 2)
#if defined(SET_DIRTY)
    block->setDirty(static_cast<u32>(addr));
#else
    block->SetHigh((addr&0x000FFFFF)>>1, NULL);
#endif
    break;

    // Cache
  default:
    if ((start & 0xFF000000) == 0xC0000000){
      block->LookupTableC.clear();
    }
    break;
  }
}

void SH2DynShowSttaics(SH2_struct * master, SH2_struct * slave ){
  CompileBlocks * block = CompileBlocks::getInstance();

  DynarecSh2 *pctx = (DynarecSh2*)master->ext;
  pctx->ShowStatics();

  DynarecSh2 *pctxs = (DynarecSh2*)slave->ext;
  pctxs->ShowStatics();

  block->ShowStatics();
}

//********************************************************************
// MemoyAcess from DynarecCPU
//********************************************************************

#ifdef VITA_SH2_IDLE_SLICE_SKIP
extern "C" { extern u32 g_wram_epoch; extern u32 g_mem_io_reads; extern u32 g_mem_io_last; extern u32 g_mem_io_ftcsr; extern u32 g_mem_io_edsr; }
#define VITA_WRAM_WRITTEN() (++g_wram_epoch)
#ifdef A9_PMU_REGIONS
static void A9IoCount(u32 a);
#define VITA_IO_READ(a) (A9IoCount(a), g_mem_io_last = (a), ++g_mem_io_reads)
#else
#define VITA_IO_READ(a) (g_mem_io_last = (a), ++g_mem_io_reads)
#endif
#else
#define VITA_WRAM_WRITTEN() ((void)0)
#define VITA_IO_READ(a) ((void)0)
#endif
#if defined(VITA_STACK_PROFILE) || defined(VITA_SH2_MEM_PROFILE)
extern "C" void YuiMsg(const char *, ...);
// Profile only: SH-2 memory helper calls by 1 MiB region ((addr >> 20) & 0x3FF;
// 0x2xx = cache-through).
static u32 prof_mem[2][1024];
#ifdef VITA_SH2_MEM_PROFILE
static struct { uintptr_t ra; u32 n, size; } prof_site[4096];
static inline void ProfSite(uintptr_t ra, u32 size) {
  unsigned h = unsigned(ra >> 2) * 2654435761u >> 20;
  for (unsigned p = 0; p < 4096; ++p, h = (h + 1) & 4095)
    if (prof_site[h].ra == ra || !prof_site[h].n) { prof_site[h].ra = ra; prof_site[h].size = size; ++prof_site[h].n; return; }
}
#define PROF_SITE(size) ProfSite(uintptr_t(__builtin_return_address(0)), size)
#endif
#define PROF_MEM(w, a) (++prof_mem[w][((a) >> 20) & 0x3FF])
extern "C" void VitaSh2MemReport(void) {
  for (int w = 0; w < 2; ++w) {
    char line[400]; int n = snprintf(line, sizeof(line), "sh2_mem_helper %s", w ? "write" : "read");
    for (int k = 0; k < 8; ++k) {
      u32 best = 0, bi = 0;
      for (u32 i = 0; i < 1024; ++i) if (prof_mem[w][i] > best) { best = prof_mem[w][i]; bi = i; }
      if (!best) break;
      n += snprintf(line + n, sizeof(line) - n, " %03x:%u", bi, best);
      prof_mem[w][bi] = 0;
    }
    YuiMsg("%s", line);
    memset(prof_mem[w], 0, sizeof(prof_mem[w]));
  }
#ifdef VITA_SH2_MEM_PROFILE
  // Top cached-WRAM read call sites, mapped to their compiled guest block.
  CompileBlocks *cb = CompileBlocks::existingInstance();
  for (int k = 0; k < 6 && cb; ++k) {
    unsigned best = 0;
    for (unsigned i = 1; i < 4096; ++i) if (prof_site[i].n > prof_site[best].n) best = i;
    if (!prof_site[best].n) break;
    const uintptr_t ra = prof_site[best].ra;
    const Block *blk = nullptr;
    for (int b = 0; b < NUMOFBLOCKS; ++b) {
      const Block &c = cb->g_CompleBlock[b];
      if (c.code && uintptr_t(c.code) <= ra && (!blk || c.code > blk->code)) blk = &c;
    }
    char line[400]; int n = snprintf(line, sizeof(line), "sh2_mem_site n=%u size=%u host=+%u",
      prof_site[best].n, prof_site[best].size, blk ? unsigned(ra - uintptr_t(blk->code)) : 0u);
    if (blk) {
      n += snprintf(line + n, sizeof(line) - n, " block=%08x..%08x ops", blk->b_addr, blk->e_addr);
      for (u32 a = blk->b_addr; a <= blk->e_addr && a < blk->b_addr + 64 && n < 380; a += 2)
        n += snprintf(line + n, sizeof(line) - n, " %04x", MappedMemoryReadWord(a, NULL));
    }
    YuiMsg("%s", line);
    prof_site[best].n = 0;
  }
  memset(prof_site, 0, sizeof(prof_site));
#endif
}
#else
#define PROF_MEM(w, a) ((void)0)
#endif
#if defined(__arm__) || defined(__aarch64__)
#pragma GCC push_options
#pragma GCC optimize ("O1")
#endif


#ifdef VITA_SH2_WRITE_HELPER_O3
#ifndef SET_DIRTY
#error "Write leaf ownership checks require SET_DIRTY metadata"
#endif
#define WRITE_LINKAGE static __attribute__((noinline))
#define WRITE_NAME(Name) WriteSlow##Name
#else
#define WRITE_LINKAGE
#define WRITE_NAME(Name) memSet##Name
#endif
#ifdef VITA_SH2_SAME_VALUE_STORES
// Compiled code depends only on the words it was compiled from, so a store
// leaving a halfword's value unchanged cannot make any block stale.
#define SAME_HIGH(Width, addr, value) (T2Read##Width(HighWram, (addr) & 0xFFFFF) == (value))
#else
#define SAME_HIGH(Width, addr, value) 0
#endif
static __attribute__((always_inline)) inline void InvalidateHighWrite(CompileBlocks *block, u32 addr) {
#ifdef VITA_SH2_WRITE_PREFLIGHT
  // Same live owner test as setDirty, before its large cold-path stack frame.
  // Never cache this result: compilation/invalidation can change ownership.
  if (block->LookupParentTable[adress_mask(addr)].size() == 0) return;
#endif
  block->setDirty(addr);
}

WRITE_LINKAGE void WRITE_NAME(Byte)(u32 addr , u8 data )
{
  PROF_MEM(1, addr);
  dynaLock();
  u32 cycle = 0;
  //LOG("memSetWord %08X, %08X\n", addr, data);
  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    block->InvalidateLow(addr, 1);
    T2WriteByte(LowWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
  case 0x06000000:
#if defined(SET_DIRTY)
    if (!SAME_HIGH(Byte, addr, data)) InvalidateHighWrite(block, addr);
#else
    block->SetHigh((addr&0x000FFFFF)>>1, NULL);
#endif
    T2WriteByte(HighWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
    dynaFree();
    return;
    break;

  // Cache
  default:
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
    // Data array (cache used as RAM): no memory cycles, as in MappedMemoryWrite.
    if ((addr >> 29) == 6) {
      if ((addr & 0xFF000000) == 0xC0000000 && !block->LookupTableC.empty())
        block->LookupTableC.clear();
      T2WriteByte(CurrentSH2->DataArray, addr & 0xFFF, data);
      dynaFree();
      return;
    }
#endif
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      block->LookupTableC.clear();
    }
  }
  MappedMemoryWriteByte(addr, data, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
}

WRITE_LINKAGE void WRITE_NAME(Word)(u32 addr, u16 data )
{
  PROF_MEM(1, addr);
  dynaLock();
  u32 cycle = 0;
  //LOG("memSetWord %08X, %08X\n", addr, data);

  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {
  // Low Memory
   case 0x00200000:
    block->InvalidateLow(addr, 2);
    T2WriteWord(LowWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
   case 0x06000000:  {
#if defined(SET_DIRTY)
     if (!SAME_HIGH(Word, addr, data)) InvalidateHighWrite(block, addr);
#else
     block->SetHigh((addr & 0x000FFFFF) >> 1, NULL);
#endif
    T2WriteWord(HighWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
    dynaFree();
    return;
   }
    break;
  // Cache
  default:
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
    // Data array (cache used as RAM): no memory cycles, as in MappedMemoryWrite.
    if ((addr >> 29) == 6) {
      if ((addr & 0xFF000000) == 0xC0000000 && !block->LookupTableC.empty())
        block->LookupTableC.clear();
      T2WriteWord(CurrentSH2->DataArray, addr & 0xFFF, data);
      dynaFree();
      return;
    }
#endif
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      block->LookupTableC.clear();
    }
  }
  MappedMemoryWriteWord(addr, data, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
}

WRITE_LINKAGE void WRITE_NAME(Long)(u32 addr , u32 data )
{
  PROF_MEM(1, addr);
  dynaLock();
  //LOG("memSetLong %08X, %08X\n", addr, data);
  u32 cycle = 0;

  CompileBlocks * block = CompileBlocks::getInstance();
  switch (addr & 0xDFF00000)
  {  
    // Low Memory
  case 0x00200000:
    block->InvalidateLow(addr, 4);
    T2WriteLong(LowWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if(addr&0x20000000) DynarecSh2::CurrentContext->memcycle_ += 7;
    dynaFree();
    return;
    break;
  // High Memory
  case 0x06000000:
#if defined(SET_DIRTY)
    if (!SAME_HIGH(Word, addr, (u16)(data >> 16))) InvalidateHighWrite(block, addr);
    if (!SAME_HIGH(Word, addr + 2, (u16)data)) InvalidateHighWrite(block, addr+2);
#else
    block->SetHigh((addr & 0x000FFFFF) >> 1, NULL);
    block->SetHigh(((addr & 0x000FFFFF) >> 1) + 1, NULL);
#endif
    T2WriteLong(HighWram, addr & 0xFFFFF, data); VITA_WRAM_WRITTEN();
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
    dynaFree();
    return;
    break;

  // Cache
  default:
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
    // Data array (cache used as RAM): no memory cycles, as in MappedMemoryWrite.
    if ((addr >> 29) == 6) {
      if ((addr & 0xFF000000) == 0xC0000000 && !block->LookupTableC.empty())
        block->LookupTableC.clear();
      T2WriteLong(CurrentSH2->DataArray, addr & 0xFFF, data);
      dynaFree();
      return;
    }
#endif
    if ((addr & 0xFF000000) == 0xC0000000)
    {
      block->LookupTableC.clear();
    }
  }
  MappedMemoryWriteLong(addr, data, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
}

#ifdef VITA_SH2_WRITE_HELPER_O3
#pragma GCC push_options
#pragma GCC optimize ("O3")
// Only the already-existing high-RAM/no-code-owner case is a leaf. All code
// invalidation, initialization, low RAM and mapped devices use the old helper.
#ifdef VITA_SH2_ONCHIP_DIRECT
// On-chip modules: MappedMemoryWrite##Name's handler, with its 0 memory cycles.
#define ONCHIP_WRITE(Name, addr, data) \
    if ((addr) >= 0xFFFFFE00u) { PROF_MEM(1, addr); OnchipWrite##Name((addr) & 0x1FF, data); return; }
#else
#define ONCHIP_WRITE(Name, addr, data)
#endif
#define WRITE_LEAF(Name, Type, Width) \
  void memSet##Name(u32 addr, Type data) { \
    CompileBlocks *block=CompileBlocks::existingInstance(); \
    if ((addr & 0xdff00000u)==0x06000000u && block && \
        block->LookupParentTable[adress_mask(addr)].size()==0 && \
        ((Width)!=4 || block->LookupParentTable[adress_mask(addr+2)].size()==0)) { \
      T2Write##Name(HighWram,addr&0xfffff,data); VITA_WRAM_WRITTEN(); \
      if(addr&0x20000000u) DynarecSh2::CurrentContext->memcycle_+=2; \
      return; \
    } \
    ONCHIP_WRITE(Name, addr, data) \
    WriteSlow##Name(addr,data); \
  }
WRITE_LEAF(Byte,u8,1)
WRITE_LEAF(Word,u16,2)
WRITE_LEAF(Long,u32,4)
#undef WRITE_LEAF
#undef ONCHIP_WRITE
#pragma GCC pop_options
#endif
#undef WRITE_LINKAGE
#undef WRITE_NAME
#ifdef VITA_SH2_READ_HELPER_O3
#pragma GCC push_options
#pragma GCC optimize ("O3")
/* Keep address-taken cycle scratch and the mapped callback's LR save out of
 * direct WRAM reads. The fallback retains the same handler and cycle update. */
#define MAPPED_READ_HELPER(Name, Type) \
  static __attribute__((noinline)) Type ReadMapped##Name(u32 addr) { \
    u32 cycle=0; \
    Type value=MappedMemoryRead##Name(addr,&cycle); \
    DynarecSh2::CurrentContext->memcycle_+=cycle; \
    return value; \
  }
MAPPED_READ_HELPER(Byte,u8)
MAPPED_READ_HELPER(Word,u16)
MAPPED_READ_HELPER(Long,u32)
#undef MAPPED_READ_HELPER
#endif
u8 memGetByte(u32 addr)
{
  PROF_MEM(0, addr);
  dynaLock();
  u8 val;
  u32 cycle = 0;
  
  switch (addr & 0xDFF00000)
  {
    // Low Memory
  case 0x00200000:
    val = T2ReadByte(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadByte(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
#ifdef VITA_SH2_MEM_PROFILE
    if (!(addr & 0x20000000)) PROF_SITE(1);
#endif
    dynaFree();
    return val;
    break;
  }
  VITA_IO_READ(addr);
#ifdef VITA_SH2_IDLE_SLICE_SKIP
  if (addr == 0xFFFFFE11u) ++g_mem_io_ftcsr;   // FTCSR: pure on-chip read (spin forward)
#endif
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
  if ((addr >> 29) == 6) return T2ReadByte(CurrentSH2->DataArray, addr & 0xFFF);
#endif
#ifdef VITA_SH2_ONCHIP_DIRECT
  // On-chip modules: MappedMemoryReadByte's handler, with its 0 memory cycles.
  if (addr >= 0xFFFFFE00u) return OnchipReadByte(addr & 0x1FF);
#endif
#ifdef VITA_SH2_READ_HELPER_O3
  return ReadMappedByte(addr);
#else
  val = MappedMemoryReadByte(addr, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
  return val;
#endif
}
 
#ifdef VITA_FB_DIRECT_READ
extern "C" int VitaFbReadWord(u32 addr, u16 *out);
#endif
u16 memGetWord(u32 addr)
{
  PROF_MEM(0, addr);
  dynaLock();
  u16 val;
  u32 cycle = 0;

  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    val = T2ReadWord(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadWord(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
#ifdef VITA_SH2_MEM_PROFILE
    if (!(addr & 0x20000000)) PROF_SITE(2);
#endif
    dynaFree();
    return val;
    break;
  }
  VITA_IO_READ(addr);
#ifdef VITA_SH2_IDLE_EDSR
  if ((addr & 0xDFFFFFFFu) == 0x05D00010u) ++g_mem_io_edsr;  // VDP1 EDSR word: pure register read (idle skip)
#endif
#ifdef VITA_FB_DIRECT_READ
  // VDP1 framebuffer (Vdp1FrameBufferReadWord), with its 50 read cycles.
  if ((addr & 0xDFF80000) == 0x05C80000) {
    u16 fb;
    if (VitaFbReadWord(addr, &fb)) {
      DynarecSh2::CurrentContext->memcycle_ += 50;
      dynaFree();
      return fb;
    }
  }
#endif
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
  if ((addr >> 29) == 6) return T2ReadWord(CurrentSH2->DataArray, addr & 0xFFF);
#endif
#ifdef VITA_SH2_ONCHIP_DIRECT
  // On-chip modules: MappedMemoryReadWord's handler, with its 0 memory cycles.
  if (addr >= 0xFFFFFE00u) return OnchipReadWord(addr & 0x1FF);
#endif
#ifdef VITA_SH2_READ_HELPER_O3
  return ReadMappedWord(addr);
#else
  val = MappedMemoryReadWord(addr, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
  return val;
#endif
}

u32 memGetLong(u32 addr)
{
  PROF_MEM(0, addr);
  dynaLock();
  u32 val;
  u32 cycle = 0;
  switch (addr & 0xDFF00000)
  {
  // Low Memory
  case 0x00200000:
    val = T2ReadLong(LowWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 4;
    dynaFree();
    return val;
    break;
    // High Memory
  case 0x06000000:
    val = T2ReadLong(HighWram, addr & 0xFFFFF);
    if (addr & 0x20000000) DynarecSh2::CurrentContext->memcycle_ += 2;
#ifdef VITA_SH2_MEM_PROFILE
    if (!(addr & 0x20000000)) PROF_SITE(4);
#endif
    dynaFree();
    return val;
    break;
  }
  VITA_IO_READ(addr);
#ifdef VITA_SH2_DATA_ARRAY_DIRECT
  if ((addr >> 29) == 6) return T2ReadLong(CurrentSH2->DataArray, addr & 0xFFF);
#endif
#ifdef VITA_SH2_ONCHIP_DIRECT
  // On-chip modules: MappedMemoryReadLong's handler, with its 0 memory cycles.
  if (addr >= 0xFFFFFE00u) return OnchipReadLong(addr & 0x1FF);
#endif
#ifdef VITA_SH2_READ_HELPER_O3
  return ReadMappedLong(addr);
#else
  val = MappedMemoryReadLong(addr, &cycle);
  DynarecSh2::CurrentContext->memcycle_ += cycle;
  dynaFree();
  return val;
#endif
}

#ifdef VITA_SH2_READ_HELPER_O3
#pragma GCC pop_options
#endif
#if defined(__arm__) || defined(__aarch64__)
#pragma GCC pop_options
#endif


void SH2DynOnFrame(SH2_struct *context) {
  DynarecSh2 *pctx = (DynarecSh2*)context->ext;
  pctx->SET_COUNT(0);
  pctx->onFrame();
}


//************************************************
// Callbacks from DynarecCPU
//************************************************
extern "C" {
  void SH2HandleBreakpoints(SH2_struct *context);
}

void DynaCheckBreakPoint(u32 pc) {
  CurrentSH2->regs.PC = pc;
  SH2HandleBreakpoints(CurrentSH2);
}


int DelayEachClock() {
  return 0;
}

int DebugDelayClock() {
  dynaLock();
  DynaCheckBreakPoint(DynarecSh2::CurrentContext->GET_PC());
  dynaFree();
  return 0;
}

int DebugEachClock() {
  dynaLock();

  #define INSTRUCTION_B(x) ((x & 0x0F00) >> 8)
  #define INSTRUCTION_C(x) ((x & 0x00F0) >> 4)

  //printf("PC:%08X\n",DynarecSh2::CurrentContext->GET_PC());

#if 0
  u32 pc = DynarecSh2::CurrentContext->GET_PC();
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);

  LOG("%08X: rotcrout R%d=%08X, SR=%08X\n", 
    DynarecSh2::CurrentContext->GET_PC(), 
    n,DynarecSh2::CurrentContext->GetGenRegPtr()[n], 
    DynarecSh2::CurrentContext->GET_SR());
#endif

#if 0  
  LOG("%08X: subc R%d=%08X R%d=%08X SR=%08X\n", 
    DynarecSh2::CurrentContext->GET_PC(), 
    m,DynarecSh2::CurrentContext->GetGenRegPtr()[m], 
    n,DynarecSh2::CurrentContext->GetGenRegPtr()[n], 
    DynarecSh2::CurrentContext->GET_SR());
#endif

#if 0
if( DynarecSh2::CurrentContext->GET_PC() >= 0x0602E3C2 &&  DynarecSh2::CurrentContext->GET_PC() < 0x0602E468 ) {
   u32 addrn = DynarecSh2::CurrentContext->GetGenRegPtr()[6]-4;
   u32 addrm = DynarecSh2::CurrentContext->GetGenRegPtr()[7]-4;
   printf("%08X: MACL R[%d]=%08X@%08X,R[%d]=%08X@%08X,MACH=%08X,MACL=%08X\n",
      DynarecSh2::CurrentContext->GET_PC(),
      6,addrn,MappedMemoryReadLong(addrn),
      7,addrm,MappedMemoryReadLong(addrm),
      DynarecSh2::CurrentContext->GET_MACH(),
      DynarecSh2::CurrentContext->GET_MACL()
   );
}
#endif

#if 0
  #define INSTRUCTION_B(x) ((x & 0x0F00) >> 8)
  #define INSTRUCTION_C(x) ((x & 0x00F0) >> 4)

  u32 pc = DynarecSh2::CurrentContext->GET_PC();
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);
  printf("%08X: DIV0S %04X R[%d]:%08X,R[%d]:%08X,SR:%08X\n", 
    DynarecSh2::CurrentContext->GET_PC(), 
    inst,
    m,DynarecSh2::CurrentContext->GetGenRegPtr()[m], 
    n,DynarecSh2::CurrentContext->GetGenRegPtr()[n], 
    DynarecSh2::CurrentContext->GET_SR());
#endif

#if 0
u32 pc = DynarecSh2::CurrentContext->GET_PC();
if( pc == 0x060133C8 ) {
  u16 inst = memGetWord(pc);
  s32 m = INSTRUCTION_C(inst);
  s32 n = INSTRUCTION_B(inst);
  printf("%08X: DIV1(O) m:%08X,n:%08X,SR:%08X\n", 
    DynarecSh2::CurrentContext->GET_PC(), 
    DynarecSh2::CurrentContext->GetGenRegPtr()[m], 
    DynarecSh2::CurrentContext->GetGenRegPtr()[n], 
    DynarecSh2::CurrentContext->GET_SR());
}
#endif


#ifdef DMPHISTORY
  CurrentSH2->pchistory_index++;
  CurrentSH2->pchistory[CurrentSH2->pchistory_index & (MAX_DMPHISTORY-1) ] = DynarecSh2::CurrentContext->GET_PC();
  //CurrentSH2->regshistory[CurrentSH2->pchistory_index & 0xFF] = NULL;
#endif
  DynaCheckBreakPoint(DynarecSh2::CurrentContext->GET_PC());

  if (DynarecSh2::CurrentContext->CheckOneStep()){
    dynaFree();
    return 1;
  }

  dynaFree();
  return 0;
}
  
int EachClock() {
  dynaLock();
  if (DynarecSh2::CurrentContext->CheckInterupt()) {
      dynaFree();
      return 1;
  }
  dynaFree();
  return 0;
}


}
#ifdef A9_PMU_REGIONS
extern "C" void YuiMsg(const char *, ...);
/* Linux profiling build: every live high work-RAM block's native size and the L1
 * set of its first line (32-byte lines, 256 sets). */
static struct { u32 addr, n[2]; } a9_io[256];
static void A9IoCount(u32 a) {
  const int c = DynarecSh2::CurrentContext && DynarecSh2::CurrentContext->IsSlave() ? 1 : 0;
  for (unsigned h = (a * 2654435761u) >> 24, p = 0; p < 256; ++p, h = (h + 1) & 255)
    if (a9_io[h].addr == a || !(a9_io[h].n[0] | a9_io[h].n[1])) { a9_io[h].addr = a; ++a9_io[h].n[c]; return; }
}
extern "C" void A9BlockDump(void) {
  for (auto &e : a9_io) if (e.n[0] + e.n[1] > 1000) YuiMsg("io_read addr=%08x master=%u slave=%u", e.addr, e.n[0], e.n[1]);
  CompileBlocks *cb = CompileBlocks::existingInstance();
  if (!cb) return;
  for (u32 i = 0; i < 0x100000 >> 1; ++i) {
    const Block *b = cb->LookupTable[i];
    if (!b || !b->code || b->b_addr != (0x06000000u | (i << 1))) continue;
    const u32 *w = reinterpret_cast<const u32 *>(b->code);
    u32 n = MAXBLOCKSIZE / 4;
    if (b->id < cb->code_bytes_.size() && cb->code_bytes_[b->id]) n = cb->code_bytes_[b->id] / 4;
    else while (n && !w[n - 1]) --n;
    YuiMsg("blk b=%08x e=%08x set=%u size=%u code=%08x", b->b_addr, b->e_addr, unsigned((uintptr_t(b->code) >> 5) & 255), n * 4, unsigned(uintptr_t(b->code)));
    char name[64]; snprintf(name, sizeof(name), "blk_%08x.bin", b->b_addr);
    if (FILE *f = fopen(name, "wb")) { fwrite(w, 4, n, f); fclose(f); }
    snprintf(name, sizeof(name), "blk_%08x.sh2", b->b_addr);
    if (FILE *f = fopen(name, "wb")) {
      for (u32 a = b->b_addr; a <= b->e_addr + 2; a += 2) { u16 op = MappedMemoryReadWord(a, NULL); op = u16(op >> 8 | op << 8); fwrite(&op, 2, 1, f); }
      fclose(f);
    }
  }
}
#endif
