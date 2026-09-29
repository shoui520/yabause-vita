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


#include <stdio.h>
#include <string.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <stdint.h>
#include <core.h>
#include <unordered_map>

#include "sh2core.h"
#include "debug.h"
#include "yabause.h"
#include "bios.h"
extern "C" {
#include "scu.h"
}

#include "DynarecSh2.h"
#include "a9_register_region.h"
#include "a9_ram_load.h"
#include "cached_dispatch.h"
static_assert(offsetof(tagSH2, GenReg) == 0 && offsetof(tagSH2, CtrlReg) == 64 && offsetof(tagSH2, SysReg) == 76,
              "IdleRegs order = first 23 words of tagSH2");
static_assert(offsetof(tagSH2, exitcount) == 128 && offsetof(tagSH2, spec_bail) == 132 &&
              offsetof(tagSH2, spec_high) == 136 && offsetof(tagSH2, spec_pages) == 140 &&
              offsetof(tagSH2, dispatch) == 144 && offsetof(tagSH2, chain_budget) == 148 &&
              offsetof(tagSH2, chain_table) == 152 && offsetof(tagSH2, chain_cur) == 156 &&
              offsetof(tagSH2, memcycle) == 160 && offsetof(tagSH2, macw_saturate) == 164 &&
              offsetof(tagSH2, macl_saturate) == 168,
              "offsets used by generated code and dynalib_arm.s");
static_assert(offsetof(Block, code) == 0 && offsetof(Block, b_addr) == 4 && offsetof(Block, flags) == 16 &&
              offsetof(Block, link_pc) == 28 && BLOCK_LOOP == 1, "Block layout used by sh2_dispatch and link stubs");
#ifdef VITA_SH2_NATIVE_CHAIN
#include "chain_runner.h"
static_assert(offsetof(Block, code) == 0 && offsetof(Block, flags) == 16,
              "Native runner Block layout");
#endif
#include "memory.h"
#include "scsp.h"
#include "opcodes.h"
#include "../../vita/sh2_code_memory.h"
#include "../../vita/telemetry.h"
#include "native_hot_samples.h"
#include "poll_loop.h"
#include "poll_step.h"
#if defined(WEBINTERFACE)
#define DEBUG_CPU
#endif
//#define DEBUG_CPU
//#define EXECUTE_STAT
//#define BUILD_INFO
//#define LOG printf

CompileBlocks * CompileBlocks::instance_ = NULL;
DynarecSh2 * DynarecSh2::CurrentContext = NULL;
// Single emulation-thread counters: count actual native returns, not selection
// of this backend. Time only one in 1024 calls to limit instrumentation cost.
#ifdef VITA_SH2_LEAN_STATS
// Reports reset both every window (~1.2M returns, ~15M cycles), so 32 bits
// are exact. native_cycles is accumulated once per ExecuteCount slice.
static u32 native_returns, native_cycles;
static u64 native_sample_us, native_samples;
#else
static u64 native_returns, native_cycles, native_sample_us, native_samples;
#endif
// The internal totals retain a common sampling cadence. Reports subtract the
// semantic steps so they are never misrepresented as generated-code execution.
static u64 poll_step_calls, poll_step_cycles, poll_step_samples, poll_step_us;
static u32 poll_fused_blocks, poll_fused_saved_bytes;
static u64 chain_batches, chain_blocks;
static u64 resident_calls, resident_iterations, resident_cycles;
static u64 poll_calls, poll_iterations, poll_cycles;
static u32 chain_stops[7];
static sh2a9::NativeHotSamples hot_samples;
#ifdef VITA_STACK_PROFILE
// Profile only: guest instructions of sampled blocks outside region admission,
// by opcode_list index (weighted by block entries, not time).
static u64 region_ops, outside_ops, outside_hist[256];
static u64 mem_ops_total, mem_ops_r15; // sampled guest memory accesses; base R15
#endif
#ifdef VITA_STACK_PROFILE
// Profile only: compiles by PC range, and blocks dropped by high-RAM writes.
u32 g_prof_compiles[4], g_prof_self_modify_reuse;
u32 g_prof_smc_pc[64], g_prof_smc_n[64], g_prof_smc_hit, g_prof_smc_miss;
// Per CPU (0 master, 1 slave): slices, blocks, slices whose first block
// idle-skipped, and blocks before an idle skip.
u32 g_prof_slices[2], g_prof_blocks[2], g_prof_idle_first[2], g_prof_idle_skips[2];
u32 g_prof_selfloop[2]; // block returned to its own start (not idle-skipped)
u32 g_prof_dispatch[9]; // native returns by first failing sh2_dispatch check; [8] chained blocks
u32 g_prof_spec[4];     // stack spec: high-RAM compiles scanned, plans active, planned accesses, bails
#endif
extern "C" void YuiMsg(const char *, ...);
extern "C" void VitaSh2ReportExecution() {
  if (VitaTelemetrySamplingEnabled()) {
    const auto sorted = hot_samples.Ranked();
    YuiMsg("jit_hot_summary samples=%llu dropped=%llu policy=periodic_1024 cpu_busy=unmeasured",
      hot_samples.samples, hot_samples.dropped);
#ifdef VITA_STACK_PROFILE
    {
      extern i_desc opcode_list[];
      unsigned order[256]; for (unsigned i = 0; i < 256; ++i) order[i] = i;
      std::sort(order, order + 256, [](unsigned a, unsigned b) { return outside_hist[a] > outside_hist[b]; });
      YuiMsg("jit_region_mix region_ops=%llu outside_ops=%llu mem_ops=%llu mem_r15=%llu", region_ops, outside_ops,
             mem_ops_total, mem_ops_r15);
      mem_ops_total = mem_ops_r15 = 0;
      for (unsigned i = 0; i < 24 && outside_hist[order[i]]; ++i)
        YuiMsg("jit_outside op=%s count=%llu", opcode_list[order[i]].mnem, outside_hist[order[i]]);
      region_ops = outside_ops = 0; memset(outside_hist, 0, sizeof(outside_hist));
    }
#endif
    for (unsigned i = 0; i < 8 && sorted[i].samples; ++i) {
      const auto &entry = sorted[i];
      const u32 region = entry.pc & 0x0ff00000;
      u8 *ram = region == 0x00200000 ? LowWram : region == 0x06000000 ? HighWram : nullptr;
      char prefix[81] = {}; unsigned used = 0;
      // Report-time bytes are explicitly NOT asserted to be the sampled code
      // version. Never perform MMIO or helper reads for a diagnostic prefix.
      if (ram && !(entry.pc & 1)) for (unsigned word = 0; word < 16 && (entry.pc & 0xfffff) + word * 2 + 1 < 0x100000; ++word)
        used += snprintf(prefix + used, sizeof(prefix) - used, "%04x ",
          T2ReadWord(ram, (entry.pc & 0xfffff) + word * 2));
      YuiMsg("jit_hot cpu=%s pc=%08x samples=%llu native_wall_us=%llu guest_cycles=%llu current_source=%s",
        entry.slave ? "slave" : "master", entry.pc, entry.samples, entry.us, entry.cycles, prefix);
#ifdef VITA_STACK_PROFILE
      { /* Profile only: one dump of the top blocks' native code, for offline disassembly. */
        static unsigned reports;
        if (i == 0) ++reports;
        if (reports == 40 && i < 3 && (entry.pc & 0x0ff00000) == 0x06000000) {
          Block *b = CompileBlocks::getInstance()->LookupTable[(entry.pc & 0xfffff) >> 1];
          if (b && b->b_addr == entry.pc) {
            const u32 *w = reinterpret_cast<const u32 *>(b->code);
            for (unsigned line = 0; line < 8; ++line) {
              char hex[64 * 9 + 64]; int m = snprintf(hex, sizeof(hex), "jit_code pc=%08x end=%08x off=%u:", entry.pc, b->e_addr, line * 64);
              for (unsigned k = 0; k < 64; ++k) m += snprintf(hex + m, sizeof(hex) - m, " %08x", w[line * 64 + k]);
              YuiMsg("%s", hex);
            }
          }
        }
      }
#endif
      // At most one extra record per report. Explain the top block's initial
      // PC-relative load without invoking MMIO or treating mutable literals
      // as immutable compile-time constants. These are REPORT-TIME bytes.
      if (!i && ram && !(entry.pc & 1)) {
        const u16 op=T2ReadWord(ram,entry.pc & 0xfffff);
        const unsigned kind=op>>12;
        if(kind==9 || kind==13) {
          const unsigned width=kind==9 ? 2 : 4;
          const u32 address=((entry.pc+4)&(kind==13 ? ~3u : ~0u))+(op&255)*width;
          const u32 offset=address&0xfffff;
          if((address&0x0ff00000)==region && offset<=0x100000-width) {
            const u32 value=width==2 ? u32(s32(s16(T2ReadWord(ram,offset)))) : T2ReadLong(ram,offset);
            YuiMsg("jit_hot_literal cpu=%s pc=%08x address=%08x width=%u register=%u report_value=%08x",
                   entry.slave ? "slave" : "master",entry.pc,address,width,(op>>8)&15,value);
          }
        }
      }
      if (entry.load_samples)
        YuiMsg("jit_hot_load cpu=%s pc=%08x samples=%llu entry_address_min=%08x entry_address_max=%08x",
          entry.slave ? "slave" : "master", entry.pc, entry.load_samples, entry.address_min, entry.address_max);
    }
    hot_samples = {};
  }
  // One complete record group: YuiMsg flushes the file on each call. Preserve
  // all four line formats while avoiding three extra storage flushes/locks.
  YuiMsg("jit_execution native_returns=%llu guest_cycles=%llu sampled_calls=%llu sampled_us=%llu\n"
    "jit_resident_loops calls=%llu iterations=%llu guest_cycles=%llu\n"
    "jit_poll_skips calls=%llu iterations=%llu guest_cycles=%llu\n"
    "jit_chains batches=%llu blocks=%llu budget=%u quota=%u miss=%u interrupt=%u loop=%u mapping=%u post_block=%u",
    (unsigned long long)(native_returns - poll_step_calls), (unsigned long long)(native_cycles - poll_step_cycles),
    native_samples - poll_step_samples, native_sample_us - poll_step_us,
    resident_calls, resident_iterations, resident_cycles,
    poll_calls, poll_iterations, poll_cycles,
    chain_batches, chain_blocks, chain_stops[0], chain_stops[1], chain_stops[2],
    chain_stops[3], chain_stops[4], chain_stops[5], chain_stops[6]);
  if (poll_step_calls)
    YuiMsg("jit_poll_steps calls=%llu guest_cycles=%llu sampled_calls=%llu sampled_us=%llu",
      poll_step_calls, poll_step_cycles, poll_step_samples, poll_step_us);
  poll_step_calls = poll_step_cycles = poll_step_samples = poll_step_us = 0;
  if (poll_fused_blocks)
    YuiMsg("jit_poll_fusion compiled=%u saved_bytes=%u", poll_fused_blocks, poll_fused_saved_bytes);
  poll_fused_blocks = poll_fused_saved_bytes = 0;
  native_returns = native_cycles = native_samples = native_sample_us = 0;
  resident_calls = resident_iterations = resident_cycles = 0;
  poll_calls = poll_iterations = poll_cycles = 0;
  chain_batches = chain_blocks = 0;
  memset(chain_stops, 0, sizeof(chain_stops));
}
#if !defined(_WINDOWS) && !defined(VITA)
#if defined(__arm__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{ 
    const int syscall = 0xf0002;
      __asm __volatile (
     "mov   r0, %0\n"      
     "mov   r1, %1\n"
     "mov   r7, %2\n"
     "mov     r2, #0x0\n"
     "svc     0x00000000\n"
     :
     : "r" (begin), "r" (end), "r" (syscall)
     : "r0", "r1", "r7"
    );
}
#elif defined(__aarch64__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{
    // Don't rely on GCC's __clear_cache implementation, as it caches
    // icache/dcache cache line sizes, that can vary between cores on
    // big.LITTLE architectures.
    uint64_t addr, ctr_el0;
    static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
    size_t isize, dsize;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    isize = 4 << ((ctr_el0 >> 0) & 0xf);
    dsize = 4 << ((ctr_el0 >> 16) & 0xf);

    // use the global minimum cache line size
    icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
    dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

    addr = (uint64_t)begin & ~(uint64_t)(dsize - 1);
    for (; addr < (uint64_t)end; addr += dsize)
        // use "civac" instead of "cvau", as this is the suggested workaround for
        // Cortex-A53 errata 819472, 826319, 827319 and 824069.
            __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    addr = (uint64_t)begin & ~(uint64_t)(isize - 1);
    for (; addr < (uint64_t)end; addr += isize)
            __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}
#else
void cacheflush(uintptr_t begin, uintptr_t end, int flag ){
  __builtin___clear_cache((void*)begin,(void*)end);
}
#endif
#endif

i_desc opcode_list[] =
{
  { ZERO_F,	"clrt",						0xffff, 0x8,	0, sh2_CLRT},
  { ZERO_F,   "clrmac",					0xffff, 0x28,	0, sh2_CLRMAC},
  { ZERO_F,   "div0u",					0xffff, 0x19,	0, sh2_DIV0U},
  { ZERO_F,   "nop",						0xffff, 0x9,	0, sh2_NOP},
  { ZERO_F,   "rte",						0xffff, 0x2b,	0, sh2_RTE},
  { ZERO_F,   "rts",						0xffff, 0xb,	0, sh2_RTS},
  { ZERO_F,   "sett",						0xffff, 0x18,	0, sh2_SETT},
  { ZERO_F,   "sleep",					0xffff, 0x1b,	0, sh2_SLEEP},
  { N_F,      "cmp/pl r%d",				0xf0ff, 0x4015,	0, sh2_CMP_PL},
  { N_F,      "cmp/pz r%d",				0xf0ff, 0x4011,	0, sh2_CMP_PZ},
  { N_F,      "dt r%d",					0xf0ff, 0x4010,	0, sh2_DT},
  { N_F,      "movt r%d",					0xf0ff, 0x0029,	0, sh2_MOVT},
  { N_F,      "rotl r%d",					0xf0ff, 0x4004,	0, sh2_ROTL},
  { N_F,      "rotr r%d",					0xf0ff, 0x4005,	0, sh2_ROTR},
  { N_F,      "rotcl r%d",				0xf0ff, 0x4024,	0, sh2_ROTCL},
  { N_F,      "rotcr r%d",				0xf0ff, 0x4025,	0, sh2_ROTCR},
  { N_F,      "shal r%d",					0xf0ff, 0x4020,	0, sh2_SHL},
  { N_F,      "shar r%d",					0xf0ff, 0x4021,	0, sh2_SHAR},
  { N_F,      "shll r%d",					0xf0ff, 0x4000,	0, sh2_SHL},
  { N_F,      "shlr r%d",					0xf0ff, 0x4001,	0, sh2_SHLR},
  { N_F,      "shll2 r%d",				0xf0ff, 0x4008,	0, sh2_SHLL2},
  { N_F,      "shlr2 r%d",				0xf0ff, 0x4009,	0, sh2_SHLR2},
  { N_F,      "shll8 r%d",				0xf0ff, 0x4018,	0, sh2_SHLL8},
  { N_F,      "shlr8 r%d",				0xf0ff, 0x4019,	0, sh2_SHLR8},
  { N_F,      "shll16 r%d",				0xf0ff, 0x4028,	0, sh2_SHLL16},
  { N_F,      "shlr16 r%d",				0xf0ff, 0x4029,	0, sh2_SHLL16},
  { N_F,      "stc sr, r%d",				0xf0ff, 0x0002,	0, sh2_STC_SR},
  { N_F,      "stc gbr, r%d",				0xf0ff, 0x0012,	0, sh2_STC_GBR},
  { N_F,      "stc vbr, r%d",				0xf0ff, 0x0022,	0, sh2_STC_VBR},
  { N_F,      "sts mach, r%d",			0xf0ff, 0x000a,	0, sh2_STS_MACH},
  { N_F,      "sts macl, r%d",			0xf0ff, 0x001a,	0, sh2_STS_MACL},
  { N_F,      "sts pr, r%d",				0xf0ff, 0x002a,	0, sh2_STS_PR},
  { N_F,      "tas.b @r%d",				0xf0ff, 0x401b,	0, sh2_TAS_B},
  { N_F,      "stc.l sr, @-r%d",			0xf0ff, 0x4003,	0, sh2_STC_SR_DEC},
  { N_F,      "stc.l gbr, @-r%d",			0xf0ff, 0x4013,	0, sh2_STC_GBR_DEC},
  { N_F,      "stc.l vbr, @-r%d",			0xf0ff, 0x4023,	0, sh2_STC_VBR_DEC},
  { N_F,      "sts.l mach, @-r%d",		0xf0ff, 0x4002,	0, sh2_STS_MACH_DEC},
  { N_F,      "sts.l macl, @-r%d",		0xf0ff, 0x4012,	0, sh2_STS_MACL_DEC},
  { N_F,      "sts.l pr, @-r%d",			0xf0ff, 0x4022,	0, sh2_STS_PR_DEC},
  { M_F,      "ldc r%d, sr",				0xf0ff, 0x400e,	0, sh2_LDC_SR},
  { M_F,      "ldc r%d, gbr",				0xf0ff, 0x401e,	0, sh2_LDC_GBR},
  { M_F,      "ldc r%d, vbr",				0xf0ff, 0x402e,	0, sh2_LDC_VBR},
  { M_F,      "lds r%d, mach",			0xf0ff, 0x400a,	0, sh2_LDS_MACH},
  { M_F,      "lds r%d, macl",			0xf0ff, 0x401a,	0, sh2_LDS_MACL},
  { M_F,      "lds r%d, pr",				0xf0ff, 0x402a,	0, sh2_LDS_PR},
  { M_F,      "jmp @r%d",					0xf0ff, 0x402b,	0, sh2_JMP},
  { M_F,      "jsr @r%d",					0xf0ff, 0x400b,	0, sh2_JSR}, 
  { M_F,      "ldc.l @r%d+, sr",			0xf0ff, 0x4007,	0, sh2_LDC_SR_INC},
  { M_F,      "ldc.l @r%d+, gbr",			0xf0ff, 0x4017,	0, sh2_LDC_GBR_INC},
  { M_F,      "ldc.l @r%d+, vbr",			0xf0ff, 0x4027,	0, sh2_LDC_VBR_INC},
  { M_F,      "lds.l @r%d+, mach",		0xf0ff, 0x4006,	0, sh2_LDS_MACH_INC},
  { M_F,      "lds.l @r%d+, macl",		0xf0ff, 0x4016,	0, sh2_LDS_MACL_INC},
  { M_F,      "lds.l @r%d+, pr",			0xf0ff, 0x4026,	0, sh2_LDS_PR_INC},
  { M_F,      "braf r%d",					0xf0ff, 0x0023,	0, sh2_BRAF},
  { M_F,      "bsrf r%d",					0xf0ff, 0x0003,	0, sh2_BSRF},
  { NM_F,     "add r%d, r%d",				0xf00f, 0x300c,	0, sh2_ADD},
  { NM_F,     "addc r%d, r%d",			0xf00f, 0x300e,	0, sh2_ADDC},
  { NM_F,     "addv r%d, r%d",			0xf00f, 0x300f,	0, sh2_ADDV},
  { NM_F,     "and r%d, r%d",				0xf00f, 0x2009,	0, sh2_AND},
  { NM_F,     "cmp/eq r%d, r%d",			0xf00f, 0x3000,	0, sh2_CMP_EQ},
  { NM_F,     "cmp/hs r%d, r%d",			0xf00f, 0x3002,	0, sh2_CMP_HS},
  { NM_F,     "cmp/ge r%d, r%d",			0xf00f, 0x3003,	0, sh2_CMP_GE},
  { NM_F,     "cmp/hi r%d, r%d",			0xf00f, 0x3006,	0, sh2_CMP_HI},
  { NM_F,     "cmp/gt r%d, r%d",			0xf00f, 0x3007,	0, sh2_CMP_GT},
  { NM_F,     "cmp/str r%d, r%d",			0xf00f, 0x200c,	0, sh2_CMP_STR},
  { NM_F,     "div1 r%d, r%d",			0xf00f, 0x3004,	0, sh2_DIV1},
  { NM_F,     "div0s r%d, r%d",			0xf00f, 0x2007,	0, sh2_DIV0S},
  { NM_F,     "dmuls.l r%d, r%d",			0xf00f, 0x300d,	0, sh2_DMULS_L},
  { NM_F,     "dmulu.l r%d, r%d",			0xf00f, 0x3005,	0, sh2_DMULU_L},
  { NM_F,     "exts.b r%d, r%d",			0xf00f, 0x600e,	0, sh2_EXTS_B},
  { NM_F,     "exts.w r%d, r%d",			0xf00f, 0x600f,	0, sh2_EXTS_W},
  { NM_F,     "extu.b r%d, r%d",			0xf00f, 0x600c,	0, sh2_EXTU_B},
  { NM_F,     "extu.w r%d, r%d",			0xf00f, 0x600d,	0, sh2_EXTU_W},
  { NM_F,     "mov r%d, r%d",				0xf00f, 0x6003,	0, sh2_MOVR},
  { NM_F,     "mul.l r%d, r%d",			0xf00f, 0x0007,	0, sh2_MUL_L},
  { NM_F,     "muls.w r%d, r%d",			0xf00f, 0x200f,	0, sh2_MULS},
  { NM_F,     "mulu.w r%d, r%d",			0xf00f, 0x200e,	0, sh2_MULU},
  { NM_F,     "neg r%d, r%d",				0xf00f, 0x600b,	0, sh2_NEG},
  { NM_F,     "negc r%d, r%d",			0xf00f, 0x600a,	0, sh2_NEGC},
  { NM_F,     "not r%d, r%d",				0xf00f, 0x6007,	0, sh2_NOT},
  { NM_F,     "or r%d, r%d",				0xf00f, 0x200b,	0, sh2_OR},
  { NM_F,     "sub r%d, r%d",				0xf00f, 0x3008,	0, sh2_SUB},
  { NM_F,     "subc r%d, r%d",			0xf00f, 0x300a,	0, sh2_SUBC},
  { NM_F,     "subv r%d, r%d",			0xf00f, 0x300b,	0, sh2_SUBV},
  { NM_F,     "swap.b r%d, r%d",			0xf00f, 0x6008,	0, sh2_SWAP_B},
  { NM_F,     "swap.w r%d, r%d",			0xf00f, 0x6009,	0, sh2_SWAP_W},
  { NM_F,     "tst r%d, r%d",				0xf00f, 0x2008,	0, sh2_TST},
  { NM_F,     "xor r%d, r%d",				0xf00f, 0x200a,	0, sh2_XOR},
  { NM_F,     "xtrct r%d, r%d",			0xf00f, 0x200d,	0, sh2_XTRCT},
  { NM_F,     "mov.b r%d, @r%d",			0xf00f, 0x2000,	0, sh2_MOVB},
  { NM_F,     "mov.w r%d, @r%d",			0xf00f, 0x2001,	0, sh2_MOVW},
  { NM_F,     "mov.l r%d, @r%d",			0xf00f, 0x2002,	0, sh2_MOVL},
  { NM_F,     "mov.b @r%d, r%d",			0xf00f, 0x6000,	0, sh2_MOVB_MEM},
  { NM_F,     "mov.w @r%d, r%d",			0xf00f, 0x6001,	0, sh2_MOVW_MEM},
  { NM_F,     "mov.l @r%d, r%d",			0xf00f, 0x6002,	0, sh2_MOVL_MEM},
  { NM_F,     "mac.l @r%d+, @r%d+",		0xf00f, 0x000f,	0, sh2_MAC_L},
  { NM_F,     "mac.w @r%d+, @r%d+",		0xf00f, 0x400f,	0, sh2_MAC_W},
  { NM_F,     "mov.b @r%d+, r%d",			0xf00f, 0x6004,	0, sh2_MOVB_INC},
  { NM_F,     "mov.w @r%d+, r%d",			0xf00f, 0x6005,	0, sh2_MOVW_INC},
  { NM_F,     "mov.l @r%d+, r%d",			0xf00f, 0x6006,	0, sh2_MOVL_INC},
  { NM_F,     "mov.b r%d, @-r%d",			0xf00f, 0x2004,	0, sh2_MOVB_DEC},
  { NM_F,     "mov.w r%d, @-r%d",			0xf00f, 0x2005,	0, sh2_MOVW_DEC},
  { NM_F,     "mov.l r%d, @-r%d",			0xf00f, 0x2006,	0, sh2_MOVL_DEC},
  { NM_F,     "mov.b r%d, @(r0, r%d)",	0xf00f, 0x0004,	0, sh2_MOVB_R0},
  { NM_F,     "mov.w r%d, @(r0, r%d)",	0xf00f, 0x0005,	0, sh2_MOVW_R0},
  { NM_F,     "mov.l r%d, @(r0, r%d)",	0xf00f, 0x0006,	0, sh2_MOVL_R0},
  { NM_F,     "mov.b @(r0, r%d), r%d",	0xf00f, 0x000c,	0, sh2_MOVB_R0_MEM},
  { NM_F,     "mov.w @(r0, r%d), r%d",	0xf00f, 0x000d,	0, sh2_MOVW_R0_MEM},
  { NM_F,     "mov.l @(r0, r%d), r%d",	0xf00f, 0x000e,	0, sh2_MOVL_R0_MEM},
  { MD_F,     "mov.b @(0x%03X, r%d), r0",	0xff00, 0x8400,	0, sh2_MOVB_DISP_R0},
  { MD_F,     "mov.w @(0x%03X, r%d), r0", 0xff00, 0x8500,	0, sh2_MOVW_DISP_R0},
  { ND4_F,    "mov.b r0, @(0x%03X, r%d)", 0xff00, 0x8000,	0, sh2_MOVB_R0_DISP},
  { ND4_F,    "mov.w r0, @(0x%03X, r%d)", 0xff00, 0x8100,	0, sh2_MOVW_R0_DISP},
  { NMD_F,    "mov.l r%d, @(0x%03X, r%d)",0xf000, 0x1000,	0, sh2_MOVL_DISP_MEM},
  { NMD_F,    "mov.l @(0x%03X, r%d), r%d",0xf000, 0x5000,	0, MOVLL4},
  { D_F,      "mov.b r0, @(0x%03X, gbr)",	0xff00, 0xc000,	1, sh2_MOVB_R0_GBR},
  { D_F,      "mov.w r0, @(0x%03X, gbr)",	0xff00, 0xc100,	2, sh2_MOVW_R0_GBR},
  { D_F,      "mov.l r0, @(0x%03X, gbr)",	0xff00, 0xc200,	4, sh2_MOVL_R0_GBR},
  { D_F,      "mov.b @(0x%03X, gbr), r0",	0xff00, 0xc400,	1, sh2_MOVB_GBR_R0},
  { D_F,      "mov.w @(0x%03X, gbr), r0",	0xff00, 0xc500,	2, sh2_MOVW_GBR_R0},
  { D_F,      "mov.l @(0x%03X, gbr), r0",	0xff00, 0xc600,	4, sh2_MOVL_GBR_R0},
  { D_F,      "mova @(0x%03X, pc), r0",	0xff00, 0xc700,	4, sh2_MOVA},
  { D_F,      "bf 0x%08X",				0xff00, 0x8b00,	4, sh2_BF},
  { D_F,      "bf/s 0x%08X",				0xff00, 0x8f00,	5, sh2_BF_S},
  { D_F,      "bt 0x%08X",				0xff00, 0x8900,	5, sh2_BT},
  { D_F,      "bt/s 0x%08X",				0xff00, 0x8d00,	5, sh2_BT_S},
  { D12_F,    "bra 0x%08X",				0xf000, 0xa000,	5, sh2_BRA},
  { D12_F,    "bsr 0x%08X",				0xf000, 0xb000,	0, sh2_BSR},
  { ND8_F,    "mov.w @(0x%03X, pc), r%d", 0xf000, 0x9000,	0, sh2_MOV_DISP_W},
  { ND8_F,    "mov.l @(0x%03X, pc), r%d",	0xf000, 0xd000,	2, sh2_MOV_DISP_L},
  { I_F,      "and.b #0x%02X, @(r0, gbr)",0xff00, 0xcd00,	4, sh2_AND_B},
  { I_F,      "or.b #0x%02X, @(r0, gbr)", 0xff00, 0xcf00,	0, sh2_OR_B},
  { I_F,      "tst.b #0x%02X, @(r0, gbr)",0xff00, 0xcc00,	0, sh2_TST_B},
  { I_F,      "xor.b #0x%02X, @(r0, gbr)",0xff00, 0xce00,	0, sh2_XOR_B},
  { I_F,      "and #0x%02X, r0",			0xff00, 0xc900,	0, sh2_ANDI},
  { I_F,      "cmp/eq #0x%02X, r0",		0xff00, 0x8800,	0, sh2_CMP_EQ_IMM},
  { I_F,      "or #0x%02X, r0",			0xff00, 0xcb00,	0, sh2_ORI},
  { I_F,      "tst #0x%02X, r0",			0xff00, 0xc800,	0, sh2_TST_R0},
  { I_F,      "xor #0x%02X, r0",			0xff00, 0xca00,	0, sh2_XORI},
  { I_F,      "trapa #0x%X",				0xff00, 0xc300,	0, sh2_TRAPA},
  { NI_F,     "add #0x%02X, r%d",			0xf000, 0x7000,	0, sh2_ADDI},
  { NI_F,     "mov #0x%02X, r%d",			0xf000, 0xe000,	0, sh2_MOVI},
  { 0,        NULL,						0,      0,		0, 0}
};


void DumpInstX( int i, u32 pc, u16 op  )
{
  char buf[256];
  //sprintf( buf, "%08X : ", pc );

  if (opcode_list[i].format == ZERO_F)
    sprintf( buf,"%s", opcode_list[i].mnem);
  else if (opcode_list[i].format == N_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == M_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 8) & 0xf);
  else if (opcode_list[i].format == NM_F)
    sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op >> 8) & 0xf);
  else if (opcode_list[i].format == MD_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, op & 0xf, (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == ND4_F)
  {
    if (op & 0x100)
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 2, (op >> 4) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf), (op >> 4) & 0xf);
  }
  else if (opcode_list[i].format == NMD_F)
  {
    if ((op & 0xf000) == 0x1000)
      sprintf( buf,opcode_list[i].mnem, (op >> 4) & 0xf, (op & 0xf) * 4, (op >> 8) & 0xf);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xf) * 4, (op >> 4) & 0xf, (op >> 8) & 0xf);
  }
  else if (opcode_list[i].format == D_F)
  {
    if (opcode_list[i].dat <= 4)
    {
      if ((op & 0xff00) == 0xc700)
      {
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4);
//				LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + PC);
      }
      else
        sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat);
    }
    else
    {
      if (op & 0x80)  /* sign extend */
        sprintf( buf,opcode_list[i].mnem, (((op & 0xff) + 0xffffff00) * 2) + pc + 4);
      else
        sprintf( buf,opcode_list[i].mnem, ((op & 0xff) * 2) + pc + 4);
    }        
  }
  else if (opcode_list[i].format == D12_F)
  {
    if (op & 0x800)         /* sign extend */
      sprintf( buf,opcode_list[i].mnem, ((op & 0xfff) + 0xfffff000) * 2 + pc + 4);
    else
      sprintf( buf,opcode_list[i].mnem, (op & 0xfff) * 2 + pc + 4);
  }
  else if (opcode_list[i].format == ND8_F)
  {
    if ((op & 0xf000) == 0x9000)    /* .W */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + pc);
    }
    else  /* .L */
    {
      sprintf( buf,opcode_list[i].mnem, (op & 0xff) * opcode_list[i].dat + 4, (op >> 8) & 0xf);
      //LOG("  ; 0x%08X", (op & 0xff) * opcode_list[i].dat + 4 + ((pc) & 0xfffffffc));
    }
  }
  else if (opcode_list[i].format == I_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff);
  else if (opcode_list[i].format == NI_F)
    sprintf( buf,opcode_list[i].mnem, op & 0xff, (op >> 8) & 0xf);
  else
  {
    sprintf( buf,"unrecognized\n");
    return;
  }
  LOG("%08X:%04X %s\n", pc , op, buf );
  return;
}


#define opdesc(op, y, c, d)	x86op_desc(x86_##op, &op##_size, &op##_src, &op##_dest, &op##_off1, &op##_imm, &op##_off3, y, c, d)

#define opNULL			x86op_desc(0,0,0,0,0,0,0,0,0,0)

#if defined(_WINDOWS)
#define PROLOGSIZE		     27    
#define EPILOGSIZE		      3
#define SEPERATORSIZE	     10
#define SEPERATORSIZE_NORMAL 7
#define SEPERATORSIZE_DEBUG  24
#define SEPERATORSIZE_DELAY  7
#define SEPERATORSIZE_DELAY_SLOT  27
#define SEPERATORSIZE_DELAY_AFTER  10 
#define SEPERATORSIZE_DELAYD_DEBUG 34
#define DELAYJUMPSIZE	     17
#define DALAY_CLOCK_OFFSET 6
#define DALAY_CLOCK_OFFSET_DEBUG 6
#define NORMAL_CLOCK_OFFSET 6
#define NORMAL_CLOCK_OFFSET_DEBUG 3
#elif defined(AARCH64)
#define PROLOGSIZE		     (11*4)    
#define SEPERATORSIZE_NORMAL (2*4)
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  (18*4)
#define SEPERATORSIZE_DELAY_SLOT  (15*4)
#define SEPERATORSIZE_DELAY_AFTER  (12*4)
#define DALAY_CLOCK_OFFSET (2*4)
#define DALAY_CLOCK_OFFSET_DEBUG (10*4)
#define SEPERATORSIZE_DELAYD_DEBUG (20*4)
#define EPILOGSIZE		      (10*4)
#define DELAYJUMPSIZE	     (11*4)
#else // ARMv7
#ifdef VITA_SH2_CHAIN_ABI
#define PROLOGSIZE                28
#else
#define PROLOGSIZE		     16    
#endif
#define SEPERATORSIZE_NORMAL 8
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  36
#define SEPERATORSIZE_DELAY_SLOT  36
#define SEPERATORSIZE_DELAY_AFTER  20
#define DALAY_CLOCK_OFFSET 8
#define DALAY_CLOCK_OFFSET_DEBUG 8
#define SEPERATORSIZE_DELAYD_DEBUG 60
#define EPILOGSIZE		      12
#define DELAYJUMPSIZE	     32
#endif


#define MININSTRSIZE    3
#define MAXINSTRSIZE	416
#define MAXJUMPSIZE		46

#define SAFEPAGESIZE	MAXBLOCKSIZE - MAXINSTRSIZE - SEPERATORSIZE_DELAY_SLOT - SEPERATORSIZE_DELAY_AFTER - SEPERATORSIZE_NORMAL - EPILOGSIZE
#define MAXINSTRCNT		MAXBLOCKSIZE/(MININSTRSIZE+SEPERATORSIZE_NORMAL)
int instrSize[NUMOFBLOCKS][MAXINSTRCNT];

#define opinit(x)	extern const unsigned short x##_size; \
                    extern const unsigned char x##_src, x##_dest, x##_off1, x##_imm, x##_off3; \
            void x86_##x();


extern "C" {

opinit(CLRT);
opinit(CLRMAC);
opinit(NOP);
opinit(DIV0U);
opinit(SETT);
opinit(SLEEP);
opinit(STS_MACH);
opinit(STS_MACL);
opinit(STS_MACH_DEC);
opinit(STS_MACL_DEC);
opinit(STS_PR);
opinit(STC_SR);
opinit(STC_VBR);
opinit(STC_GBR);
opinit(LDS_PR);
opinit(LDS_MACH);
opinit(LDS_MACL);
opinit(LDS_PR_INC);
opinit(LDS_MACH_INC);
opinit(LDS_MACL_INC);
opinit(LDC_SR);
opinit(LDC_VBR);
opinit(LDCGBR);
opinit(LDC_SR_INC);
opinit(LDC_VBR_INC);
opinit(LDC_GBR_INC);
opinit(STC_SR_MEM);
opinit(STC_VBR_MEM);
opinit(STC_GBR_MEM);
opinit(STSMPR);
opinit(CMP_EQ);
opinit(CMP_HI);
opinit(CMP_GE);
opinit(CMP_HS);
opinit(CMP_GT);
opinit(DT);
opinit(CMP_PL);
opinit(CMP_PZ);
opinit(ROTL);
opinit(ROTR);
opinit(ROTCL);
opinit(ROTCR);
opinit(SHL);
opinit(SHLR);
opinit(SHAR);
opinit(SHLL2);
opinit(SHLR2);
opinit(SHLL8);
opinit(SHLR8);
opinit(SHLL16);
opinit(SHLR16);
opinit(AND);
opinit(OR);
opinit(XOR);
opinit(NOT);
opinit(XTRCT);
opinit(ADD);
opinit(ADDC);
opinit(SUB);
opinit(SUBC);
opinit(NEG);
opinit(NEGC);
opinit(TST);
opinit(TSTI);
opinit(AND_B);
opinit(OR_B);
opinit(TST_B);
opinit(XOR_B);
opinit(ADDI);
opinit(ANDI);
opinit(ORI);
opinit(XORI);
opinit(MOVI);
opinit(CMP_EQ_IMM);
opinit(SWAP_B);
opinit(SWAP_W);
opinit(EXTUB);
opinit(EXTU_W);
opinit(EXTS_B);
opinit(EXTS_W);
opinit(BT)
opinit(BF);
opinit(BF_S);
opinit(BT_S);
opinit(JMP);
opinit(JSR);
opinit(BRA);
opinit(BSR);
opinit(BSRF);
opinit(BRAF);
opinit(RTE);
opinit(RTS);
opinit(MOVA);
opinit(MOVT);
opinit(MOVBL);
opinit(MOVWL);
opinit(MOVL_MEM_REG);
opinit(MOVBS);
opinit(MOVWS);
opinit(MOVLS);
opinit(MOVWI);
opinit(MOVLI);
opinit(MOVBS4);
opinit(MOVWS4);
opinit(MOVLS4);
opinit(MOVR);
opinit(MOVBP);
opinit(MOVWP);
opinit(MOVLP);
opinit(MOVBL0);
opinit(MOVWL0);
opinit(MOVLL0);
opinit(MOVBS0);
opinit(MOVWS0);
opinit(MOVLS0);
opinit(MOVBSG);
opinit(MOVLSG);
opinit(MOVWL4);  // 0x8500
opinit(MOVLL4);
opinit(MOVLM);
opinit(TAS);
opinit(ADDV);
opinit(CMPSTR);
opinit(DIV0S);
opinit(DMULS);
opinit(DMULU);
opinit(MULL);
opinit(MULS);
opinit(MULU);
opinit(SUBV);
opinit(MAC_L);
opinit(MOVBM);
opinit(MOVWM);
opinit(MOVBL4);
opinit(MOVWSG);
opinit(MOVBLG);
opinit(MOVWLG);
opinit(MOVLLG);
opinit(TRAPA);
opinit(DIV1);
opinit(MAC_W);


void prologue(void);
void epilogue(void);
void seperator(void);
void seperator_normal(void);
void seperator_delay(void);
void seperator_delay_slot(void);
void seperator_delay_after(void);

void seperator_d_normal(void);
void seperator_d_delay(void);

void PageJump(void); // jumps to a different page
void PageFlip(void); // "flips" the page

#if defined(AARCH64)
void internal_jmp(void);
void internal_delay_jmp(void);
#else
void internal_jmp(void){ printf("nope"); }
void internal_delay_jmp(void){ printf("nope"); }
#endif
int internal_jmp_size = 18*4;
int internal_delay_jmp_size = 15*4;
const intptr_t internal_jmp_to_offset = 4*4;
const intptr_t internal_delay_jmp_to_offset = 4*4;
 
extern x86op_desc asm_list[];

}

x86op_desc asm_list[] =
{
  opdesc(CLRT,0,1,0),       
  opdesc(CLRMAC,0,1,0),
  opdesc(DIV0U,0,1,0),
  opdesc(NOP,0,1,0),
  opdesc(RTE,4,4,0),
  opdesc(RTS,4,2,0),
  opdesc(SETT,0,1,0),
  opdesc(SLEEP,0,8,0),
  opdesc(CMP_PL,0,1,0),
  opdesc(CMP_PZ,0,1,0),
  opdesc(DT,0,1,1),
  opdesc(MOVT,0,1,0),
  opdesc(ROTL,0,1,0),
  opdesc(ROTR,0,1,0),
  opdesc(ROTCL,0,1,0),
  opdesc(ROTCR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHAR,0,1,0),
  opdesc(SHL,0,1,0),
  opdesc(SHLR,0,1,0),
  opdesc(SHLL2,0,1,0),
  opdesc(SHLR2,0,1,0),
  opdesc(SHLL8,0,1,0),
  opdesc(SHLR8,0,1,0),
  opdesc(SHLL16,0,1,0),
  opdesc(SHLR16,0,1,0),
  opdesc(STC_SR,0xFF,1,1),
  opdesc(STC_GBR,0xFF,1,1),
  opdesc(STC_VBR,0xFF,1,1),
  opdesc(STS_MACH,0xFF,1,0),
  opdesc(STS_MACL,0xFF,1,0),
  opdesc(STS_PR,0xFF,1,0),
  opdesc(TAS,0,4,1),         // 0x401b
  opdesc(STC_SR_MEM,0xFF,2,1),
  opdesc(STC_GBR_MEM,0xFF,2,1),
  opdesc(STC_VBR_MEM,0xFF,2,1),
  opdesc(STS_MACH_DEC,0xFF,2,1),
  opdesc(STS_MACL_DEC,0xFF,2,1),
  opdesc(STSMPR,0xFF,1,1),     // 0x4022
  opdesc(LDC_SR,0xFF,1,0),
  opdesc(LDCGBR,0xFF,1,0),
  opdesc(LDC_VBR,0xFF,1,0),
  opdesc(LDS_MACH,0xFF,1,0),
  opdesc(LDS_MACL,0xFF,1,0),
  opdesc(LDS_PR,0xFF,1,0),
  opdesc(JMP, 3,2,0),
  opdesc(JSR, 4,2,0),
  opdesc(LDC_SR_INC,0xFF,3,0),  
  opdesc(LDC_GBR_INC,0xFF,3,0),
  opdesc(LDC_VBR_INC,0xFF,3,0),
  opdesc(LDS_MACH_INC,0xFF,1,0),
  opdesc(LDS_MACL_INC,0xFF,1,0),
  opdesc(LDS_PR_INC,0xFF,1,0),
  opdesc(BRAF,4,2,0),
  opdesc(BSRF,4,2,0),
  opdesc(ADD,0,1,1),
  opdesc(ADDC,0,1,1),
  opdesc(ADDV,0,1,1),  // 0x300F
  opdesc(AND,0,1,0),
  opdesc(CMP_EQ,0,1,0),
  opdesc(CMP_HS,0,1,0),
  opdesc(CMP_GE,0,1,0),
  opdesc(CMP_HI,0,1,0),
  opdesc(CMP_GT,0,1,0),
  opdesc(CMPSTR,0,1,0), // 0x200C
  opdesc(DIV1,0,1,0),   // 0x3004
  opdesc(DIV0S,0,1,0),  // 0x2007
  opdesc(DMULS,0,4,0),  // 0x300D
  opdesc(DMULU,0,4,0),  // 0x3005
  opdesc(EXTS_B,0,1,0),
  opdesc(EXTS_W,0,1,0),
  opdesc(EXTUB,0,1,0),
  opdesc(EXTU_W,0,1,0),
  opdesc(MOVR,0,1,0),  // 0x6003
  opdesc(MULL,0,4,0),  // 0x0007
  opdesc(MULS,0,3,0),  // 0x200f
  opdesc(MULU,0,3,0),  // 0x200e
  opdesc(NEG,0,1,0),
  opdesc(NEGC,0,1,0),
  opdesc(NOT,0,1,0),
  opdesc(OR,0,1,0),
  opdesc(SUB,0,1,0),
  opdesc(SUBC,0,1,1),
  opdesc(SUBV,0,1,1),  // 0x300B
  opdesc(SWAP_B,0,1,0),
  opdesc(SWAP_W,0,1,0),
  opdesc(TST,0,1,0),
  opdesc(XOR,0,1,0),
  opdesc(XTRCT,0,1,0),
  opdesc(MOVBS,0,1,1),
  opdesc(MOVWS,0,1,1),
  opdesc(MOVLS,0,1,1),
  opdesc(MOVBL,0,1,0), // 6000
  opdesc(MOVWL,0,1,0), // 6001
  opdesc(MOVL_MEM_REG,0,1,0),
  opdesc(MAC_L,0,3,0),  // 0x000f
  opdesc(MAC_W,0,3,0),  // 0x400f
  opdesc(MOVBP,0,1,0),  // 6004
  opdesc(MOVWP,0,1,0),  // 6005
  opdesc(MOVLP,0,1,1),  // 6006
  opdesc(MOVBM,0,1,1),  // 0x2004,
  opdesc(MOVWM,0,1, 0),  // 0x2005,
  opdesc(MOVLM,0,1,1),  // 0x2006
  opdesc(MOVBS0,0,1,1), // 0x0004
  opdesc(MOVWS0,0,1,1), // 0x0005
  opdesc(MOVLS0,0,1,1), // 0x0006
  opdesc(MOVBL0,0,1, 0), // 0x000C
  opdesc(MOVWL0,0,1, 0), // 0x000D 
  opdesc(MOVLL0,0,1, 0), // 0x000E
  opdesc(MOVBL4,0,1,1), // 0x8400
  opdesc(MOVWL4,0,1, 0), // 0x8500
  opdesc(MOVBS4,0,1, 0), // 0x8000
  opdesc(MOVWS4,0,1,1), // 0x8100
  opdesc(MOVLS4,0,1,1), // 0x1000
  opdesc(MOVLL4,0,1, 0), // 0x5000 ,1
  opdesc(MOVBSG,0,1,1), // 0xC000
  opdesc(MOVWSG,0,1,1), // 0xc100
  opdesc(MOVLSG,0,1,1), // 0xC200
  opdesc(MOVBLG,0,1, 0), // 0xC400
  opdesc(MOVWLG,0,1, 0), // 0xC500
  opdesc(MOVLLG,0,1, 0), // 0xC600
  opdesc(MOVA,0,1, 0),
  opdesc(BF,1,3, 0),
  opdesc(BF_S,3,2, 0),
  opdesc(BT,1,3, 0),
  opdesc(BT_S,3,2, 0),
  opdesc(BRA,2,2, 0),
  opdesc(BSR,2,2, 0),
  opdesc(MOVWI,0,1, 0),
  opdesc(MOVLI,0, 1, 0),
  opdesc(AND_B,0,3,1),
  opdesc(OR_B,0,3,1),
  opdesc(TST_B,0, 1, 0),
  opdesc(XOR_B,0,3,1),
  opdesc(ANDI,0,1, 0),
  opdesc(CMP_EQ_IMM,0,1, 0),
  opdesc(ORI,0,1, 0),
  opdesc(TSTI,0,1, 0),  // C800
  opdesc(XORI,0,1, 0),
  opdesc(TRAPA,5,8,1), // 0xc300
  opdesc(ADDI,0,1, 1),
  opdesc(MOVI,0,1, 0),
  opNULL
}; 

void CompileBlocks::Init()
{
  self_modify_block.clear();
  smc_slot_.assign(NUMOFBLOCKS, SmcVariant());
  smc_pcs_.clear();
  if (LookupParentTable) {
    for (unsigned i = 0; i < (0x100000 >> 1); ++i) LookupParentTable[i].clear();
  }
  if (!dCode) {
    dCode = (Block*)calloc(NUMOFBLOCKS, sizeof(Block));
    if (!dCode) throw std::bad_alloc();
  }
  memset((void*)dCode, 0, sizeof(Block)*NUMOFBLOCKS);
  unsigned char *code_base = VitaSh2CodeArena();
  for (unsigned i = 0; i < NUMOFBLOCKS; ++i)
    dCode[i].code = code_base + i * vitacode::Layout::Stride;
  code_off_.assign(NUMOFBLOCKS, 0);
  code_bytes_.assign(NUMOFBLOCKS, 0);
  code_page_slots_.assign(vitacode::Layout::Sh2 >> 12, {});
  code_cursor_ = 0;
  if (!link_out) {
    link_out = static_cast<LinkExit (*)[2]>(calloc(NUMOFBLOCKS, sizeof(*link_out)));
    if (!link_out) throw std::bad_alloc();
  }
  memset((void*)link_out, 0, sizeof(*link_out) * NUMOFBLOCKS);
  link_in.clear();
#if defined(VITA_SH2_ARENA_DISPATCH)
  if (!arena_dispatch) {
    // Blocks reach this copy with a direct B (predicted) instead of
    // ldr pc,[r7,#144]; tagSH2::dispatch is then unused by compiled code.
    const size_t bytes = reinterpret_cast<uintptr_t>(&sh2_dispatch_end) - reinterpret_cast<uintptr_t>(&sh2_dispatch);
    if (bytes > vitacode::Layout::Stubs) throw std::bad_alloc();
    arena_dispatch = code_base + vitacode::Layout::StubOffset;
    VitaSh2CodeWrite write(arena_dispatch, bytes);
    memcpy(arena_dispatch, reinterpret_cast<const void *>(&sh2_dispatch), bytes);
  }
#endif
#ifdef VITA_SH2_LINK
  if (!link_check) {
    // Called by linked exits (BL): the checks of sh2_dispatch before its
    // lookup, in the same order, leaving r0 and ip untouched, except the
    // BLOCK_LOOP test: only non-loop blocks link their exits, and with a
    // nonzero budget chain_cur is always the block running. Returns with
    // r1 = the decremented chain budget when the next block may be entered,
    // otherwise continues into the dispatcher (which then returns to C)
    // through BX LR: that consumes the return-stack entry of the BL, so the
    // dispatcher's return to C and the C returns after it stay predicted.
    static const u32 check[] = {
      0xe5971094u,  // ldr   r1,[r7,#148]  chain_budget
      0xe5972080u,  // ldr   r2,[r7,#128]  exitcount
      0xe2511001u,  // subs  r1,r1,#1
      0x3a000009u,  // blo   exit          budget was 0
      0xe1590002u,  // cmp   r9,r2
      0x2a000007u,  // bhs   exit          count reached
      0xe5972040u,  // ldr   r2,[r7,#64]   SR
      0xe5973060u,  // ldr   r3,[r7,#96]   pending interrupt level
      0xe20220f0u,  // and   r2,r2,#0xF0
      0xe1520003u,  // cmp   r2,r3
      0x3a000002u,  // blo   exit          interrupt acceptable
      0xe59720a0u,  // ldr   r2,[r7,#160]  memcycle_
      0xe3520000u,  // cmp   r2,#0
      0x012fff1eu,  // bxeq  lr            no memory cycles to account
      0xe597e090u,  // exit: ldr lr,[r7,#144]
      0xe12fff1eu,  // bx    lr
    };
    static_assert(sizeof(check) <= vitacode::Layout::Stubs, "link check fits the stub area");
    size_t at = 0;
#if defined(VITA_SH2_ARENA_DISPATCH)
    at = (size_t(reinterpret_cast<uintptr_t>(&sh2_dispatch_end) - reinterpret_cast<uintptr_t>(&sh2_dispatch)) + 31) & ~size_t(31);
#endif
    if (at + sizeof(check) > vitacode::Layout::Stubs) throw std::bad_alloc();
    link_check = code_base + vitacode::Layout::StubOffset + at;
    VitaSh2CodeWrite write(link_check, sizeof(check));
    memcpy(link_check, check, sizeof(check));
  }
#endif

  memset(LookupTable, 0, sizeof(LookupTable));
  memset(LookupTableRom, 0, sizeof(LookupTableRom));
  memset(LookupTableLow, 0, sizeof(LookupTableLow));
  for (auto &page : low_code_pages) page.clear();
  LookupTableC.clear();

  blockCount = 0;
  LastMakeBlock = 0;

  g_CompleBlock = dCode;
  for (int i = 0; i < NUMOFBLOCKS; i++ ) {
    g_CompleBlock[i].id = i;
  }
  return;
}

int CompileBlocks::opcodeIndex(u16 op)
{
  /*register*/ int i = 0;
  while (((op & opcode_list[i].mask) != opcode_list[i].bits) && opcode_list[i].mnem != 0) i++;
  return i;
}

void CompileBlocks::FindOpCode(u16 opcode, u8 * instindex)
{
  *instindex = opcodeIndex(opcode);
  return;
}

void CompileBlocks::BuildInstructionList()
{
  u32 optest;
  memset(dsh2_instructions, 0, sizeof(u8)*MAX_INSTSIZE);
  for (optest = 0; optest <= 0xFFFF; optest++) {
    FindOpCode(optest, &dsh2_instructions[optest]);
  }
}

void CompileBlocks::InvalidateLow(u32 address, u32 length) {
  if (!length) return;
  const u32 first = address & 0xfffff;
  if (length >= 0x100000 || static_cast<u64>(first) + length > 0x100000) {
    // Conservatively handle wraparound/bulk writes without overflowing bounds.
    memset(LookupTableLow, 0, sizeof(LookupTableLow));
    return;
  }
  const u32 last = first + length - 1;
  for (u32 p = first >> 12; p <= (last >> 12); ++p) {
    for (Block *b : low_code_pages[p]) {
      const u32 begin = b->b_addr & 0xfffff;
      const u32 end = (b->e_addr & 0xfffff) + 1;
      if (end < begin || (first <= end && last >= begin))
        LookupTableLow[begin >> 1] = nullptr;
    }
  }
}

/* Retire the block in a slot: remove it from every table that can return it. */
void CompileBlocks::EvictSlot(int slot) {
  if (g_CompleBlock[slot].b_addr != 0x00) {
    Block *old = &g_CompleBlock[slot];
    if ((old->b_addr & 0x0ff00000) == 0x00200000) {
      for (auto &page : low_code_pages) {
        for (auto it = page.begin(); it != page.end(); ) {
          if (*it == old) it = page.erase(it); else ++it;
        }
      }
    }
    
    if ((g_CompleBlock[slot].b_addr & 0xFF000000) == 0xC0000000) {
      // A recycled code slot may have entries for either CPU or an alias.
      for (auto it = LookupTableC.begin(); it != LookupTableC.end(); ) {
        if (it->second == &g_CompleBlock[slot]) it = LookupTableC.erase(it);
        else ++it;
      }
    }
    else {
      switch (g_CompleBlock[slot].b_addr & 0x0FF00000) {
      case 0x00000000:
        if (yabsys.emulatebios) {
          //return NULL; do nothing
        }
        else {
          LookupTableRom[(g_CompleBlock[slot].b_addr & 0x000FFFFF) >> 1] = NULL;
        }
        break;
      case 0x00200000:
        LookupTableLow[(g_CompleBlock[slot].b_addr & 0x000FFFFF) >> 1] = NULL;
        break;
      case 0x06000000:
        /*case 0x06100000:*/
#ifdef VITA_SH2_SMC_VARIANTS
        // An unpublished variant slot may share its PC with the published one.
        if (LookupTable[(g_CompleBlock[slot].b_addr & 0x000FFFFF) >> 1] != &g_CompleBlock[slot])
          break;
#endif
        SetHigh((g_CompleBlock[slot].b_addr & 0x000FFFFF) >> 1, NULL);
        //LOG("%d, %08X is removed due to overflow", blockCount, g_CompleBlock[slot].b_addr);
        break;
      default:
        break;
      }
    }
  }
  UnlinkSlot(slot);
  g_CompleBlock[slot].link_pc = 0;
}

#ifdef VITA_SH2_PACKED_CODE
void CompileBlocks::CodeRelease(int slot) {
  if (!code_bytes_[slot]) return;
  std::vector<int> &page = code_page_slots_[code_off_[slot] >> 12];
  page.erase(std::find(page.begin(), page.end(), slot));
  code_bytes_[slot] = 0;
}

/* Code room for a compile into slot: MAXBLOCKSIZE bytes at the cursor, after
 * evicting every other live block with code there (blocks start at most
 * MAXBLOCKSIZE before the room). */
void CompileBlocks::PlaceCode(int slot) {
  CodeRelease(slot);
  if (code_cursor_ + MAXBLOCKSIZE > vitacode::Layout::StubOffset) code_cursor_ = 0;
  const size_t lo = code_cursor_, hi = lo + MAXBLOCKSIZE;
  for (size_t p = lo >= 4096 ? (lo >> 12) - 1 : 0; p <= (hi - 1) >> 12; ++p) {
    const std::vector<int> page = code_page_slots_[p];
    for (int other : page) {
      if (code_off_[other] >= hi || code_off_[other] + code_bytes_[other] <= lo) continue;
      if (g_CompleBlock[other].b_addr) {
        EvictSlot(other);
        g_CompleBlock[other].b_addr = 0;
        smc_slot_[other].pc = 0;
      }
      CodeRelease(other);
    }
  }
  code_off_[slot] = u32(lo);
  code_bytes_[slot] = MAXBLOCKSIZE;
  code_page_slots_[lo >> 12].push_back(slot);
  g_CompleBlock[slot].code = VitaSh2CodeArena() + lo;
  code_cursor_ = hi;
}

/* The compile into slot succeeded: keep only its code (cache-line aligned). */
void CompileBlocks::PlacedCode(int slot) {
  code_bytes_[slot] = last_code_bytes_;
  code_cursor_ = code_off_[slot] + ((last_code_bytes_ + 31) & ~31u);
}
#endif

#include "../../vita/telemetry.h"
static bool Sh2MayWrite(u16 op);
Block * CompileBlocks::CompileBlock(u32 pc, addrs * ParentT = NULL)
{
  VT_SCOPE(VT_SH2_COMPILE);
  compile_count_++;
#ifdef VITA_STACK_PROFILE
  ++g_prof_compiles[(pc & 0x0ff00000) == 0x06000000 ? 0 : (pc & 0x0ff00000) == 0x00200000 ? 1
                    : (pc & 0xff000000) == 0xc0000000 ? 2 : 3];
  if (self_modify_block.count(pc)) ++g_prof_self_modify_reuse;
#endif

  auto block_index = self_modify_block.find(pc);
#ifdef VITA_SH2_SMC_VARIANTS
  const bool smc = (pc & 0x0FF00000) == 0x06000000 && ParentT && !debug_mode_ &&
                   (block_index != self_modify_block.end() || smc_pcs_.count(pc));
  if (smc) {
    if (block_index != self_modify_block.end()) {
      // The invalidated slot becomes a variant unless it was compiled before
      // the PC was tracked (no recorded source words).
      std::vector<int> &slots = smc_pcs_[pc];
      const int old = block_index->second;
      if (std::find(slots.begin(), slots.end(), old) == slots.end() && slots.size() < kSmcVariants)
        slots.push_back(old);
      self_modify_block.erase(block_index);
    }
#ifdef VITA_STACK_PROFILE
    { extern u32 g_prof_smc_pc[64], g_prof_smc_n[64], g_prof_smc_hit, g_prof_smc_miss;
      unsigned k = (pc >> 1) & 63; if (g_prof_smc_pc[k] != pc) { g_prof_smc_pc[k] = pc; g_prof_smc_n[k] = 0; } ++g_prof_smc_n[k];
      if (Block *reused = SmcReuse(pc, ParentT)) { ++g_prof_smc_hit; return reused; }
      ++g_prof_smc_miss; }
#else
    if (Block *reused = SmcReuse(pc, ParentT)) return reused;
#endif
    blockCount = SmcSlot(pc);
  } else
#endif
  if( block_index != self_modify_block.end()  ){
      blockCount = block_index->second ;
      self_modify_block.erase(pc);
      LOG( "hit! last_modified_block = %d, last_modified_pc = %08X",blockCount, pc );
  }else{
    blockCount = LastMakeBlock;
    LastMakeBlock = vitacode::Layout::NextSh2Block(LastMakeBlock);
  }
#ifdef VITA_SH2_SMC_VARIANTS
  smc_slot_[blockCount].pc = 0;  // overwritten below: no longer a variant
#endif
  
  EvictSlot(blockCount);
  g_CompleBlock[blockCount].b_addr = pc;
#ifdef VITA_SH2_PACKED_CODE
  PlaceCode(blockCount);
#endif


  //LOG("%d,%08X is compiled",blockCount,pc );
#ifdef VITA_SH2_MAC_REGIONS
  // MAC regions change native code size, and the baseline ends some blocks
  // on native size (max(native, template bytes) reaching MAXBLOCKSIZE). The
  // baseline pass fixes the extent; a block holding a MAC operation is then
  // recompiled with MAC regions to exactly that end, keeping the baseline
  // code whenever the second pass cannot reach it.
  mac_regions_ = false;
  forced_end_ = 0;
  if (EmmitCode(&g_CompleBlock[blockCount], ParentT) != 0) {
    return NULL;
  }
  if (!debug_mode_) {
    Block *page = &g_CompleBlock[blockCount];
    bool mac = false;
    for (u32 a = page->b_addr; a <= page->e_addr && !mac; a += 2)
      mac = sh2a9::RegisterRegion::IsMacOperation(MappedMemoryReadWord(a, NULL));
    if (mac) {
      const u32 end = page->e_addr;
      mac_regions_ = true;
      forced_end_ = end;
      const int result = EmmitCode(page, ParentT);
      mac_regions_ = false;
      forced_end_ = 0;
      if ((result != 0 || page->e_addr != end) && EmmitCode(page, ParentT) != 0)
        return NULL;
    }
  }
#else
  if (EmmitCode(&g_CompleBlock[blockCount], ParentT) != 0) {
    return NULL;
  }
#endif

#ifdef VITA_SH2_PACKED_CODE
  PlacedCode(blockCount);
#endif
  Block *created = &g_CompleBlock[blockCount];
  for (u32 a = created->b_addr; a <= created->e_addr; a += 2)   // e_addr: last (delay-slot) instruction
    if (Sh2MayWrite(MappedMemoryReadWord(a, NULL))) { created->flags |= BLOCK_MAY_WRITE; break; }
#ifdef VITA_SH2_SMC_VARIANTS
  if (smc) {
    SmcVariant &v = smc_slot_[blockCount];
    v.pc = pc;
    v.denied = spec_deny.count(pc) != 0;
    v.words = SmcSourceWords(pc, created->e_addr);
  }
#endif
  if ((created->b_addr & 0x0ff00000) == 0x00200000) {
    const u32 first = (created->b_addr & 0xfffff) >> 12;
    const u32 last = (created->e_addr & 0xfffff) >> 12;
    if (last < first) {
      for (auto &page : low_code_pages) page.push_back(created);
    } else {
      for (u32 p = first; p <= last; ++p) low_code_pages[p].push_back(created);
    }
  }

  return &g_CompleBlock[blockCount];
}

#ifdef VITA_SH2_SMC_VARIANTS
/* Every source word a compile at pc reads, given the block's end e_addr:
 * resident-loop scan (pc..pc+128), infinite-loop check (pc+2..pc+5), the
 * size check of the instructions after the end (e_addr+2..e_addr+5) and the
 * stack plan (to the first branch and its delay slot, at most 256 words). */
std::vector<u16> CompileBlocks::SmcSourceWords(u32 pc, u32 e_addr) {
  u32 end = std::max(pc + 130, e_addr + 6);  // exclusive
  for (u32 a = pc, k = 0; k < 256; ++k, a += 2) {
    const u16 o = MappedMemoryReadWord(a, NULL);
    const u8 dl = asm_list[dsh2_instructions[o]].delay;
    if ((dl != 0 && dl != 0xFF) || (o & 0xF0FF) == 0x400e || (o & 0xF0FF) == 0x4007 || k == 255) {
      end = std::max(end, a + 4);
      break;
    }
  }
  std::vector<u16> words;
  words.reserve((end - pc) / 2);
  for (u32 a = pc; a != end; a += 2) words.push_back(MappedMemoryReadWord(a, NULL));
  return words;
}

/* A recorded variant of pc whose source is current: registered as the owner
 * of its words exactly as its compile registered it, and returned for
 * publication. */
Block *CompileBlocks::SmcReuse(u32 pc, addrs *ParentT) {
  auto found = smc_pcs_.find(pc);
  if (found == smc_pcs_.end()) return nullptr;
  const bool denied = spec_deny.count(pc) != 0;
  std::vector<u16> &current = smc_words_;
  current.clear();
  for (int slot : found->second) {
    const SmcVariant &v = smc_slot_[slot];
    Block *b = &g_CompleBlock[slot];
    if (v.pc != pc || b->b_addr != pc || v.denied != denied) continue;
    if (current.size() < v.words.size()) {
      // Grow lazily; spans differ only by their tails. Cached and
      // cache-through high work RAM (pages 0x600-0x610) read as HighWram.
      for (u32 a = pc + 2 * u32(current.size()); current.size() < v.words.size(); a += 2)
        current.push_back((a >> 29) <= 1 && ((a >> 16) & 0xFFF) >= 0x600 && ((a >> 16) & 0xFFF) <= 0x610
                              ? T2ReadWord(HighWram, a & 0xFFFFF) : MappedMemoryReadWord(a, NULL));
    }
    if (!std::equal(v.words.begin(), v.words.end(), current.begin())) continue;
    for (u32 a = b->b_addr; a <= b->e_addr; a += 2) {
      const u32 keep = adress_mask(a);
      MarkCode(keep);
      ParentT[keep].push_back(adress_mask(pc));
      ParentT[keep].unique();
    }
    return b;
  }
  return nullptr;
}

/* The slot for a new compile of self-modified pc: a fresh one while pc has
 * fewer than kSmcVariants, else its variants in turn. */
int CompileBlocks::SmcSlot(u32 pc) {
  std::vector<int> &slots = smc_pcs_[pc];
  slots.erase(std::remove_if(slots.begin(), slots.end(),
                             [&](int s) { return smc_slot_[s].pc != pc && g_CompleBlock[s].b_addr != pc; }),
              slots.end());
  if (slots.size() < kSmcVariants) {
    const int slot = LastMakeBlock;
    LastMakeBlock = vitacode::Layout::NextSh2Block(LastMakeBlock);
    slots.push_back(slot);
    return slot;
  }
  const int slot = slots.front();
  slots.erase(slots.begin());
  slots.push_back(slot);
  return slot;
}
#endif

#ifdef VITA_SH2_LINK
/* Exit into a statically known high-RAM PC, in place of BLOCK_RETURN once the
 * exit has stored the PC (held in pc_reg) and the count. Unlinked, word 0 is
 * BLOCK_RETURN. PatchLink makes word 0 BL link_check (sh2_dispatch's checks;
 * r1 = decremented budget), words 1-2 the target Block and word 9 a branch to
 * its body past the reload of r8/r9. The target is entered only while its
 * link_pc equals the PC, i.e. while sh2_dispatch's lookup would return it;
 * otherwise word 10 continues into sh2_dispatch.
 *   0 ldr pc,[r7,#144] | bl link_check    6 str r1,[r7,#148]  chain_budget
 *   1 movw r2,#block                      7 str r2,[r7,#156]  chain_cur
 *   2 movt r2,#block                      8 mov r8,pc_reg
 *   3 ldr r3,[r2,#28]  link_pc            9 b 10 | b target body
 *   4 cmp r3,pc_reg                      10 ldr pc,[r7,#144]
 *   5 bne 10                                                               */
static_assert(PROLOGSIZE == 16, "linked exits enter blocks past push, mov r7 and the r8/r9 loads");
static constexpr size_t kLinkStubBytes = 11 * 4;
static constexpr u32 kBlockReturn = 0xe597f090u;  // ldr pc,[r7,#144]
static void EmitLinkStub(u8 *p, unsigned pc_reg) {
  const u32 words[] = {kBlockReturn, 0xe3002000u, 0xe3402000u, 0xe592301cu, 0xe1530000u | pc_reg,
                       0x1a000003u, 0xe5871094u, 0xe587209cu, 0xe1a08000u | pc_reg, 0xeaffffffu, kBlockReturn};
  static_assert(sizeof(words) == kLinkStubBytes, "stub layout");
  memcpy(p, words, sizeof(words));
}
static u32 ArmBranch(u32 opcode, const void *from, const void *to) {
  return opcode | ((u32(reinterpret_cast<const u8 *>(to) - reinterpret_cast<const u8 *>(from) - 8) >> 2) & 0xffffffu);
}
void CompileBlocks::PatchLink(int slot, int exit, const Block *target) {
  u32 *w = reinterpret_cast<u32 *>(dCode[slot].code + link_out[slot][exit].offset);
  const u32 b = u32(reinterpret_cast<uintptr_t>(target));
  w[1] = 0xe3002000u | ((b & 0xf000u) << 4) | (b & 0xfffu);
  w[2] = 0xe3402000u | ((b >> 12) & 0xf0000u) | ((b >> 16) & 0xfffu);
  w[9] = ArmBranch(0xea000000u, &w[9], target->code + PROLOGSIZE);
  w[0] = ArmBranch(0xeb000000u, &w[0], link_check);
}
void CompileBlocks::Relink(Block *b) {
  const auto range = link_in.equal_range(b->b_addr);
  for (auto it = range.first; it != range.second; ++it) {
    const int slot = int(it->second >> 1), exit = int(it->second & 1);
    VitaSh2CodeWrite write(dCode[slot].code + link_out[slot][exit].offset, kLinkStubBytes);
    PatchLink(slot, exit, b);
  }
}
#else
void CompileBlocks::Relink(Block *) {}
void CompileBlocks::PatchLink(int, int, const Block *) {}
#endif
void CompileBlocks::UnlinkSlot(int slot) {
  for (u32 k = 0; k < 2; ++k) {
    LinkExit &e = link_out[slot][k];
    if (!e.offset) continue;
    const auto range = link_in.equal_range(e.target);
    for (auto it = range.first; it != range.second; ++it)
      if (it->second == u32(slot) * 2 + k) { link_in.erase(it); break; }
    e = {0, 0};
  }
}

void CompileBlocks::ShowStatics() {
  //LOG("Compile\t%d\t%d\t%d\n", compile_count_, exec_count_, remove_count_);
  compile_count_ = 0;
  exec_count_ = 0;
  remove_count_ = 0;
}

// memo DirectMemoryAccess
// MOVLI,MOVWI


void CompileBlocks::opcodePass(x86op_desc *op, u16 opcode, u8 *ptr)
{
#if defined(AARCH64)
  // multiply source and dest regions by 4 (size of register) 

  if (*(op->src) != 0xFF) // C
  {
    *(u32*)(ptr + *(op->src)) |= ((u32)((opcode >> 4) & 0xf) << (2+5));
  }

  if (*(op->dest) != 0xFF) // B
  {
    *(u32*)(ptr + *(op->dest)) |= ((u32)((opcode >> 8) & 0xf) << (2+5)) ;
  }

  if (*(op->off1) != 0xFF){
    *(u32*)(ptr + *(op->off1)) |= ((u32)(opcode & 0xf) << 5) ;
  }

  if (*(op->imm) != 0xFF){
    *(u32*)(ptr + *(op->imm)) |= ((u32)opcode & 0xff)<<5;
  }

  if (*(op->off3) != 0xFF) {
    *(u32*)(ptr + *(op->off3)) |= ((u32)opcode & 0xfff) << 5;
  }

#else

  if (*(op->src) != 0xFF) // C
    *(ptr + *(op->src)) = (u8)(((opcode >> 4) & 0xf) << 2);

  if (*(op->dest) != 0xFF) // B
    *(ptr + *(op->dest)) = (u8)(((opcode >> 8) & 0xf) << 2);

  if (*(op->off1) != 0xFF)
    *(ptr + *(op->off1)) = (u8)(opcode & 0xf);

  if (*(op->imm) != 0xFF)
    *(ptr + *(op->imm)) = (u8)(opcode & 0xff);

#if _WINDOWS
  if (*(op->off3) != 0xFF)
    *(u16*)(ptr + *(op->off3)) = (u16)(opcode & 0xfff);
#else  
  if (*(op->off3) != 0xFF) {
    *(ptr + *(op->off3)) = (u8)((opcode >> 8) & 0x0f);
    *(ptr + *(op->off3) + 4) = (u8)(opcode & 0xff);
}
#endif  

#endif
}

#define Y_MAX(a, b) ((a) > (b) ? (a) : (b))
#define Y_MIN(a, b) ((a) < (b) ? (a) : (b))

#ifdef VITA_SH2_RAM_STORES
// Compile-time switch; the startup differential test compiles both forms.
bool g_sh2_region_stores = true;
#endif
#ifdef VITA_SH2_DEFER_SEPARATORS
// Templates that read the live PC (r8) or cycle (r9) register mid-block:
// PC-relative MOV.W/MOV.L/MOVA, TRAPA and SLEEP (dynalib_arm.s). Branches
// and delay-slot forms (delay != 0) are handled by the caller.
static inline bool Sh2TemplateReadsPcOrCount(u16 op) {
  return (op & 0xF000) == 0x9000 || (op & 0xF000) == 0xD000 ||
         (op & 0xFF00) == 0xC700 || (op & 0xFF00) == 0xC300 || op == 0x001B;
}
// Compile-time switch; the startup differential test compiles both forms.
bool g_sh2_defer_separators = true;
// ARMv7-A A8.8.5 ADD (immediate, ARM) encoding A1, 8-bit immediate chunks.
static inline size_t Sh2EmitAddImm(u8 *ptr, unsigned reg, u32 value) {
  size_t bytes = 0;
  while (value) {
    const u32 chunk = value > 255 ? 255 : value;
    const u32 word = 0xE2800000u | (reg << 16) | (reg << 12) | chunk;
    memcpy(ptr + bytes, &word, 4);
    bytes += 4; value -= chunk;
  }
  return bytes;
}
#endif
#ifdef VITA_SH2_STACK_SPEC
// Per-instruction R15 offset from its value at block entry (see
// RegisterRegion stack speculation). Valid only while every instruction so far
// has a tracked or no effect on R15.
struct Sh2StackPlan { u32 start = 0; bool active = false; std::vector<int> off; std::vector<u8> valid; };
static bool Sh2StackPlanLookup(void *ctx, uint32_t pc, int *off) {
  const auto *plan = static_cast<const Sh2StackPlan *>(ctx);
  if (!plan->active || pc < plan->start) return false;
  const u32 index = (pc - plan->start) >> 1;
  if (index >= plan->valid.size() || !plan->valid[index]) return false;
  *off = plan->off[index];
  return true;
}
#endif
// Conservative: true for every SH-2 instruction that can write memory
// (SH-1/SH-2 Programming Manual instruction tables) and for anything not
// decoded as a known instruction (exceptions push SR/PC).
static bool Sh2MayWrite(u16 op) {
  const unsigned hi = op >> 12, lo = op & 15;
  switch (hi) {
    case 0x0: return (lo >= 4 && lo <= 6) ||                    // MOV.x Rm,@(R0,Rn)
                     !(lo == 2 || lo == 3 || lo == 7 || lo == 8 || lo == 9 || lo == 0xA ||
                       lo == 0xB || lo == 0xC || lo == 0xD || lo == 0xE || lo == 0xF); // others: STC/STS/branch/MOV.x @(R0,Rm)/MAC.L/...
    case 0x1: return true;                                      // MOV.L Rm,@(disp,Rn)
    case 0x2: return lo <= 2 || (lo >= 4 && lo <= 6) || lo == 3; // MOV.x Rm,@Rn / @-Rn; 3 undefined
    case 0x3: return lo == 1 || lo == 9;                        // undefined
    case 0x4: {                                                 // everything but these writes or is undefined
      if (lo == 0xF) return false;                              // MAC.W @Rm+,@Rn+
      switch (op & 0xFF) {
        case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
        case 0x0A: case 0x0B: case 0x0E: case 0x10: case 0x11: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1E: case 0x20: case 0x21: case 0x24: case 0x25:
        case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E:
          return false;
        default: return true;                                   // STS.L/STC.L @-Rn, TAS.B, undefined
      }
    }
    case 0x8: { const unsigned s = (op >> 8) & 15; return s == 0 || s == 1 || !(s == 4 || s == 5 || s == 8 || s == 9 || s == 0xB || s == 0xD || s == 0xF); }
    case 0xC: { const unsigned s = (op >> 8) & 15; return s <= 3 || s == 0xD || s == 0xE || s == 0xF; } // MOV.x R0,@(d,GBR), TRAPA, AND/XOR/OR.B
    case 0xF: return true;                                      // undefined on SH-2
    default: return false;                                      // 5,6,7,9,A,B,D,E: no memory writes
  }
}
int CompileBlocks::EmmitCode(Block *page, addrs * ParentT )
{
  VitaSh2CodeWrite code_write(page->code, MAXBLOCKSIZE);
  int i, j, jmp = 0, count = 0;
  u16 op, temp;
  u32 addr = page->b_addr;
  u32 start_addr = page->b_addr;
  u8 *ptr, *startptr;
  u32 instruction_counter = 0;
  u32 write_memory_counter = 0;
  u32 calsize;
  std::unordered_map<u32, uintptr_t> addr_map;

  startptr = ptr = page->code;
#ifdef VITA_SH2_LINK
  // Static exits into high RAM (link stubs), registered once the block is final.
  LinkExit links[2];
  unsigned link_count = 0;
  bool branch_exit = false;  // the block ends in a branch: the epilogue is unreachable
  auto link_stub = [&](u8 *p, unsigned pc_reg, u32 target) {
    EmitLinkStub(p, pc_reg);
    if ((target & 0xFFF00001u) == 0x06000000u && link_count < 2)
      links[link_count++] = {target, u32(p - startptr)};
  };
#endif

  i = 0;
  j = 0;
  count = 0;  
  memset((void*)ptr,0,sizeof(char)*MAXBLOCKSIZE);
  memcpy((void*)ptr, (void*)prologue, PROLOGSIZE);
  ptr += PROLOGSIZE;
#ifdef VITA_SH2_STACK_SPEC
  // Plan R15 over the block's straight-line instructions (to the branch and
  // its delay slot) and validate R15 once here, before anything executes.
  Sh2StackPlan stack_plan;
  if (sh2a9::RegisterRegion::stack_spec_enabled && !debug_mode_ && ParentT && g_sh2_region_stores && LowWram && HighWram &&
      (start_addr & 0x0FF00000) == 0x06000000 && !spec_deny.count(start_addr)) {
    stack_plan.start = start_addr;
    int off = 0; bool valid = true; unsigned loads = 0, stores = 0;
    // Byte extents [lo,hi) of every speculative access (and [slo,shi) of the
    // stores) relative to R15 at entry; validated below exactly as planned.
    int lo = 0, hi = 4, slo = 1 << 20, shi = -(1 << 20);
    auto count = [&](u16 op) {
      const auto acc = sh2a9::RegisterRegion::SpecDecode(op);
      if (!valid || !acc.ok || !sh2a9::RegisterRegion::SpecOffsetOk(acc, off + acc.disp)) return;
      const int o = off + acc.disp, e = o + int(acc.width);
      lo = std::min(lo, o & ~3); hi = std::max(hi, (e + 3) & ~3);
      if (acc.store) { ++stores; slo = std::min(slo, o & ~3); shi = std::max(shi, (e + 3) & ~3); }
      else ++loads;
    };
    for (u32 a = start_addr, k = 0; k < 256; ++k, a += 2) {
      const u16 o = MappedMemoryReadWord(a, NULL);
      stack_plan.valid.push_back(valid); stack_plan.off.push_back(off);
      count(o);
      int delta;
      const int effect = sh2a9::RegisterRegion::R15Effect(o, &delta);
      if (effect == 1) { off += delta; if (off <= -512 || off >= 512) valid = false; }
      else if (effect == 2) valid = false;
      const u8 dl = asm_list[dsh2_instructions[o]].delay;
      if (dl != 0 && dl != 0xFF) {                // branch: include its delay slot, then stop
        if (dl != 1 && dl != 5) {
          const u16 slot = MappedMemoryReadWord(a + 2, NULL);
          stack_plan.valid.push_back(valid); stack_plan.off.push_back(off);
          count(slot);
        }
        break;
      }
      if ((o & 0xF0FF) == 0x400e || (o & 0xF0FF) == 0x4007) break; // LDC SR ends the block
    }
    // Entry check ~8 words (+7 with stores); each speculative access saves
    // the ~6-word load guard or ~13-word store guard.
    stack_plan.active = loads + 2 * stores >= 2 && (!stores || (shi - slo <= 1020 && slo - lo <= 1020));
#ifdef VITA_STACK_PROFILE
    ++g_prof_spec[0]; if (stack_plan.active) { ++g_prof_spec[1]; g_prof_spec[2] += loads + stores; }
#endif
    if (stack_plan.active) {
      std::vector<u32> w;
      std::vector<size_t> bail;
      // ADD/SUB rd,rn,#v for a multiple of 4 below 1024 (imm8 ROR 30).
      auto add = [&](unsigned rd, unsigned rn, int v) {
        if (!v && rd == rn) return;
        const u32 m = u32(v < 0 ? -v : v) >> 2;
        w.push_back((v < 0 ? 0xe2400f00u : 0xe2800f00u) | (rn << 16) | (rd << 12) | m);
      };
      w.push_back(0xe597003cu);                    // LDR r0,[r7,#60]      R15
      w.push_back(0xe220a406u);                    // EOR r10,r0,#0x06000000
      add(10, 10, lo);                             // r10 = offset of R15+lo
      // R15+lo in [0x06000000,0x060FF000) and R15 4-aligned (lo is): every
      // planned access lies inside high work RAM (hi-lo <= 2044).
      w.push_back(0xe1a0c16au);                    // MOV ip,r10,ROR #2
      w.push_back(0xe35c0bffu);                    // CMP ip,#0x3FC00
      bail.push_back(w.size()); w.push_back(0x20000000u); // BHS bail
      if (stores) {                                // KiBs of [R15+slo,R15+shi) own no code
        add(14, 10, slo - lo);                     // lr = offset of first store byte
        w.push_back(0xe597c08cu);                  // LDR ip,[r7,#140]     code_pages
        w.push_back(0xe7dc052eu);                  // LDRB r0,[ip,lr,LSR #10]
        add(14, 14, shi - slo - 4);                // lr = offset of the last store word
        w.push_back(0xe7dce52eu);                  // LDRB lr,[ip,lr,LSR #10]
        w.push_back(0xe190e00eu);                  // ORRS lr,r0,lr
        bail.push_back(w.size()); w.push_back(0x10000000u); // BNE bail
      }
      w.push_back(0xe597c088u);                    // LDR ip,[r7,#136]     HighWram
      w.push_back(0xe08cb00au);                    // ADD r11,ip,r10
      add(11, 11, -lo);                            // host address of R15
      const size_t skip = w.size(); w.push_back(0);
      const size_t target = w.size();
      for (size_t at : bail)
        w[at] = w[at] | 0x0a000000u | (u32(int32_t(target) - int32_t(at) - 2) & 0xffffffu);
      w.push_back(0xe5878084u);                    // STR r8,[r7,#132]     spec_bail = block PC
      w.push_back(0xe5878058u);                    // STR r8,[r7,#88]      PC unchanged
      w.push_back(0xe587905cu);                    // STR r9,[r7,#92]      count unchanged
      w.push_back(0xe8bd8ff8u);                    // POP {r3-r11,pc}
      w[skip] = 0xea000000u | (u32(int32_t(w.size()) - int32_t(skip) - 2) & 0xffffffu); // B body
      memcpy(ptr, w.data(), w.size() * 4);
      ptr += w.size() * 4;
    }
  }
#endif
  size_t scheduling_size = PROLOGSIZE;
  u8 *scheduling_cursor = ptr;
  // Bytes the r12-base region emission would have added (RegisterRegion::
  // legacy_words); every size decision below uses used(), so block and
  // region boundaries are those of that emission.
  size_t legacy_extra = 0;
#ifdef VITA_SH2_BLOCK_COLD
  // Regions' slow edges, placed after the block's exits (see FinishHot).
  struct ColdPart { unsigned hot_base; std::vector<uint32_t> words;
                    std::vector<sh2a9::RegisterRegion::Fixup> to_cold, to_hot; };
  std::vector<ColdPart> cold_parts;
  size_t cold_bytes = 0;
  auto used = [&]() { return size_t(ptr - startptr) + cold_bytes + legacy_extra; };
#else
  auto used = [&]() { return size_t(ptr - startptr) + legacy_extra; };
#endif
#ifdef VITA_SH2_DEFER_SEPARATORS
  // Per-instruction "add r8,#2; add r9,#cycles" separators of plain templates
  // are accumulated and applied once, immediately before any code that can
  // observe r8/r9 (listed templates, branches, regions, epilogue). Blocks are
  // straight-line on ARM32 (internal jumps are AArch64-only), so every exit
  // and observer is preceded by a flush. scheduling_size keeps the baseline
  // byte accounting, so block boundaries are unchanged.
  u32 defer_pc = 0, defer_cycles = 0;
  auto flush_deferred = [&]() {
    size_t bytes = Sh2EmitAddImm(ptr, 8, defer_pc);
    bytes += Sh2EmitAddImm(ptr + bytes, 9, defer_cycles);
    ptr += bytes;
    scheduling_size -= bytes; /* not baseline bytes; next loop top adds them */
    defer_pc = defer_cycles = 0;
  };
  const bool defer_enabled = !debug_mode_ && g_sh2_defer_separators;
#endif
  int MaxSize = 0;

  void * nomal_seperator;
  u32 nomal_seperator_size;
  u8 nomal_seperator_counter_offset;
  void * delay_seperator;
  u32 delay_seperator_size;
  u8 delayslot_seperator_counter_offset;

  if (debug_mode_) {
    nomal_seperator = (void*)seperator_d_normal;
    nomal_seperator_size = SEPERATORSIZE_DEBUG;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET_DEBUG;
    delay_seperator = (void*)seperator_d_delay;
    delay_seperator_size = SEPERATORSIZE_DELAYD_DEBUG;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET_DEBUG;
  }
  else {
    nomal_seperator = (void*)seperator_normal;
    nomal_seperator_size = SEPERATORSIZE_NORMAL;
    nomal_seperator_counter_offset = NORMAL_CLOCK_OFFSET;
    delay_seperator = (void*)seperator_delay_slot;
    delay_seperator_size = SEPERATORSIZE_DELAY_SLOT;
    delayslot_seperator_counter_offset = DALAY_CLOCK_OFFSET;
  }
  
  page->flags = 0;
  page->poll = 0;
  page->poll_step = 0;
  bool resident_emitted = false;
#if defined(VITA_SH2_RESIDENT_LOOPS) && !defined(AARCH64) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
  // Only register-only loops on ordinary cached work RAM. ExecuteBlock also
  // limits these to one iteration while the asynchronous SCSP worker exists:
  // its main interrupt callback can trigger SCU DMA and source invalidation.
  if (!debug_mode_ && ((start_addr >> 20) == 2 || (start_addr >> 20) == 0x60)) {
    std::vector<u16> body;
    size_t baseline_size = PROLOGSIZE;
    u32 progress = 0;
    for (unsigned index = 0; index <= 64 && index + 2 < MAXINSTRCNT; ++index) {
      if ((start_addr & 0xfffff) + index * 2 > 0xffffe) break;
      const u16 candidate = MappedMemoryReadWord(start_addr + index * 2, nullptr);
      const int decoded = dsh2_instructions[candidate];
      if (!asm_list[decoded].func) break;
      baseline_size += *asm_list[decoded].size + nomal_seperator_size;
      if ((candidate >> 8) == 0x89 || (candidate >> 8) == 0x8b) {
        if (!progress || baseline_size + DELAYJUMPSIZE + EPILOGSIZE >= MAXBLOCKSIZE) break;
        const auto code = sh2a9::RegisterRegion::ResidentLoop(body, candidate);
        if (code.empty() || PROLOGSIZE + code.size() * 4 + EPILOGSIZE >= MAXBLOCKSIZE) break;
        memcpy(ptr, code.data(), code.size() * 4);
        ptr += code.size() * 4;
        for (unsigned owner = 0; owner <= index; ++owner) {
#ifdef SET_DIRTY
          if (ParentT) {
            auto &owners = ParentT[adress_mask(start_addr + owner * 2)];
            MarkCode(adress_mask(start_addr + owner * 2));
            owners.push_back(adress_mask(start_addr));
            owners.unique();
          }
#endif
          const u16 source = owner < body.size() ? body[owner] : candidate;
          ++asm_list[dsh2_instructions[source]].build_count;
          instrSize[blockCount][count++] = owner ? 0 : code.size() * 4;
        }
        instruction_counter += index + 1;
        write_memory_counter = progress;
        addr = start_addr + (index + 1) * 2;
        page->flags |= BLOCK_RESIDENT_LOOP;
        resident_emitted = true;
        break;
      }
      if (!sh2a9::RegisterRegion::Supports(candidate) ||
          asm_list[decoded].delay || asm_list[decoded].cycle != 1 ||
          baseline_size + EPILOGSIZE >= MAXBLOCKSIZE) break;
      body.push_back(candidate);
      progress += asm_list[decoded].write_count;
    }
  }
#endif
  
#ifdef BUILD_INFO  
  if( show_code_ ) LOG("*********** start block %08X *************\n", addr );
#endif
  //LOG("Compile %08X\n", addr );
  //MaxSize = MAXBLOCKSIZE - MAXINSTRSIZE- delay_seperator_size - SEPERATORSIZE_DELAY_AFTER - nomal_seperator_size - EPILOGSIZE;
  //while (ptr - startptr < MaxSize) {
#if defined(VITA_SH2_A9_REGIONS) && !defined(AARCH64)
  auto make_region = [&]() {
    sh2a9::RegisterRegion region(reinterpret_cast<uintptr_t>(LowWram),
                                reinterpret_cast<uintptr_t>(HighWram)
#ifdef VITA_SH2_RAM_LOADS
                                , true
#endif
                                );
#if defined(VITA_SH2_GBR_LOADS) && defined(VITA_SH2_MAC_REGIONS)
    // Not in the MAC-region recompile (measured: Sonic Jam's MAC blocks ran
    // about 3% slower overall with GBR loads admitted there).
    region.gbr_loads = !mac_regions_;
#endif
#ifdef VITA_SH2_RAM_STORES
    // Only high-RAM compiles maintain LookupParentTable, hence code_pages.
    if (ParentT && g_sh2_region_stores)
      region.code_pages = reinterpret_cast<uintptr_t>(code_pages);
#endif
#ifdef VITA_SH2_STACK_SPEC
    if (stack_plan.active) { region.spec_plan = Sh2StackPlanLookup; region.spec_ctx = &stack_plan; }
#endif
#ifdef VITA_SH2_ONCHIP_DIRECT
    region.onchip_write[0] = reinterpret_cast<uintptr_t>(&OnchipWriteByte);
    region.onchip_write[1] = reinterpret_cast<uintptr_t>(&OnchipWriteWord);
    region.onchip_write[2] = reinterpret_cast<uintptr_t>(&OnchipWriteLong);
#endif
#if defined(VITA_SH2_MAC_REGIONS) && defined(VITA_SH2_MACL_WRAM)
    region.macl_helper = reinterpret_cast<uintptr_t>(&sh2_macl_region);
#endif
#if defined(VITA_SH2_MAC_REGIONS) && defined(VITA_SH2_MACW_WRAM)
    region.macw_helper = reinterpret_cast<uintptr_t>(&sh2_macw_region);
#endif
    return region;
  };
  auto region_admits = [&](const sh2a9::RegisterRegion &region, u16 candidate, u32 at, u32 literal_pc) {
    const int decoded = dsh2_instructions[candidate];
    (void)at;
    return !(!region.CanEmit(candidate, literal_pc) ||
#ifndef VITA_SH2_IMMEDIATE_LOGIC
            sh2a9::RegisterRegion::IsImmediateLogic(candidate) ||
#endif
#ifdef VITA_SH2_MAC_REGIONS
            (!mac_regions_ && sh2a9::RegisterRegion::IsMacOperation(candidate)) ||
            (forced_end_ && at > forced_end_) ||
#else
            sh2a9::RegisterRegion::IsMacOperation(candidate) ||
#endif
            // Only non-branch templates. The old separator charges the table
            // cost, except after delay-0xFF (no interrupt) templates, whose
            // separator keeps its default single state.
            (asm_list[decoded].delay != 0 && asm_list[decoded].delay != 0xFF) ||
            (asm_list[decoded].delay ? 1u : unsigned(asm_list[decoded].cycle)) !=
              sh2a9::RegisterRegion::Cycles(candidate));
  };
#endif
  while (!resident_emitted) {
    scheduling_size += ptr - scheduling_cursor;
    scheduling_cursor = ptr;
    // Compact regions can fit far more guest instructions than old templates;
    // retain room in the per-instruction metadata, including a delay slot.
    if (count + 2 >= MAXINSTRCNT) break;
#ifdef VITA_SH2_MAC_REGIONS
    if (forced_end_ && addr > forced_end_) break; // the baseline pass ended here
#endif
    // translate the opcode and insert code
    op = MappedMemoryReadWord(addr, NULL);
#ifdef SET_DIRTY
    if (ParentT) {
      u32 keepaddr = adress_mask(addr);
      MarkCode(keepaddr);
      ParentT[keepaddr].push_back(adress_mask(start_addr));
      ParentT[keepaddr].unique();
    }
#endif

    addr_map[addr] = (uintptr_t)ptr;

    i = dsh2_instructions[op];
    if (asm_list[i].func == 0) {
      LOG("bad instruction code @ PC=%08X code=%04X\n", addr, op );
      return -1;  // bad instruction code
    }

#if defined(VITA_SH2_A9_REGIONS) && !defined(AARCH64)
    // SH7604 instruction tables: admitted operations take one cycle, do not
    // change the interrupt mask or directly access device memory. Q/T remain
    // resident until helper-capable loads or region exit. They cannot observe intermediate
    // PC/cycles. Proven aligned cached-RAM loads retain the callback's data
    // layout and zero extra memory-cycle cost without exiting the region.
    // Branches and their delay slots stay in the existing lowering below.
    // Each source can require a dirty eviction and two-word constant load.
    // Reserve materialization of 16 deferred constants (MOVW/MOVT + STR),
    // seven resident stores, one SR store, PC/cycle updates and the block epilogue.
    if (!debug_mode_ && (sh2a9::RegisterRegion::Supports(op)
#ifndef VITA_SH2_IMMEDIATE_LOGIC
        && !sh2a9::RegisterRegion::IsImmediateLogic(op)
#endif
#ifdef VITA_SH2_MAC_REGIONS
        && (mac_regions_ || !sh2a9::RegisterRegion::IsMacOperation(op))
#else
        && !sh2a9::RegisterRegion::IsMacOperation(op)
#endif
#ifdef VITA_SH2_RAM_LOADS
        || (LowWram && HighWram && sh2a9::RegisterRegion::IsIndirectLoad(op))
        || (LowWram && HighWram && sh2a9::RegisterRegion::disp_loads_enabled &&
            (sh2a9::RegisterRegion::IsDisplacementLoad(op) ||
             sh2a9::RegisterRegion::IsIndexedLoad(op)))
#ifdef VITA_SH2_GBR_LOADS
        || (LowWram && HighWram && sh2a9::RegisterRegion::IsGbrLoad(op)
#ifdef VITA_SH2_MAC_REGIONS
            && !mac_regions_
#endif
           )
#endif
#endif
#ifdef VITA_SH2_RAM_STORES
        || (LowWram && HighWram && ParentT && g_sh2_region_stores &&
            sh2a9::RegisterRegion::IsStore(op))
#endif
#ifdef VITA_SH2_PC_LOADS
        || (LowWram && HighWram && sh2a9::RegisterRegion::IsPcLoad(op))
#endif
#if defined(VITA_SH2_MAC_REGIONS) && defined(VITA_SH2_RAM_LOADS)
        || (mac_regions_ && LowWram && HighWram && sh2a9::RegisterRegion::IsMacLong(op))
        || (mac_regions_ && LowWram && HighWram && sh2a9::RegisterRegion::IsMacWord(op))
#endif
#ifdef VITA_SH2_STACK_SPEC
        || (stack_plan.active && sh2a9::RegisterRegion::IsStackPr(op))
#endif
        )) {
#ifdef VITA_SH2_DEFER_SEPARATORS
      if (defer_pc) flush_deferred();
#endif
      const size_t remaining = MAXBLOCKSIZE - used();
      // At most 64 MAC operations: their extra cycles fit in one immediate.
#ifdef VITA_SH2_MAC_REGIONS
      constexpr size_t reserve = 59 * sizeof(u32) + EPILOGSIZE + 1;
#else
      constexpr size_t reserve = 58 * sizeof(u32) + EPILOGSIZE + 1;
#endif
      const size_t limit = remaining > reserve ?
        Y_MIN(size_t(MAXINSTRCNT - count - 2),
          Y_MIN(size_t(64), (remaining - reserve) / (7 * sizeof(u32)))) : 0;
      if (limit == 0) break;
      sh2a9::RegisterRegion region = make_region();
      const int first = count;
      while (region.instructions < limit) {
        const u16 candidate = MappedMemoryReadWord(addr, NULL);
        const int decoded = dsh2_instructions[candidate];
#ifdef VITA_SH2_PC_LOADS
        const u32 literal_pc = addr;
#else
        const u32 literal_pc = 0;
#endif
        if (!region_admits(region, candidate, addr, literal_pc))
          break;
        if ((region.PendingWords() + region.legacy_words + region.MaxEmissionWords(candidate)) * sizeof(u32)
              + reserve > remaining)
          break;
        const size_t old_bytes = *asm_list[decoded].size + nomal_seperator_size;
        if (scheduling_size + old_bytes + EPILOGSIZE >= MAXBLOCKSIZE) break;
        region.Emit(candidate, literal_pc);
        scheduling_size += old_bytes;
#ifdef SET_DIRTY
        if (ParentT) {
          auto &owners = ParentT[adress_mask(addr)];
          MarkCode(adress_mask(addr));
          owners.push_back(adress_mask(start_addr));
          owners.unique();
        }
#endif
        ++instruction_counter;
        ++asm_list[decoded].build_count;
        // This is also the legacy loop detector's progress marker (ADD/DT
        // count, not just memory writes). Omitting it can skip real loops.
        write_memory_counter += asm_list[decoded].write_count;
        instrSize[blockCount][count++] = 0;
        addr += 2;
      }
      if (region.instructions) {
#ifdef VITA_SH2_BLOCK_COLD
        region.FinishHot(); // Canonical state/PC/cycles before ANY old template.
        if (!region.cold.empty()) {
          cold_parts.push_back({unsigned((ptr - startptr) / 4), std::move(region.cold),
                                std::move(region.to_cold), std::move(region.to_hot)});
          cold_bytes += cold_parts.back().words.size() * sizeof(u32);
          legacy_extra += sizeof(u32);  // the in-line layout's branch over its cold code
        }
#else
        region.Finish(); // Canonical state/PC/cycles before ANY old template.
#endif
        const size_t bytes = region.code.size() * sizeof(u32);
        legacy_extra += size_t(region.legacy_words) * sizeof(u32);
        memcpy(ptr, region.code.data(), bytes);
        ptr += bytes;
        scheduling_cursor = ptr;
        instrSize[blockCount][first] = bytes;
        continue;
      }
    }
#endif

    // Ordinary and post-increment indirect MOV loads are specialized. Branch delay slots
    // continue to use their original templates, as does per-instruction debug.
    const void *regular_code = reinterpret_cast<const void *>(asm_list[i].func);
    size_t regular_size = *asm_list[i].size;
    bool lowered_load = false;
#if defined(VITA_SH2_RAM_LOADS) && !defined(AARCH64)
    std::vector<uint32_t> ram_load;
    if (!debug_mode_ && asm_list[i].delay == 0 && LowWram && HighWram &&
        sh2a9::RegisterRegion::IsIndirectLoad(op)) {
      ram_load = sh2a9::RamLoad((op >> 8) & 15, (op >> 4) & 15,
        1u << (op & 3), reinterpret_cast<uintptr_t>(LowWram),
        reinterpret_cast<uintptr_t>(HighWram), (op & 4) != 0);
      regular_code = ram_load.data();
      regular_size = ram_load.size() * sizeof(uint32_t);
      lowered_load = true;
    }
#endif
    // CheckSize
    u8 delay = asm_list[i].delay;
#if defined(AARCH64)
    if ( delay == 0 || delay == 0xFF) {
      calsize  = used() + regular_size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = used() + *asm_list[i].size + nomal_seperator_size + Y_MAX(internal_jmp_size,DELAYJUMPSIZE) + EPILOGSIZE;
    } else {
      u32 op2 = memGetWord(addr+2);
      u32 delayop = dsh2_instructions[op2];
      calsize = used() + *asm_list[i].size + *asm_list[delayop].size + 
      delay_seperator_size + Y_MAX(internal_delay_jmp_size,SEPERATORSIZE_DELAY_AFTER) + EPILOGSIZE;
    }
#else    
    if ( delay == 0 || delay == 0xFF) {
      calsize  = used() + regular_size + nomal_seperator_size + EPILOGSIZE;
    }else if(delay == 1 || delay == 5) {
      calsize = used() + *asm_list[i].size + nomal_seperator_size + DELAYJUMPSIZE + EPILOGSIZE;
    } else {
      u32 op2 = MappedMemoryReadWord(addr+2,NULL);
      u32 delayop = dsh2_instructions[op2];
      calsize = used() + *asm_list[i].size + *asm_list[delayop].size + delay_seperator_size + SEPERATORSIZE_DELAY_AFTER + EPILOGSIZE;
    }
#endif
    // Keep the baseline's code-size-derived execution boundaries. Smaller
    // native code must not postpone scheduler/interrupt observations.
    if (scheduling_size > used())
      calsize += scheduling_size - used();
    if (calsize >= MAXBLOCKSIZE) {
      break; // no space is available
    }

    if (0x1b == op) { // SLEEP
      page->flags |= BLOCK_LOOP;
    }

    // Inifinity Loop Detection
    if (count == 0 && (op & 0xF00F) == 0x6000) { // mov ? R0
      u32 loopcheck = memGetLong(addr + 2);
      if ((loopcheck & 0xFF00FFFF) == 0xC80089FC) { // test, bf
        page->flags |= BLOCK_LOOP;
      }
      if( (loopcheck&0xF00FFFFF) == 0x20088DFC ){ // slave waits intrrupt capture
          page->flags |= BLOCK_LOOP;
      }
    }

//#ifdef BUILD_INFO  
//    LOG("compiling %08X, 0x%04X @ 0x%08X\n", startptr, op, addr);
//#endif    

    addr += 2;

#ifdef BUILD_INFO
    if(show_code_) DumpInstX( i, addr-2, op  );
#endif

    instruction_counter++;
    asm_list[i].build_count++;
    write_memory_counter += asm_list[i].write_count;

    u32 jumppc = 0xFFFFFFFF;
    uintptr_t jumpptr = 0xFFFFFFFF;
    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {
      if (asm_list[i].delay == 1) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
      else if (asm_list[i].delay == 2) {
        temp = (op & 0xfff) << 1;
        if (temp & 0x1000)
          temp |= 0xfffff000;
        jumppc = addr + ((signed)(op & 0xfff) << 1) + 2;
      }
      else if (asm_list[i].delay == 3) {
        jumppc = addr + ((signed char)(op & 0xff) << 1) + 2;
      }
#if defined(AARCH64)
      if ( addr_map.count(jumppc) != 0 && write_memory_counter != 0 ) {
        jumpptr = addr_map[jumppc];
        //LOG("jumpptr = %lx\n", jumpptr );
      }
#endif      
    }


#ifdef VITA_SH2_DEFER_SEPARATORS
    if (defer_enabled && asm_list[i].delay == 0 && !lowered_load && !Sh2TemplateReadsPcOrCount(op)) {
      memcpy(ptr, regular_code, regular_size);
      instrSize[blockCount][count++] = regular_size;
      opcodePass(&asm_list[i], op, ptr);
      ptr += regular_size;
      defer_pc += 2;
      defer_cycles += asm_list[i].cycle;
      scheduling_size += nomal_seperator_size; /* baseline would have emitted it */
      if ( (op & 0xF0FF) == 0x400e || (op & 0xF0FF) == 0x4007) break; // sh2_LDC_SR
      continue;
    }
    if (defer_pc) flush_deferred();
#endif
    // Regular Opcode ( No Delay Branch )
    if (asm_list[i].delay == 0) { 
      memcpy(ptr, regular_code, regular_size);
      memcpy(ptr + regular_size, nomal_seperator, nomal_seperator_size);
      instrSize[blockCount][count++] = regular_size + nomal_seperator_size;
      if (!lowered_load) opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
      u32 * counterpos = (u32*)(ptr + regular_size + nomal_seperator_counter_offset);
      *counterpos |= ((asm_list[i].cycle)<<10);
#else
      u8 * counterpos = ptr + regular_size + nomal_seperator_counter_offset;
      *counterpos = asm_list[i].cycle;
#endif

      ptr += regular_size + nomal_seperator_size;
    }

    // No Intrupt Func ToDo: Never end block these functions
    else if (asm_list[i].delay == 0xFF ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      instrSize[blockCount][count++] = *(asm_list[i].size) + nomal_seperator_size;
      opcodePass(&asm_list[i], op, ptr);
      ptr += *(asm_list[i].size) + nomal_seperator_size;
    }

#if defined(VITA_SH2_FUSED_BRANCH) && !defined(AARCH64) && !defined(VITA_SH2_CHAIN_ABI)
    // Block-ending non-delayed BT/BF (SH7604 table 2.16: 3 states taken, 1
    // not taken). Same result as the BT/BF template + normal separator +
    // PageFlip exit: PC = r8 + 2 or r8 + 4 + 2*disp, cycles += 1 or 3. A32
    // modified immediates only; otherwise the template path below is used.
    else if (!debug_mode_ && asm_list[i].delay == 1 && jumpptr == 0xFFFFFFFF &&
             ((op >> 8) == 0x89 || (op >> 8) == 0x8B) &&
             [&]() { const int off = 4 + 2 * int(int8_t(op & 0xff));
                     const unsigned mag = unsigned(off < 0 ? -off : off);
                     return mag <= 255 || (mag & 3) == 0; }()) {
      const int off = 4 + 2 * int(int8_t(op & 0xff));
      const unsigned mag = unsigned(off < 0 ? -off : off);
      // mag <= 255: plain imm8; else (<= 260, multiple of 4): imm8 = mag/4 ror 30.
      const u32 imm = mag <= 255 ? mag : (0xF00u | (mag >> 2));
      const u32 taken = (op >> 8) == 0x89 ? 0x10000000u : 0x00000000u; // BT: NE (T=1), BF: EQ
      const u32 words[] = {
        0xe5971040u,                                           // ldr r1,[r7,#64]  SR
        0xe2880002u,                                           // add r0,r8,#2     fall-through
        0xe3110001u,                                           // tst r1,#1        T
        taken | (off < 0 ? 0x02480000u : 0x02880000u) | imm,   // add/sub<cc> r0,r8,#|off|
        0xe2899001u,                                           // add r9,r9,#1
        taken | 0x02899002u,                                   // add<cc> r9,r9,#2
        0xe5870058u,                                           // str r0,[r7,#88]  PC
        0xe587905cu,                                           // str r9,[r7,#92]  count
#if defined(VITA_SH2_NATIVE_DISPATCH)
        0xe597f090u,                                           // ldr pc,[r7,#144] BLOCK_RETURN
#elif defined(VITA_SH2_BASE_REG)
        0xe8bd8ff8u,                                           // pop {r3-r11,pc}  BLOCK_RETURN
#else
        0xe8bd87f0u,                                           // pop {r4-r10,pc}  BLOCK_RETURN
#endif
      };
#ifdef VITA_SH2_LINK
      if (!debug_mode_ && used() + 4 * 11 + 2 * kLinkStubBytes + EPILOGSIZE <= MAXBLOCKSIZE) {
        // The same PC and count, with one exit per outcome so both can link.
        const u32 at = addr - 2;  // this BT/BF; r8 holds it
        u32 *w = reinterpret_cast<u32 *>(ptr);
        w[0] = 0xe5971040u;                                   // ldr r1,[r7,#64]  SR
        w[1] = 0xe2899001u;                                   // add r9,r9,#1
        w[2] = 0xe3110001u;                                   // tst r1,#1        T
        w[3] = taken | 0x0a00000du;                           // b<cc> taken (word 18)
        w[4] = 0xe2880002u;                                   // add r0,r8,#2     fall-through
        w[5] = 0xe5870058u;                                   // str r0,[r7,#88]  PC
        w[6] = 0xe587905cu;                                   // str r9,[r7,#92]  count
        link_stub(ptr + 7 * 4, 0, at + 2);
        u32 *t = w + 7 + kLinkStubBytes / 4;
        t[0] = 0xe2899002u;                                   // add r9,r9,#2
        t[1] = (off < 0 ? 0xe2480000u : 0xe2880000u) | imm;   // add/sub r0,r8,#|off|
        t[2] = 0xe5870058u;                                   // str r0,[r7,#88]  PC
        t[3] = 0xe587905cu;                                   // str r9,[r7,#92]  count
        link_stub(reinterpret_cast<u8 *>(t + 4), 0, at + off);
        const size_t bytes = 11 * 4 + 2 * kLinkStubBytes;
        instrSize[blockCount][count++] = bytes;
        ptr += bytes;
      } else
#endif
      {
      memcpy(ptr, words, sizeof(words));
      instrSize[blockCount][count++] = sizeof(words);
      ptr += sizeof(words);
      }
    }
#endif
    // Normal Jump
    else if (asm_list[i].delay == 1 || asm_list[i].delay == 5 ) { 
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)nomal_seperator, nomal_seperator_size);
      if (jumpptr != 0xFFFFFFFF ) {
        intptr_t offset = *(asm_list[i].size) + nomal_seperator_size;
        memcpy((void*)(ptr + offset), (void*)internal_jmp, internal_jmp_size);
        instrSize[blockCount][count++] = offset + internal_jmp_size;
        opcodePass(&asm_list[i], op, ptr);
        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_jmp_to_offset);

        u32 * jumppos = (u32*)(ptr + offset + internal_jmp_to_offset);
        LOG("nomal jump_offset = %08X- %08X = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | ((((jump_offset)>>2)&0x7FFFF)<<5);
        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);

#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= ((asm_list[i].cycle) << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + internal_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
        memcpy((void*)(ptr + *(asm_list[i].size) + nomal_seperator_size), (void*)PageFlip, DELAYJUMPSIZE);
        instrSize[blockCount][count++] = *(asm_list[i].size) + nomal_seperator_size + DELAYJUMPSIZE;
        opcodePass(&asm_list[i], op, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[i].size) + nomal_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[i].size) + nomal_seperator_counter_offset;
        *counterpos = asm_list[i].cycle;
#endif
        ptr += *(asm_list[i].size) + nomal_seperator_size + DELAYJUMPSIZE;
      }
    }

    // Jmp With Delay Operation
    else { 
#if defined(VITA_SH2_FUSED_DELAY) && defined(VITA_SH2_A9_REGIONS) && !defined(AARCH64)
      // BRA/BSR, BT/S, BF/S, RTS, JSR/JMP @Rn with a region-admissible delay
      // slot: the slot as a one-instruction region, then one exit with the
      // templates' PC and count (cycle(branch) + cycle(slot), one less for
      // an untaken BT/S or BF/S). The branch reads its inputs after the slot,
      // so slots that write them are excluded (T, Rn, PR). BSR/JSR store PR
      // before the slot, as their templates do. PC-relative slots are
      // excluded: the templates run them with r8 = target - 2.
      const u32 branch_pc = addr - 2;
      const u16 slot_op = MappedMemoryReadWord(addr, NULL);
      const unsigned rn = (op >> 8) & 15;
      const bool bt_bf = (op & 0xFD00) == 0x8D00, relative = (op & 0xE000) == 0xA000;
      const bool call = (op & 0xF000) == 0xB000 || (op & 0xF0FF) == 0x400B;
      const bool indirect = (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x402B;
      const bool slot_writes_t = sh2a9::RegisterRegion::WritesT(slot_op);
      const bool slot_touches_rn = ((slot_op >> 8) & 15) == rn || ((slot_op >> 4) & 15) == rn ||
        (rn == 0 && ((slot_op >> 12) == 8 || (slot_op >> 12) == 0xC));
      sh2a9::RegisterRegion slot_region = make_region();
      if (!debug_mode_ && (relative || bt_bf || indirect || op == 0x000B) &&
          asm_list[dsh2_instructions[slot_op]].delay == 0 && region_admits(slot_region, slot_op, addr, addr) &&
          !sh2a9::RegisterRegion::IsPcLoad(slot_op) &&
          !(bt_bf && slot_writes_t) && !(indirect && slot_touches_rn) &&
          !(op == 0x000B && slot_op == 0x4F26) &&
          used() + (slot_region.MaxEmissionWords(slot_op) + 80) * sizeof(u32) + EPILOGSIZE
#ifdef VITA_SH2_LINK
            + 2 * kLinkStubBytes + 4
#endif
            < MAXBLOCKSIZE) {
        instrSize[blockCount][count++] = 0;
        if (call) {
          slot_region.LoadConstant(12, branch_pc + 4);
          slot_region.code.push_back(0xe587c054u);              // str ip,[r7,#84]  PR
        }
#ifdef SET_DIRTY
        if (ParentT) {
          auto &owners = ParentT[adress_mask(addr)];
          MarkCode(adress_mask(addr));
          owners.push_back(adress_mask(start_addr));
          owners.unique();
        }
#endif
        slot_region.Emit(slot_op, addr);
        const int slot_i = dsh2_instructions[slot_op];
        ++asm_list[slot_i].build_count;
        write_memory_counter += 2 * asm_list[slot_i].write_count;  // as the template path counts it
        addr += 2;
#ifdef VITA_SH2_BLOCK_COLD
        slot_region.FinishHot();
        if (!slot_region.cold.empty()) {
          cold_parts.push_back({unsigned((ptr - startptr) / 4), std::move(slot_region.cold),
                                std::move(slot_region.to_cold), std::move(slot_region.to_hot)});
          cold_bytes += cold_parts.back().words.size() * sizeof(u32);
          legacy_extra += sizeof(u32);
        }
#else
        slot_region.Finish();
#endif
        legacy_extra += size_t(slot_region.legacy_words) * sizeof(u32);
        std::vector<u32> &w = slot_region.code;
        const u32 fall = branch_pc + 4;
        u32 taken_cc = 0xe0000000u, skip_cc = 0;
        if (relative) {
          slot_region.LoadConstant(0, fall + 2 * u32(s32(u32(op) << 20) >> 20));
        } else if (bt_bf) {
          const int off = 2 * int(int8_t(op & 0xff));
          const unsigned mag = unsigned(off < 0 ? -off : off);
          taken_cc = (op >> 8) == 0x8D ? 0x10000000u : 0x00000000u;  // BT/S: NE (T=1), BF/S: EQ
          skip_cc = taken_cc ^ 0x10000000u;
          w.push_back(0xe5971040u);                               // ldr r1,[r7,#64]  SR
          slot_region.LoadConstant(0, fall);
          w.push_back(0xe3110001u);                               // tst r1,#1        T
          if (mag)
            w.push_back(taken_cc | (off < 0 ? 0x02400000u : 0x02800000u) |
                        (mag <= 255 ? mag : 0xC01u));             // add/sub<cc> r0,r0,#|off|
        } else if (op == 0x000B) {
          w.push_back(0xe5970054u);                               // ldr r0,[r7,#84]  PR
        } else {
          w.push_back(0xe5970000u | (rn * 4));                    // ldr r0,[r7,#Rn]
        }
        w.push_back(0xe2899000u | asm_list[i].cycle);             // add r9,r9,#cycle(branch)
        if (bt_bf) w.push_back(skip_cc | 0x02499001u);            // sub<!cc> r9,r9,#1
        w.push_back(0xe5870058u);                                 // str r0,[r7,#88]  PC
        w.push_back(0xe587905cu);                                 // str r9,[r7,#92]  count
#ifdef VITA_SH2_LINK
        if (relative || bt_bf) {
          // Static targets link (PC in r0). BT/S, BF/S: the flags of the T
          // test still hold, the not-taken exit is the second stub.
          const u32 target = relative ? fall + 2 * u32(s32(u32(op) << 20) >> 20)
                                      : fall + u32(2 * int(int8_t(op & 0xff)));
          if (bt_bf) w.push_back(skip_cc | 0x0a000000u | (kLinkStubBytes / 4 - 1));  // b<!cc> not-taken stub
          memcpy(ptr, w.data(), w.size() * sizeof(u32));
          u8 *stub = ptr + w.size() * sizeof(u32);
          link_stub(stub, 0, target);
          stub += kLinkStubBytes;
          if (bt_bf) { link_stub(stub, 0, fall); stub += kLinkStubBytes; }
          instrSize[blockCount][count - 1] = u32(stub - ptr);
          instrSize[blockCount][count++] = 0;
          ptr = stub;
        } else {
#endif
#if defined(VITA_SH2_NATIVE_DISPATCH)
        w.push_back(0xe597f090u);                                 // ldr pc,[r7,#144] BLOCK_RETURN
#elif defined(VITA_SH2_BASE_REG)
        w.push_back(0xe8bd8ff8u);                                 // pop {r3-r11,pc}  BLOCK_RETURN
#else
        w.push_back(0xe8bd87f0u);                                 // pop {r4-r10,pc}  BLOCK_RETURN
#endif
        memcpy(ptr, w.data(), w.size() * sizeof(u32));
        instrSize[blockCount][count - 1] = w.size() * sizeof(u32);
        instrSize[blockCount][count++] = 0;
        ptr += w.size() * sizeof(u32);
#ifdef VITA_SH2_LINK
        }
#endif
      } else
#endif
      {

      u32 cycle = asm_list[i].cycle;
      memcpy((void*)ptr, (void*)(asm_list[i].func), *(asm_list[i].size));
      memcpy((void*)(ptr + *(asm_list[i].size)), (void*)delay_seperator, delay_seperator_size);
      instrSize[blockCount][count++] = *(asm_list[i].size) + delay_seperator_size;
      opcodePass(&asm_list[i], op, ptr);
#ifdef VITA_SH2_LINK
      u8 *const slot_separator = ptr + *(asm_list[i].size);  // its not-taken exit is word 6
      const u32 branch_pc = addr - 2;
#endif
      ptr += *(asm_list[i].size) + delay_seperator_size;

      // Get NExt instruction
      temp = MappedMemoryReadWord(addr,NULL);
#ifdef SET_DIRTY
      if (ParentT) {
        u32 keepaddr = adress_mask(addr);
        MarkCode(keepaddr);
        ParentT[keepaddr].push_back(adress_mask(start_addr));
        ParentT[keepaddr].unique();
      }
#endif
      addr += 2;
      j = opcodeIndex(temp);
      write_memory_counter += asm_list[j].write_count;

#ifdef BUILD_INFO
      if(show_code_) DumpInstX( j, addr-2, temp  );
#endif
      if (asm_list[j].func == 0) {
        LOG("Unimplemented Opcode (0x%4x) at 0x%8x\n", temp, addr-2);
        return -1;
        break;
      }
      
      asm_list[j].build_count++;
      write_memory_counter += asm_list[j].write_count;

      cycle += asm_list[j].cycle;
      
      intptr_t offset = 0;
      memcpy((void*)ptr, (void*)(asm_list[j].func), *(asm_list[j].size));
      offset = *(asm_list[j].size);

      // internal loop
      if (jumpptr != 0xFFFFFFFF) {

        u32 cpsize = internal_delay_jmp_size;
        memcpy((void*)(ptr + offset), (void*)internal_delay_jmp, internal_delay_jmp_size);
        instrSize[blockCount][count++] = offset + internal_delay_jmp_size;
        opcodePass(&asm_list[j], temp, ptr);

        intptr_t jump_offset = jumpptr - (intptr_t)(ptr + offset + internal_delay_jmp_to_offset);
        u32 * jumppos = (u32*)(ptr + offset + internal_delay_jmp_to_offset);

        LOG("delay jump_offset = %lX - %lX = %08X\n", jumpptr, (intptr_t)(ptr + offset + internal_jmp_to_offset), jump_offset);

        *jumppos = 0x14000000 | ((jump_offset>>2)&0x3FFFFFF);
        // cbnz w0,#imm(aarch64)
        //*jumppos = 0x35000000 | (((jump_offset>>2)&0x7FFFF)<<5);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + internal_delay_jmp_size;
        write_memory_counter = 0;
        continue;
      }
      else {
        memcpy((void*)(ptr + offset), (void*)seperator_delay_after, SEPERATORSIZE_DELAY_AFTER);
        instrSize[blockCount][count++] = offset + SEPERATORSIZE_DELAY_AFTER;
        opcodePass(&asm_list[j], temp, ptr);
#if defined(AARCH64)
        u32 * counterpos = (u32*)(ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset);
        *counterpos |= (asm_list[i].cycle << 10);
#else
        u8 * counterpos = ptr + *(asm_list[j].size) + delayslot_seperator_counter_offset;
        *counterpos = cycle;
#endif
        ptr += *(asm_list[j].size) + SEPERATORSIZE_DELAY_AFTER;
#ifdef VITA_SH2_LINK
        // PC-relative BRA/BSR, BT/S, BF/S: the taken exit (after the delay
        // slot, PC in r8) and for BT/S, BF/S the not-taken exit before it
        // (PC = the delay slot, in r8) have static targets.
        const bool relative = (op & 0xE000) == 0xA000;
        const bool conditional = (op & 0xFD00) == 0x8D00;
        u32 *const taken_return = reinterpret_cast<u32 *>(ptr) - 1;
        u32 *const skip_return = reinterpret_cast<u32 *>(slot_separator) + 6;
        if (!debug_mode_ && (relative || conditional) && *taken_return == kBlockReturn &&
            (!conditional || *skip_return == kBlockReturn) &&
            used() + 2 * kLinkStubBytes + EPILOGSIZE <= MAXBLOCKSIZE) {
          const u32 target = relative ? branch_pc + 4 + 2 * u32(s32(u32(op) << 20) >> 20)
                                      : branch_pc + 4 + 2 * u32(s32(s8(op & 0xff)));
          ptr -= 4;
          link_stub(ptr, 8, target);
          ptr += kLinkStubBytes;
          if (conditional) {
            *skip_return = ArmBranch(0xea000000u, skip_return, ptr);
            link_stub(ptr, 8, branch_pc + 2);
            ptr += kLinkStubBytes;
          }
        }
#endif
      }
      }
    }

    if (asm_list[i].delay != 0xFF && asm_list[i].delay != 0x00) {

      // jump to inside and no write is happend
      if (jumppc >= start_addr &&  jumppc < addr ) {
        if (write_memory_counter == 0) {
          page->flags |= BLOCK_LOOP;
#ifdef BUILD_INFO 
              LOG("InfinityLoop block %08X 0x%04X  from 0x%08X to 0x%08X\n", start_addr, op, addr - 2, jumppc);
#endif
        }
      }
      else if (jumppc < start_addr && write_memory_counter == 0 ) {

        Block * tmp = NULL; 
        if ( (jumppc&0x0FF00000) == 0x06000000 && (start_addr & 0x0FF00000) == 0x06000000) {
          tmp = LookupTable[(jumppc & 0x000FFFFF) >> 1];
        }else if ((jumppc & 0x0FF00000) == 0x00200000 && (start_addr & 0x0FF00000) == 0x00200000) {
          tmp = LookupTableLow[(jumppc & 0x000FFFFF) >> 1];
        }
        else if ((jumppc & 0x0FF00000) == 0x00000000 && (start_addr & 0x0FF00000) == 0x00000000) {
          tmp = LookupTableRom[(jumppc & 0x000FFFFF) >> 1];
        }
        if (tmp != NULL && (tmp->flags&BLOCK_WRITE) == 0 && (tmp->e_addr+2) == page->b_addr ) {
          page->flags |= BLOCK_LOOP;
        }

      }
      write_memory_counter = 0;
      //if( (op&0xFF00) == 0x8900) continue;  // BT
      //if( (op&0xFF00) == 0x8B00) continue;  // BF
#ifdef VITA_SH2_LINK
      branch_exit = true;
#endif
      break;
    }

    if ( (op & 0xF0FF) == 0x400e || (op & 0xF0FF) == 0x4007) // sh2_LDC_SR
    {
      break;
    }

  }
  page->e_addr = addr-2;
#if defined(VITA_SH2_POLL_FUSION) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
  // Decode only side-effect-free instruction storage. The original compiler
  // has already established source owners, block flags and execution extent.
  if (!debug_mode_ && !(page->flags & BLOCK_RESIDENT_LOOP) &&
      ((start_addr >> 20) == 2 || (start_addr >> 20) == 0x60)) {
    const u32 words = (page->e_addr - page->b_addr) / 2 + 1;
    if (words == 3 || words == 5) {
      u16 source[5];
      for (unsigned k = 0; k < words; ++k)
        source[k] = MappedMemoryReadWord(page->b_addr + k * 2, nullptr);
      const auto fused = sh2a9::PollStep::Emit(source, words);
      const size_t original_bytes = ptr - (startptr + PROLOGSIZE);
      const size_t fused_bytes = fused.size() * sizeof(u32);
      if (!fused.empty() && fused_bytes < original_bytes) {
        ptr = startptr + PROLOGSIZE;
        memcpy(ptr, fused.data(), fused_bytes);
        ptr += fused_bytes;
        for (int k = 0; k < count; ++k) instrSize[blockCount][k] = k ? 0 : fused_bytes;
        page->flags |= BLOCK_POLL_FUSED;
        ++poll_fused_blocks;
        poll_fused_saved_bytes += original_bytes - fused_bytes;
      }
    }
  }
#endif
#if defined(VITA_SH2_POLL_STEP) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
  if (!debug_mode_ && !(page->flags & BLOCK_RESIDENT_LOOP)) {
    const u32 words = (page->e_addr - page->b_addr) / 2 + 1;
    if (words == 3 || words == 5) {
      u16 source[5];
      for (unsigned k = 0; k < words; ++k)
        source[k] = MappedMemoryReadWord(page->b_addr + k * 2, nullptr);
      page->poll_step = sh2a9::PollStep::Decode(source, words);
      // Retain the original flags and native code. Normal ownership tables
      // invalidate/recompile the recipe along with its source block.
    }
  }
#endif
#if defined(VITA_SH2_POLL_SKIP) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
  if (!debug_mode_ && !(page->flags & BLOCK_RESIDENT_LOOP)) {
    const u32 words = (page->e_addr - page->b_addr) / 2 + 1;
    if (words == 3 || words == 5) {
      u16 source[5];
      for (unsigned k = 0; k < words; ++k)
        source[k] = MappedMemoryReadWord(page->b_addr + k * 2, nullptr);
      page->poll = sh2a9::PollLoop::Decode(source, words);
      // These recipes supersede the legacy unguarded idle-loop shortcut.
      // On a rejected address or concurrent sound mode execute normally;
      // never let FinishBlock bypass the runtime proof via BLOCK_LOOP.
      if (page->poll) page->flags &= ~BLOCK_LOOP;
    }
  }
#endif
#ifdef VITA_SH2_DEFER_SEPARATORS
  if (defer_pc) flush_deferred();
#endif
  memcpy((void*)ptr, (void*)epilogue, EPILOGSIZE);
  ptr += EPILOGSIZE;
#ifdef VITA_SH2_LINK
  // Falling off the end continues at the next instruction (PC in r8).
  if (!debug_mode_ && !branch_exit && !resident_emitted && !(page->flags & BLOCK_POLL_FUSED) &&
      reinterpret_cast<u32 *>(ptr)[-1] == kBlockReturn &&
      used() - 4 + kLinkStubBytes <= MAXBLOCKSIZE) {
    ptr -= 4;
    link_stub(ptr, 8, addr);
    ptr += kLinkStubBytes;
  }
  // A slot can be compiled more than once before eviction (MAC regions
  // recompile it): drop what an earlier pass registered first.
  UnlinkSlot(int(page - dCode));
  // BLOCK_LOOP blocks never chain (sh2_dispatch returns to C after them);
  // poll fusion replaced the code the exits were recorded in.
  if (!debug_mode_ && !(page->flags & (BLOCK_LOOP | BLOCK_POLL_FUSED))) {
    const int slot = int(page - dCode);
    for (unsigned k = 0; k < link_count; ++k) {
      link_out[slot][k] = links[k];
      link_in.emplace(links[k].target, u32(slot) * 2 + k);
      const Block *t = LookupTable[(links[k].target & 0x000FFFFF) >> 1];
      if (t && t->link_pc == links[k].target) PatchLink(slot, int(k), t);  // published with this block
    }
  }
#endif

#ifdef VITA_SH2_BLOCK_COLD
  for (auto &part : cold_parts) {
    u32 *const words = reinterpret_cast<u32 *>(startptr);
    const unsigned cold_base = unsigned((ptr - startptr) / 4);
    for (const auto &f : part.to_cold)
      words[part.hot_base + f.at] = sh2a9::RegisterRegion::Branch(f.cond, part.hot_base + f.at, cold_base + f.target);
    for (const auto &f : part.to_hot)
      part.words[f.at] = sh2a9::RegisterRegion::Branch(f.cond, cold_base + f.at, part.hot_base + f.target);
    memcpy(ptr, part.words.data(), part.words.size() * sizeof(u32));
    ptr += part.words.size() * sizeof(u32);
  }
#endif

#if defined(VITA_SH2_ARENA_DISPATCH)
  // Every BLOCK_RETURN (templates, regions, exits) becomes B arena_dispatch.
  // No emitted data word equals this instruction (constants are MOVW/MOVT).
  for (u32 *w = reinterpret_cast<u32 *>(startptr); w < reinterpret_cast<u32 *>(ptr); ++w)
    if (*w == 0xe597f090u)                         // ldr pc,[r7,#144]
      *w = 0xea000000u | ((u32(arena_dispatch - reinterpret_cast<u8 *>(w) - 8) >> 2) & 0xffffffu);
#endif
  last_code_bytes_ = u32(ptr - startptr);
  if (write_memory_counter > 0) {
    page->flags |= BLOCK_WRITE;
  }

#ifdef BUILD_INFO 
  //LOG("*********** end block size = %08X *************\n", (ptr - startptr));
  if( show_code_ ) {
      if( page->flags & BLOCK_LOOP ) LOG("Infinity loop");
      LOG("*********** end block *************\n");
  }
#endif 

#if defined(ARCH_IS_LINUX)
  cacheflush((uintptr_t)page->code,(uintptr_t)ptr,0);
#endif

#if 0 //defined(BUILD_INFO) // Dump code
  char fname[64];
#if defined(ANDROID)
  sprintf(fname,"/mnt/sdcard/yabause/%08X.bin",start_addr);
#else
  sprintf(fname,"%08X.bin",start_addr);
#endif
   FILE * fp = fopen(fname, "wb");
  if(fp){
    fwrite(page->code, sizeof(char), (uintptr_t)ptr - (uintptr_t)page->code, fp);
    fclose(fp);
  }
#endif

  return 0;
}

DynarecSh2::DynarecSh2() : m_pDynaSh2(new tagSH2()), memcycle_(m_pDynaSh2->memcycle) {
  m_pDynaSh2->getmembyte = (uintptr_t)memGetByte;
  m_pDynaSh2->getmemword = (uintptr_t)memGetWord;
  m_pDynaSh2->getmemlong = (uintptr_t)memGetLong;
  m_pDynaSh2->setmembyte = (uintptr_t)memSetByte;
  m_pDynaSh2->setmemword = (uintptr_t)memSetWord;
  m_pDynaSh2->setmemlong = (uintptr_t)memSetLong;
  m_pDynaSh2->eachclock = (uintptr_t)DebugEachClock;
  m_pDynaSh2->spec_high = (uintptr_t)HighWram;
  m_pDynaSh2->spec_pages = (uintptr_t)CompileBlocks::getInstance()->code_pages;
  m_pDynaSh2->chain_table = (uintptr_t)CompileBlocks::getInstance()->LookupTable;
#ifdef VITA_SH2_MACW_WRAM
  m_pDynaSh2->macw_saturate = (uintptr_t)&sh2_macw_saturate;
#endif
#ifdef VITA_SH2_MACL_WRAM
  m_pDynaSh2->macl_saturate = (uintptr_t)&sh2_macl_saturate;
#endif
#ifdef VITA_SH2_NATIVE_DISPATCH
  m_pDynaSh2->dispatch = (uintptr_t)&sh2_dispatch;  // chain_budget 0 = plain return
#endif

  m_pCompiler = CompileBlocks::getInstance();
  m_ClockCounter = 0;
  m_IntruptTbl.clear();
  m_bIntruptSort = true;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  pre_PC_ = 0;
  ctx_ = NULL;
  mtx_ = YabThreadCreateMutex();
  logenable_ = false;
  memcycle_ = 0;
}

DynarecSh2::~DynarecSh2(){
  YabThreadFreeMutex(mtx_);
  delete m_pDynaSh2;
}

void DynarecSh2::ResetCPU(){
  memset((void*)m_pDynaSh2->GenReg, 0, sizeof(u32) * 16);
  memset((void*)m_pDynaSh2->CtrlReg, 0, sizeof(u32) * 3);
  memset((void*)m_pDynaSh2->SysReg, 0, sizeof(u32) * 6);

  m_pDynaSh2->CtrlReg[0] = 0x000000;  // SR
  m_pDynaSh2->CtrlReg[2] = 0x000000; // VBR
  m_pDynaSh2->SysReg[3] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2],NULL);
  m_pDynaSh2->GenReg[15] = MappedMemoryReadLong(m_pDynaSh2->CtrlReg[2] + 4,NULL);
  m_pDynaSh2->SysReg[4] = 0;
  m_pDynaSh2->SysReg[5] = 0;
  m_pDynaSh2->spec_high = (uintptr_t)HighWram;   // allocated by now
  m_pDynaSh2->spec_pages = (uintptr_t)m_pCompiler->code_pages;
  pre_cnt_ = 0;
  pre_exe_count_ = 0;
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  memcycle_ = 0;
  m_IntruptTbl.clear();
}

void DynarecSh2::ExecuteCount( u32 Count ) {
#ifdef VITA_SH2_LEAN_DISPATCH
  // The debug body stays out of line: its frame is not paid on every slice.
  if (!m_pCompiler->debug_mode_) { ExecuteCountLean(Count); return; }
#endif
  ExecuteCountDebug(Count);
}
void __attribute__((noinline)) DynarecSh2::ExecuteCountDebug( u32 Count ) {
  u32 targetcnt = 0;
  
  m_pDynaSh2->SysReg[4] = 0;
    if (Count > pre_exe_count_) {
    targetcnt = Count - pre_exe_count_;
  }
  else {
    // Just Onestep
    //Execute();
    pre_exe_count_ = (pre_exe_count_ + m_pDynaSh2->SysReg[4]) - Count ;
    return;
  }

#if 0
  // Overflow
  if (targetcnt < m_pDynaSh2->SysReg[4]){
    targetcnt = Count + (0xFFFFFFFF - m_pDynaSh2->SysReg[4]) + 1;
    m_pDynaSh2->SysReg[4] = 0;
  }
#endif

  m_pDynaSh2->exitcount = targetcnt;
  counted_slice_active_ = true;

  //if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
  //  this->CheckInterupt();
  //}
  memcycle_ = 0;
#ifdef VITA_SH2_LEAN_STATS
  // Native cycles = slice count minus memory-cycle and loop-skip additions,
  // the same total the per-block GET_COUNT deltas used to sum.
  u32 slice_memory = 0, slice_skip = 0;
#endif
  while (m_pDynaSh2->SysReg[4] < targetcnt) {
#if defined(VITA_SH2_NATIVE_CHAIN) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
    // Keep every 1024th native call on ExecuteBlock's timed path. A chain
    // never crosses the next sampling boundary, so instrumentation is retained.
    if (!m_pCompiler->debug_mode_ && (native_returns & 1023u)) {
      Sh2ChainContext chain = {m_pDynaSh2->GenReg,
        reinterpret_cast<void *const *>(m_pCompiler->LookupTableRom),
        reinterpret_cast<void *const *>(m_pCompiler->LookupTableLow),
        reinterpret_cast<void *const *>(m_pCompiler->LookupTable),
        &memcycle_, targetcnt, u32(yabsys.emulatebios || yabsys.extend_backup),
        u32(1024 - (native_returns & 1023u)), 0, 0, 0, nullptr, 0};
      VitaSh2RunCached(&chain);
      ++chain_batches;
      chain_blocks += chain.completed;
      if (chain.reason < 7) ++chain_stops[chain.reason];
      native_returns += chain.completed;
      native_cycles += (u64(chain.guest_cycles_high) << 32) | chain.guest_cycles_low;
      m_pCompiler->exec_count_ += chain.completed;
      if (chain.reason == SH2_CHAIN_INTERRUPT || chain.reason == SH2_CHAIN_POST_BLOCK) {
        // As in the reference loop, finish interrupt/loop handling BEFORE
        // accounting for the final block's pending memory cycles.
        if (FinishBlock(static_cast<Block *>(chain.last_block)) == IN_INFINITY_LOOP) {
          SET_COUNT(targetcnt);
          ++loopskip_cnt_;
        }
        m_pDynaSh2->SysReg[4] += memcycle_;
        memcycle_ = 0;
      }
      if (chain.completed) continue;
    }
#endif
    int result;
#if defined(VITA_SH2_CACHED_DISPATCH) && !defined(DEBUG_CPU) && !defined(EXECUTE_STAT)
    Block *cached = !m_pCompiler->debug_mode_ ? sh2a9::CachedBlock(GET_PC(),
      yabsys.emulatebios || yabsys.extend_backup,
      m_pCompiler->LookupTableRom, m_pCompiler->LookupTableLow,
      m_pCompiler->LookupTable) : nullptr;
#ifdef VITA_SH2_PC_LOADS
    // Tables alias cached/uncached addresses. PC-specialized blocks require
    // the full source PC, including its memory-cycle/alias interpretation.
    if (cached && cached->b_addr != GET_PC()) cached = nullptr;
#endif
    if (cached) {
#ifndef VITA_SH2_LEAN_STATS
      ++m_pCompiler->exec_count_;
#endif
      result = ExecuteBlock(cached);
    } else result = Execute();
#else
    result = Execute();
#endif
    if (result == IN_INFINITY_LOOP ) {
#ifdef VITA_SH2_LEAN_STATS
        slice_skip += targetcnt - GET_COUNT();
#endif
        SET_COUNT(targetcnt);
        loopskip_cnt_++;
    }
#ifdef VITA_SH2_LEAN_STATS
    slice_memory += memcycle_;
#endif
    m_pDynaSh2->SysReg[4] += memcycle_;
    memcycle_ = 0;
    //printf("%d/%d\n",GET_COUNT(),targetcnt);
  }
#ifdef VITA_SH2_LEAN_STATS
  native_cycles += m_pDynaSh2->SysReg[4] - slice_memory - slice_skip;
#endif

  CurrentSH2->cycles = m_pDynaSh2->SysReg[4];
  counted_slice_active_ = false;
  //if (Count == 1) {
  //  one_step_ = true;
  //  pre_exe_count_ = 0;
  //}
  //else {
  //  one_step_ = false;
    pre_exe_count_ = m_pDynaSh2->SysReg[4] - targetcnt;
  //}
}

int DynarecSh2::CheckOneStep() {
  if (one_step_ == 1) {
    m_pDynaSh2->SysReg[3] += 2;
    return 1;
  }
  return 0;
}

void DynarecSh2::Undecoded(){

  LOG("Undecoded %08X", GET_PC());
  // Save regs.SR on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_SR());

  // Save regs.PC on stack
  GetGenRegPtr()[15] -= 4;
  memSetLong(GetGenRegPtr()[15], GET_PC()+2);


  // What caused the exception? The delay slot or a general instruction?
  // 4 for General Instructions, 6 for delay slot
  u32 vectnum = 4; //  Fix me

  // Jump to Exception service routine
  u32 newpc = memGetLong(GET_VBR() + (vectnum << 2));
  SET_PC(newpc);

  return;
}

#if defined(VITA_SH2_CACHED_DISPATCH)
__attribute__((noinline))
#elif defined(VITA_SH2_INLINE_DISPATCH)
// Keep the existing per-block semantic boundaries, but do not pay a second
// C++ register-save frame for every block inside ExecuteCount. This is not
// native block linking: all interrupt, budget and invalidation checks remain.
__attribute__((always_inline))
#endif
#if !defined(VITA_SH2_CACHED_DISPATCH)
inline
#endif
int DynarecSh2::Execute(){

  Block * pBlock = NULL;

#ifndef VITA_SH2_LEAN_STATS
  m_pCompiler->exec_count_++;
#endif
#if defined(EXECUTE_STAT)
  m_pCompiler->setShowCode( is_slave_ );
#endif
//#endif

  if ((GET_PC() & 0xFF000000) == 0xC0000000)
  {
    const u64 key = (static_cast<u64>(is_slave_) << 32) | GET_PC();
    auto cached = m_pCompiler->LookupTableC.find(key);
    if (cached != m_pCompiler->LookupTableC.end()) pBlock = cached->second;
    if (pBlock == NULL)
    {
      pBlock = m_pCompiler->CompileBlock(GET_PC());
      if (pBlock) m_pCompiler->LookupTableC[key] = pBlock;
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
    }
  }
  else {

    switch (GET_PC() & 0x0FF00000)
    {

      // ROM
    case 0x00000000:
      if (yabsys.extend_backup) {
        const u32 bupaddr = 0x0007d600; // MappedMemoryReadLong(0x06000358);
        if (GET_PC() == bupaddr) {
          LOG("BUP_Init");
          BiosBUPInit(ctx_);
          yabsys.extend_backup = 2;
          return IN_INFINITY_LOOP;
        }
        else if (yabsys.extend_backup == 2 &&
          GET_PC() >= 0x0380 &&
          GET_PC() <= 0x03A8) {
          BiosHandleFunc(ctx_);
          return IN_INFINITY_LOOP;
        }
      }
      if (yabsys.emulatebios) {
        ctx_->cycles = 0;
         BiosHandleFunc(ctx_);
         memcycle_ += ctx_->cycles;
        return 0;
      }
      pBlock = m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1];
#ifdef VITA_SH2_PC_LOADS
      if (pBlock && pBlock->b_addr != GET_PC()) pBlock = nullptr;
#endif
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC());
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableRom[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // Low Memory
    case 0x00200000:
      pBlock = m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1];
#ifdef VITA_SH2_PC_LOADS
      if (pBlock && pBlock->b_addr != GET_PC()) pBlock = nullptr;
#endif
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC());
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->LookupTableLow[(GET_PC() & 0x000FFFFF) >> 1] = pBlock;
      }
      break;

      // High Memory
    case 0x06000000:
      /*case 0x06100000:*/

      pBlock = m_pCompiler->LookupTable[(GET_PC() & 0x000FFFFF) >> 1];
#ifdef VITA_SH2_PC_LOADS
      if (pBlock && pBlock->b_addr != GET_PC()) pBlock = nullptr;
#endif
      if (pBlock == NULL)
      {
        pBlock = m_pCompiler->CompileBlock(GET_PC(), m_pCompiler->LookupParentTable);
        if (pBlock == NULL) {
          Undecoded();
          return IN_INFINITY_LOOP;
        }
        m_pCompiler->SetHigh((GET_PC() & 0x000FFFFF) >> 1, pBlock);
      }
      break;

      // Cache
    default:
      pBlock = m_pCompiler->CompileBlock(GET_PC());
      if (pBlock == NULL) {
        Undecoded();
        return IN_INFINITY_LOOP;
      }
      break;
    }
  }
    
#if 0
    static FILE * fp = NULL;
    char fname[64];
    sprintf(fname,"/mnt/sdcard/yabause/intlog.txt");
    if( fp == NULL ) {
        fp = fopen(fname, "w");
    }
    if(fp){
        fprintf(fp,"\n---dynaExecute %08X----\n", GET_PC());
        fflush(fp);
    }
#endif
//  if(yabsys.frame_count == 7){
//    logenable_ = true;
//  }
//  if (logenable_) {
//    LOG("[%s] dynaExecute start %08X %08X", (is_slave_ == false) ? "M" : "S", GET_PC(), GET_PR());
//  }
  return ExecuteBlock(pBlock);
}

// One shared native entry/exit implementation for fast hits and slow lookup.
// Retain telemetry, interrupts and loop recognition in exactly the old order.
__attribute__((always_inline)) inline int DynarecSh2::ExecuteBlock(Block *pBlock) {
  const u32 native_start_cycle = GET_COUNT();
  const u32 native_start_pc = GET_PC();
#ifdef VITA_SH2_RESIDENT_LOOPS
  const u32 saved_exitcount = m_pDynaSh2->exitcount;
  if ((pBlock->flags & BLOCK_RESIDENT_LOOP) && !ScspCpuSliceIsQuiescent())
    m_pDynaSh2->exitcount = 0; // Always exit the first back edge, even on wrap.
#endif
  const bool sample_native = (native_returns & 1023u) == 0;
  const bool detail_sample = sample_native && VitaTelemetrySamplingEnabled();
  bool starts_with_load = false;
  u32 load_address = 0;
  if (detail_sample && !(native_start_pc & 1)) {
    const u32 region = native_start_pc & 0x0ff00000;
    u8 *ram = region == 0x00200000 ? LowWram : region == 0x06000000 ? HighWram : nullptr;
    if (ram) {
      const u16 instruction = T2ReadWord(ram, native_start_pc & 0xfffff);
      const u16 kind = instruction & 0xf00f;
      starts_with_load = kind >= 0x6000 && kind <= 0x6002;
      if (starts_with_load) load_address = m_pDynaSh2->GenReg[(instruction >> 4) & 15];
#ifdef VITA_STACK_PROFILE
      for (u32 a = pBlock->b_addr, k = 0; a <= pBlock->e_addr && k < 256; a += 2, ++k) {
        const u16 op = T2ReadWord(ram, a & 0xfffff);
        const bool admitted = (sh2a9::RegisterRegion::Supports(op)
#ifndef VITA_SH2_IMMEDIATE_LOGIC
            && !sh2a9::RegisterRegion::IsImmediateLogic(op)
#endif
#ifndef VITA_SH2_MAC_REGIONS
            && !sh2a9::RegisterRegion::IsMacOperation(op)
#endif
            ) || sh2a9::RegisterRegion::IsIndirectLoad(op) || sh2a9::RegisterRegion::IsPcLoad(op)
#ifdef VITA_SH2_DISP_LOADS
            || sh2a9::RegisterRegion::IsDisplacementLoad(op) || sh2a9::RegisterRegion::IsIndexedLoad(op)
#endif
#ifdef VITA_SH2_RAM_STORES
            || sh2a9::RegisterRegion::IsStore(op)
#endif
            ;
        if (admitted) ++region_ops;
        else { ++outside_ops; ++outside_hist[m_pCompiler->dsh2_instructions[op]]; }
        { /* memory access base register (loads/stores with a register base) */
          int base = -1;
          if (sh2a9::RegisterRegion::IsIndirectLoad(op) || sh2a9::RegisterRegion::IsDisplacementLoad(op) ||
              sh2a9::RegisterRegion::IsIndexedLoad(op))
            base = ((op >> 12) == 8) ? ((op >> 4) & 15) : ((op >> 4) & 15);
          else if (sh2a9::RegisterRegion::IsStore(op))
            base = ((op >> 12) == 8) ? ((op >> 4) & 15) : ((op >> 8) & 15);
          else if ((op & 0xf0ff) == 0x4022 || (op & 0xf0ff) == 0x4026) base = (op >> 8) & 15; // STS.L PR,@-Rn / LDS.L @Rm+,PR
          if (base >= 0) { ++mem_ops_total; mem_ops_r15 += base == 15; }
        }
      }
#endif
    }
  }
  const u64 native_start_time = sample_native ? YabauseGetTicks() : 0;
  bool semantic_step = false;
#if defined(DEBUG_CPU) || defined(EXECUTE_STAT)
    u32 prepc = GET_PC();
  if (is_slave_) { //statics_trigger_ == COLLECTING) {
    u64 pretime = YabauseGetTicks();
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
    compie_statics_[prepc].count++;
    compie_statics_[prepc].time += YabauseGetTicks() - pretime;
    compie_statics_[prepc].end_addr = pBlock->e_addr;
  }
  else {
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
  }
#else
#ifdef VITA_SH2_POLL_STEP
  if (!m_pCompiler->debug_mode_ && pBlock->poll_step) {
    sh2a9::PollStep::Run(pBlock->poll_step, m_pDynaSh2->GenReg,
      m_pDynaSh2->CtrlReg[0], m_pDynaSh2->SysReg[3], m_pDynaSh2->SysReg[4],
      [&](u32 address, unsigned width) -> u32 {
        const uintptr_t callback = width == 1 ? m_pDynaSh2->getmembyte :
          width == 2 ? m_pDynaSh2->getmemword : m_pDynaSh2->getmemlong;
        return reinterpret_cast<u32 (*)(u32)>(callback)(address);
      });
    semantic_step = true;
    ++poll_step_calls;
    poll_step_cycles += static_cast<u32>(GET_COUNT() - native_start_cycle);
  } else
#endif
  {
#ifdef VITA_STACK_PROFILE
    if (!is_slave_) { vt_pc_off = offsetof(Block, b_addr); vt_pc_src = &m_pDynaSh2->chain_cur; }
    m_pDynaSh2->chain_cur = reinterpret_cast<uintptr_t>(pBlock);
    VitaStackPush(VT_SH2_NATIVE);
#elif defined(A9_PMU_REGIONS)
    VitaStackPush(VT_SH2_NATIVE);
#endif
    ((dynaFunc)((void*)(pBlock->code)))(m_pDynaSh2);
#if defined(VITA_STACK_PROFILE) || defined(A9_PMU_REGIONS)
    VitaStackPop();
#endif
  }
#endif
#ifdef VITA_SH2_RESIDENT_LOOPS
  m_pDynaSh2->exitcount = saved_exitcount;
#endif
  ++native_returns;
#ifndef VITA_SH2_LEAN_STATS
  native_cycles += static_cast<u32>(GET_COUNT() - native_start_cycle);
#endif
#ifdef VITA_SH2_RESIDENT_LOOPS
  // Only that compiler tier can create BLOCK_RESIDENT_LOOP. Keep its counter
  // arithmetic out of both inlined dispatch paths when the tier is disabled.
  if (pBlock->flags & BLOCK_RESIDENT_LOOP) {
    const u32 cycles = static_cast<u32>(GET_COUNT() - native_start_cycle);
    const u32 body_cycles = (pBlock->e_addr - pBlock->b_addr) / 2;
    // All taken iterations cost body+3; only the final untaken costs body+1.
    const bool fell_through = GET_PC() == native_start_pc + body_cycles * 2 + 2;
    ++resident_calls;
    resident_cycles += cycles;
    resident_iterations += (u64(cycles) + (fell_through ? 2 : 0)) / (body_cycles + 3);
  }
#endif
  if (sample_native) {
    const u64 elapsed = YabauseGetTicks() - native_start_time;
    native_sample_us += elapsed;
    if (semantic_step) { ++poll_step_samples; poll_step_us += elapsed; }
    if (detail_sample && !semantic_step)
      hot_samples.Add(native_start_pc, is_slave_, elapsed,
        static_cast<u32>(GET_COUNT() - native_start_cycle), starts_with_load, load_address);
    ++native_samples;
  }
  
#ifdef VITA_SH2_POLL_SKIP
  const bool interrupt_pending = (GET_SR() & 0xf0) < GET_ICOUNT();
  const int result = FinishBlock(pBlock);
  if (counted_slice_active_ && !interrupt_pending && result == 0 && pBlock->poll &&
      GET_PC() == native_start_pc && ScspCpuSliceIsQuiescent() &&
      sh2a9::PollLoop::StableAddress(pBlock->poll, m_pDynaSh2->GenReg[pBlock->poll & 15])) {
    const u32 iteration = static_cast<u32>(GET_COUNT() - native_start_cycle);
    const auto skip = sh2a9::PollLoop::ComputeSkip(GET_COUNT(), memcycle_, m_pDynaSh2->exitcount, iteration);
    if (skip.cycles) {
      SET_COUNT(GET_COUNT() + skip.cycles);
      ++poll_calls;
      poll_iterations += skip.iterations;
      poll_cycles += skip.cycles;
    }
  }
  return result;
#else
  return FinishBlock(pBlock);
#endif
}

__attribute__((always_inline)) inline int DynarecSh2::FinishBlock(Block *pBlock) {
#ifdef VITA_SH2_STACK_SPEC
  if (UNLIKELY(m_pDynaSh2->spec_bail)) {  // entry validation failed, nothing executed
    m_pCompiler->SpecDeny(m_pDynaSh2->spec_bail);
    m_pDynaSh2->spec_bail = 0;
    return 0;
  }
#endif
  if ((GET_SR() & 0xF0) < GET_ICOUNT()) {
    this->CheckInterupt();
  }

  if (!m_pCompiler->debug_mode_ && (pBlock->flags&BLOCK_LOOP) ){
    if (m_pDynaSh2->SysReg[3] < pBlock->e_addr && m_pDynaSh2->SysReg[3] >= pBlock->b_addr) {
      return IN_INFINITY_LOOP;
    } else {
      return 0;
    }
  }
  return 0;
}

bool operator < (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return data1.level > data2.level;
} 
bool operator == (const dIntcTbl & data1 , const dIntcTbl & data2 )
{
  return ( data1.Vector == data2.Vector );
}

void DynarecSh2::RemoveInterrupt(u8 Vector, u8 level) {
  YabThreadLock(mtx_);
  m_IntruptTbl.remove_if([&](const dIntcTbl & n) { 
    return n.Vector == Vector; 
  });
  if (m_IntruptTbl.size() != 0) {
    m_IntruptTbl.sort();
    m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level << 4;
  }
  else {
    m_pDynaSh2->SysReg[5] = 0x0000;
  }
  YabThreadUnLock(mtx_);
}

void DynarecSh2::AddInterrupt( u8 Vector, u8 level )
{
  // Ignore Timer0 and Timer1 when masked
  //if ((Vector == 67 /*|| Vector == 68*/) && level <= ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)){
  //  LOG("Vector %d is skiped\n", Vector);
  //  return;
  //}

  dIntcTbl tmp;
  tmp.Vector = Vector;
  tmp.level  = level;

  YabThreadLock(mtx_);
  m_bIntruptSort = false;
  m_IntruptTbl.push_back(tmp);
  if( m_IntruptTbl.size() > 1 ) {
    m_IntruptTbl.sort();
    m_IntruptTbl.unique();
  }
  m_bIntruptSort = true;
  m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
  YabThreadUnLock(mtx_);
}


int DynarecSh2::CheckInterupt(){

  interruput_chk_cnt_++;

  if( m_IntruptTbl.size() == 0 ) {
    return 0;
  }

  
    
  YabThreadLock(mtx_);  
  dlstIntct::iterator pos = m_IntruptTbl.begin();
  if( InterruptRutine((*pos).Vector, (*pos).level ) != 0 ) {
    m_IntruptTbl.pop_front();
    if( m_IntruptTbl.size() != 0 ) {
      m_pDynaSh2->SysReg[5] = m_IntruptTbl.begin()->level<<4;
    }else{
      m_pDynaSh2->SysReg[5] = 0x0000;
    }
    YabThreadUnLock(mtx_);
    return 1;
  }
  YabThreadUnLock(mtx_);
  return 0;
}

int DynarecSh2::InterruptRutine(u8 Vector, u8 level)
{
  if (((u32)level) > ((m_pDynaSh2->CtrlReg[0] >> 4) & 0x0F)) {

    u32 prepc = m_pDynaSh2->SysReg[3];

    interruput_cnt_++;
    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->CtrlReg[0],NULL);
    m_pDynaSh2->GenReg[15] -= 4;
    MappedMemoryWriteLong(m_pDynaSh2->GenReg[15], m_pDynaSh2->SysReg[3],NULL);
    if (level == 0x10) { //NMI
      m_pDynaSh2->CtrlReg[0] |= 0x000000F0;
    }
    else {
      m_pDynaSh2->CtrlReg[0] &= ~0x000000F0;
      m_pDynaSh2->CtrlReg[0] |= ((u32)(level << 4) & 0x000000F0);
    }
    m_pDynaSh2->SysReg[3] = memGetLong(m_pDynaSh2->CtrlReg[2] + (((u32)Vector) << 2));

    //LOG("**** [%s] Exception vecnum=%s(%x), PC=%08X to %08X, level=%08X\n", (is_slave_ == false) ? "M" : "S", ScuGetVectorString(Vector), Vector,prepc, m_pDynaSh2->SysReg[3], level);

#if defined(DEBUG_CPU)
//    LOG("**** [%s] Exception vecnum=%u, PC=%08X to %08X, level=%08X\n", (is_slave_==false)?"M":"S", Vector, prepc, m_pDynaSh2->SysReg[3], level);
#endif
    return 1;
  }
  return 0; 
}


int DynarecSh2GetDisasmebleString(string & out, u32 from, u32 to) {
  char linebuf[128];
  if (from > to) return -1;
  for (u32 i = from; i < (to+2); i += 2) {
    SH2Disasm(i, MappedMemoryReadWord(i,NULL), 0, NULL, linebuf);
    out += linebuf;
    out += "\n";
  }
  return 0;
}

int DynarecSh2::Resume() {
  statics_trigger_ = NORMAL;
  return 0;
}

void DynarecSh2::ShowStatics(){
#if defined(DEBUG_CPU)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() - pre_cnt_, loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  pre_cnt_ = GET_COUNT();
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

  switch (statics_trigger_) {
  case NORMAL:
    break;
  case REQUESTED:
    statics_trigger_ = COLLECTING;
    break;
  case COLLECTING:
    statics_trigger_ = FINISHED;
    while (FINISHED == statics_trigger_) {
      YabThreadUSleep(10000);
    }
    break;
  case FINISHED:
    break;
  }
#elif defined(EXECUTE_STAT)
  LOG("\nExec cnt %d loopskip_cnt_ = %d, interruput_chk_cnt_ = %d, interruput_cnt_ = %d\n", GET_COUNT() , loopskip_cnt_, interruput_chk_cnt_, interruput_cnt_ );
  interruput_chk_cnt_ = 0;
  interruput_cnt_ = 0;
  loopskip_cnt_ = 0;

for( auto i = compie_statics_.begin(); i != compie_statics_.end() ; ++i ) {
    if(i->second.time>100) LOG("%08X\t%d\t%d",i->first,i->second.count, i->second.time);
  }
compie_statics_.clear();
#endif
}

int DynarecSh2::GetCurrentStatics(MapCompileStatics & buf){
#if !defined(DEBUG_CPU)
  return -1;
#else

  if (statics_trigger_ != NORMAL && statics_trigger_ != FINISHED) return -1;

  statics_trigger_ = REQUESTED;
  while (statics_trigger_!= FINISHED) {
    YabThreadUSleep(10000);
  }
  
  buf = compie_statics_;
  compie_statics_.clear();
#endif
  return 0;
}

void DynarecSh2::ShowCompileInfo(){
  int i = 0;
  while (asm_list[i].func != NULL) {
    printf("%s: %d\n", opcode_list[i].mnem, asm_list[i].build_count);
    i++;
  }
}

void DynarecSh2::ResetCompileInfo() {
  int i = 0;
  while (asm_list[i].func != NULL) {
    asm_list[i].build_count = 0;
    i++;
  }
}
