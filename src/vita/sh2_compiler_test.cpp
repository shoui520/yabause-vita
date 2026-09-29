/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core.h"
#include "memory.h"
#include "sh2core.h"
#include "sh2_dynarec/DynarecSh2.h"
#include "sh2_dynarec/poll_step.h"
#include "sh2_dynarec/a9_register_region.h"
#include <cstring>
#include <new>

extern "C" void YuiMsg(const char *, ...);

static unsigned load_calls;
static u32 TestRead(u32 address, unsigned width) {
  ++load_calls;
  u8 *ram = (address & 0xdff00000) == 0x00200000 ? LowWram :
            (address & 0xdff00000) == 0x06000000 ? HighWram : nullptr;
  if (!ram) return 0x89abcdef; // Synthetic peripheral: never touch real MMIO.
  const u32 offset = address & 0xfffff;
  return width == 1 ? T2ReadByte(ram, offset) :
         width == 2 ? T2ReadWord(ram, offset) : T2ReadLong(ram, offset);
}
static u32 TestByte(u32 a) { return TestRead(a, 1) & 255; }
static u32 TestWord(u32 a) { return TestRead(a, 2) & 65535; }
static u32 TestLong(u32 a) { return TestRead(a, 4); }

#ifdef VITA_SH2_POLL_SKIP
static unsigned poll_memory_cycles;
static u32 PollByte(u32 a) {
  DynarecSh2::CurrentContext->memcycle_ += poll_memory_cycles;
  return TestByte(a);
}
static u32 PollWord(u32 a) {
  DynarecSh2::CurrentContext->memcycle_ += poll_memory_cycles;
  return TestWord(a);
}
#endif

/* Startup-only check before guest execution. Preserve the scratch RAM exactly.
 * SH7604 manual tables 2.15/2.16: BRA/BSR execute the delay slot; BSR additionally
 * saves the address after that slot in PR. No peripheral accesses in this test.
 * This deliberately uses CompileBlock, including decode, patching and VM publish.
 */
extern "C" int VitaSh2CompilerTest(void) {
  constexpr u32 offset = 0xff000, pc = 0x06000000 + offset;
  struct Restore {
    u8 bytes[0x480];
    u8 low[32];
    Restore() {
      std::memcpy(bytes, HighWram + offset, sizeof(bytes));
      std::memcpy(low, LowWram + offset, sizeof(low));
    }
    ~Restore() {
      std::memcpy(HighWram + offset, bytes, sizeof(bytes));
      std::memcpy(LowWram + offset, low, sizeof(low));
    }
  } restore;
  try {
    {
      struct RestoreContext {
        DynarecSh2 *saved=DynarecSh2::CurrentContext;
        ~RestoreContext() { DynarecSh2::CurrentContext=saved; }
      } restore_context;
      DynarecSh2 cpu;
      cpu.SetCurrentContext();
      unsigned cases=0;
      for(unsigned bank=0;bank<2;++bank) {
        u8 *ram=bank ? HighWram : LowWram;
        u32 base=bank ? pc : 0x00200000;
        unsigned start=bank ? offset : 0;
        for(unsigned i=0;i<32;++i) T2WriteByte(ram,start+i,(i*71+137)&255);
        for(unsigned alias=0;alias<2;++alias) for(unsigned i=0;i<32;i+=4)
          for(unsigned width=1;width<=4;width*=2) {
            u32 addr=(base+i)|(alias ? 0x20000000 : 0);
            u32 expected=width==1 ? T2ReadByte(ram,start+i) :
                width==2 ? T2ReadWord(ram,start+i) : T2ReadLong(ram,start+i);
            cpu.memcycle_=17;
            u32 actual=width==1 ? memGetByte(addr) : width==2 ? memGetWord(addr) : memGetLong(addr);
            u32 cycles=17+(alias ? (bank ? 2 : 4) : 0);
            if(actual!=expected || cpu.memcycle_!=cycles) {
              YuiMsg("jit_read_helper_failed addr=%08x width=%u value=%08x expected=%08x cycles=%u",
                     addr,width,actual,expected,cpu.memcycle_);
              return -1;
            }
            ++cases;
          }
      }
      /* BIOS reads exercise all three mapped fallbacks; FTCSR exercises the
       * emulated on-chip byte handler without changing or caching its state. */
      for(unsigned width=1;width<=4;width*=2) {
        u32 cycles=0;
        u32 expected=width==1 ? MappedMemoryReadByte(0x20,&cycles) :
            width==2 ? MappedMemoryReadWord(0x20,&cycles) : MappedMemoryReadLong(0x20,&cycles);
        cpu.memcycle_=17;
        u32 actual=width==1 ? memGetByte(0x20) : width==2 ? memGetWord(0x20) : memGetLong(0x20);
        if(actual!=expected || cpu.memcycle_!=17+cycles) return -1;
        ++cases;
      }
      u32 cycles=0;
      u8 expected=MappedMemoryReadByte(0xfffffe11,&cycles);
      cpu.memcycle_=17;
      if(memGetByte(0xfffffe11)!=expected || cpu.memcycle_!=17+cycles) return -1;
      YuiMsg("jit_read_helper_pass cases=%u",cases+1);
    }
    CompileBlocks *compiler = CompileBlocks::getInstance();
    for (unsigned call = 0; call < 10; ++call) {
      const bool conditional = call >= 2;
      const bool delayed = call < 2 || call >= 6;
      const unsigned t = conditional ? (call & 1) : 1;
      const bool taken = !conditional || ((call < 4 || (call >= 6 && call < 8)) ? t != 0 : t == 0);
      const u16 branch = call == 0 ? 0xa001 : call == 1 ? 0xb001 :
                         call < 4 ? 0x8901 : call < 6 ? 0x8b01 : call < 8 ? 0x8d01 : 0x8f01;
      const u16 program[] = {
        0xe005, // mov #5,r0
        0x70ff, // add #-1,r0
        0x4008, // shll2 r0
        branch, // BRA/BSR/BT/BF to pc+12
        0x7003, // add #3,r0 (delay slot)
        0xe07f, // must not execute
        0x0009, // branch target, outside this block
      };
      for (unsigned i = 0; i < sizeof(program)/sizeof(program[0]); ++i)
        T2WriteWord(HighWram, offset + i * 2, program[i]);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block) { YuiMsg("jit_compiler_test_failed compile=%u", call); return -1; }
      tagSH2 actual = {};
      for (unsigned i = 0; i < 16; ++i) actual.GenReg[i] = 0x12340000 + i;
      actual.CtrlReg[0] = 0x3f2 | t;
      actual.SysReg[2] = 0xdead0000;
      actual.SysReg[3] = pc;
      tagSH2 expected = actual;
      expected.GenReg[0] = delayed ? 19 : 16;
      expected.SysReg[3] = pc + (taken ? 12 : delayed ? 10 : 8);
      expected.SysReg[4] = taken ? 6 : delayed ? 5 : 4;
      if (call == 1) expected.SysReg[2] = pc + 10;
      reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
      if (std::memcmp(&actual, &expected, sizeof(actual))) {
        YuiMsg("jit_compiler_test_failed case=%u r0=%08x pc=%08x pr=%08x cycles=%u sr=%08x",
          call, actual.GenReg[0], actual.SysReg[3], actual.SysReg[2],
          actual.SysReg[4], actual.CtrlReg[0]);
        return -1;
      }
    }
#ifdef VITA_SH2_DEFER_SEPARATORS
    { /* Differential: identical programs compiled with per-instruction
       * separators and with deferred ones must leave identical state/memory. */
      extern bool g_sh2_defer_separators;
      struct Toggle { ~Toggle() { g_sh2_defer_separators = true; } } toggle;
      constexpr u32 scratch = offset + 256;
      static u8 *scratch_ram; scratch_ram = HighWram;
      struct W { static void B(u32 a, u32 v) { a &= 0xfffff; if (a >= scratch && a < scratch + 64) T2WriteByte(scratch_ram, a, v); }
                 static void Wd(u32 a, u32 v) { a &= 0xfffff; if (a >= scratch && a + 1 < scratch + 64) T2WriteWord(scratch_ram, a, v); }
                 static void L(u32 a, u32 v) { a &= 0xfffff; if (a >= scratch && a + 3 < scratch + 64) T2WriteLong(scratch_ram, a, v); } };
      u32 seed = 0x1234567u;
      auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
      static const unsigned dsts[] = {0, 4, 5, 6}, ptrs[] = {1, 2, 3};
      unsigned defer_cases = 0;
      u8 init_code[128], init_scratch[64];
      for (unsigned c = 0; c < 400; ++c) {
        u16 program[16]; unsigned n = 0;
        const unsigned body = 1 + rnd() % 11;
        for (unsigned k = 0; k < body; ++k) {
          const unsigned d = dsts[rnd() % 4], pr = ptrs[rnd() % 3], sz = rnd() % 3;
          switch (rnd() % 9) {
            case 0: program[n++] = 0x6000 | d << 8 | pr << 4 | (sz == 0 ? 2 : sz == 1 ? 1 : 0); break; // MOV.x @Rp,Rd
            case 1: program[n++] = 0x2000 | pr << 8 | d << 4 | (sz == 0 ? 2 : sz == 1 ? 1 : 0); break; // MOV.x Rd,@Rp
            case 2: program[n++] = 0x9000 | d << 8 | (rnd() & 15); break;                           // MOV.W @(d,PC)
            case 3: program[n++] = 0xd000 | d << 8 | (rnd() & 15); break;                           // MOV.L @(d,PC)
            case 4: program[n++] = 0xc700 | (rnd() & 15); break;                                    // MOVA
            case 5: program[n++] = 0x300c | d << 8 | dsts[rnd() % 4] << 4; break;                   // ADD Rm,Rn
            case 6: program[n++] = 0xe000 | d << 8 | (rnd() & 255); break;                          // MOV #i
            case 7: program[n++] = 0x4008 | d << 8; break;                                          // SHLL2
            default: program[n++] = 0x0009; break;                                                 // NOP
          }
        }
        switch (rnd() % 3) {
          case 0: program[n++] = 0x000b; program[n++] = 0x0009; break; // RTS; NOP
          case 1: program[n++] = 0x8902; program[n++] = 0x0009; break; // BT +2
          default: program[n++] = 0x8b01; program[n++] = 0x0009; break; // BF +1
        }
        for (unsigned i = 0; i < sizeof(init_code); i += 2) T2WriteWord(HighWram, offset + i, (u16)rnd());
        for (unsigned i = 0; i < n; ++i) T2WriteWord(HighWram, offset + i * 2, program[i]);
        for (unsigned i = 0; i < 64; i += 4) T2WriteLong(HighWram, scratch + i, rnd());
        std::memcpy(init_code, HighWram + offset, sizeof(init_code));
        std::memcpy(init_scratch, HighWram + scratch, sizeof(init_scratch));
        tagSH2 start = {};
        for (unsigned i = 0; i < 16; ++i) start.GenReg[i] = rnd();
        for (unsigned i = 1; i <= 3; ++i) start.GenReg[i] = 0x06000000 + scratch + 16 * (i - 1) + 4 * (rnd() % 3);
        start.CtrlReg[0] = 0x3f0 | (rnd() & 1);
        start.SysReg[2] = 0x06001230; start.SysReg[3] = pc; start.SysReg[4] = rnd() & 0xffff;
        start.getmembyte = reinterpret_cast<uintptr_t>(&TestByte);
        start.getmemword = reinterpret_cast<uintptr_t>(&TestWord);
        start.getmemlong = reinterpret_cast<uintptr_t>(&TestLong);
        start.setmembyte = reinterpret_cast<uintptr_t>(&W::B);
        start.setmemword = reinterpret_cast<uintptr_t>(&W::Wd);
        start.setmemlong = reinterpret_cast<uintptr_t>(&W::L);
        tagSH2 result[2]; u8 memory[2][64];
        for (unsigned mode = 0; mode < 2; ++mode) {
          std::memcpy(HighWram + offset, init_code, sizeof(init_code));
          std::memcpy(HighWram + scratch, init_scratch, sizeof(init_scratch));
          g_sh2_defer_separators = mode != 0;
          Block *block = compiler->CompileBlock(pc, nullptr);
          if (!block) { YuiMsg("jit_defer_test_failed compile case=%u", c); return -1; }
          result[mode] = start;
          reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&result[mode]);
          std::memcpy(memory[mode], HighWram + scratch, 64);
        }
        if (std::memcmp(&result[0], &result[1], sizeof(tagSH2)) || std::memcmp(memory[0], memory[1], 64)) {
          YuiMsg("jit_defer_test_failed case=%u pc=%08x/%08x cycles=%u/%u r0=%08x/%08x", c,
            result[0].SysReg[3], result[1].SysReg[3], result[0].SysReg[4], result[1].SysReg[4],
            result[0].GenReg[0], result[1].GenReg[0]);
          return -1;
        }
        ++defer_cases;
      }
      YuiMsg("jit_defer_test_pass cases=%u", defer_cases);
    }
#endif
#ifdef VITA_SH2_RAM_STORES
    { /* Differential: the same programs compiled as high-RAM blocks (owner
       * tracking on) with region stores and with the original templates must
       * leave identical state and memory. Scratch is either in the code's own
       * KiB (owned: helper path) or the next KiB (unowned: inline path). */
      extern bool g_sh2_region_stores;
      struct Toggle { ~Toggle() { g_sh2_region_stores = true; } } toggle;
      static u32 scratch; static u8 *scratch_ram; scratch_ram = HighWram;
      struct W { static bool In(u32 a, unsigned w) { a &= 0xfffff; return a >= scratch && a + w <= scratch + 128; }
                 static void B(u32 a, u32 v) { if (In(a, 1)) T2WriteByte(scratch_ram, a & 0xfffff, v); }
                 static void Wd(u32 a, u32 v) { if (In(a, 2)) T2WriteWord(scratch_ram, a & 0xfffff, v); }
                 static void L(u32 a, u32 v) { if (In(a, 4)) T2WriteLong(scratch_ram, a & 0xfffff, v); } };
      u32 seed = 0x5354524fu;
      auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
      // R0 is the index for @(R0,Rn) forms: a small multiple of 4, never a
      // destination, so every access stays inside this test's scratch.
      static const unsigned dsts[] = {4, 5, 6, 7}, ptrs[] = {1, 2, 3};
      unsigned store_cases = 0;
      u8 init_code[128], init_scratch[128];
      for (unsigned c = 0; c < 600; ++c) {
        scratch = (c & 1) ? offset + 0x400 : offset + 256;
        u16 program[20]; unsigned n = 0;
        const unsigned body = 1 + rnd() % 15;
        for (unsigned k = 0; k < body; ++k) {
          const unsigned d = dsts[rnd() % 4], pr = ptrs[rnd() % 3], sz = rnd() % 3;
          const unsigned sizes = sz == 0 ? 2 : sz == 1 ? 1 : 0;
          static const u16 shifts[] = {0x4000, 0x4001, 0x4020, 0x4021};
          switch (rnd() % 16) {
            case 0: program[n++] = 0x6000 | d << 8 | pr << 4 | sizes; break;          // MOV.x @Rp,Rd
            case 1: program[n++] = 0x2000 | pr << 8 | d << 4 | sizes; break;          // MOV.x Rd,@Rp
            case 2: program[n++] = 0x1000 | pr << 8 | d << 4 | (rnd() % 4); break;    // MOV.L Rd,@(disp,Rp)
            case 3: program[n++] = 0x8000 | pr << 4 | (rnd() % 16); break;            // MOV.B R0,@(disp,Rp)
            case 4: program[n++] = 0x8100 | pr << 4 | (rnd() % 8); break;             // MOV.W R0,@(disp,Rp)
            case 5: program[n++] = 0x5000 | d << 8 | pr << 4 | (rnd() % 4); break;    // MOV.L @(disp,Rp),Rd
            case 6: program[n++] = 0x000c | d << 8 | pr << 4 | (2 - sizes); break;    // MOV.x @(R0,Rp),Rd
            case 7: program[n++] = 0x0004 | pr << 8 | d << 4 | sizes; break;          // MOV.x Rd,@(R0,Rp)
            case 8: program[n++] = 0x2004 | pr << 8 | (rnd() & 1 ? pr : d) << 4 | sizes; break; // MOV.x Rm,@-Rp
            case 9: program[n++] = 0x300c | d << 8 | dsts[rnd() % 4] << 4; break;     // ADD Rm,Rn
            case 10: program[n++] = 0xe000 | d << 8 | (rnd() & 255); break;          // MOV #i
            case 11: program[n++] = 0x4008 | d << 8; break;                          // SHLL2
            case 12: case 13: program[n++] = shifts[rnd() % 4] | d << 8; break;      // SHLL/SHLR/SHAL/SHAR
            case 14: program[n++] = 0x0029 | d << 8; break;                          // MOVT
            default: program[n++] = 0x0009; break;                                   // NOP
          }
        }
        switch (rnd() % 3) {
          case 0: program[n++] = 0x000b; program[n++] = 0x0009; break; // RTS; NOP
          case 1: program[n++] = 0x8902; program[n++] = 0x0009; break; // BT +2
          default: program[n++] = 0x8b01; program[n++] = 0x0009; break; // BF +1
        }
        for (unsigned i = 0; i < sizeof(init_code); i += 2) T2WriteWord(HighWram, offset + i, (u16)rnd());
        for (unsigned i = 0; i < n; ++i) T2WriteWord(HighWram, offset + i * 2, program[i]);
        for (unsigned i = 0; i < 128; i += 4) T2WriteLong(HighWram, scratch + i, rnd());
        std::memcpy(init_code, HighWram + offset, sizeof(init_code));
        std::memcpy(init_scratch, HighWram + scratch, sizeof(init_scratch));
        tagSH2 start = {};
        for (unsigned i = 0; i < 16; ++i) start.GenReg[i] = rnd();
        // Pointers leave room for up to 15 pre-decrements below and index 12 above.
        for (unsigned i = 1; i <= 3; ++i) start.GenReg[i] = 0x06000000 + scratch + 64 + 16 * (i - 1) + 4 * (rnd() % 3);
        start.GenReg[0] = 4 * (rnd() % 4);
        start.CtrlReg[0] = 0x3f0 | (rnd() & 1);
        start.SysReg[2] = 0x06001230; start.SysReg[3] = pc; start.SysReg[4] = rnd() & 0xffff;
        start.getmembyte = reinterpret_cast<uintptr_t>(&TestByte);
        start.getmemword = reinterpret_cast<uintptr_t>(&TestWord);
        start.getmemlong = reinterpret_cast<uintptr_t>(&TestLong);
        start.setmembyte = reinterpret_cast<uintptr_t>(&W::B);
        start.setmemword = reinterpret_cast<uintptr_t>(&W::Wd);
        start.setmemlong = reinterpret_cast<uintptr_t>(&W::L);
        tagSH2 result[2]; u8 memory[2][128];
        for (unsigned mode = 0; mode < 2; ++mode) {
          std::memcpy(HighWram + offset, init_code, sizeof(init_code));
          std::memcpy(HighWram + scratch, init_scratch, sizeof(init_scratch));
          g_sh2_region_stores = mode != 0;
          Block *block = compiler->CompileBlock(pc, compiler->LookupParentTable);
          if (!block) { YuiMsg("jit_store_test_failed compile case=%u", c); return -1; }
          result[mode] = start;
          reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&result[mode]);
          std::memcpy(memory[mode], HighWram + scratch, 128);
          for (u32 i = 0; i < 64; ++i) compiler->LookupParentTable[adress_mask(pc) + i].clear();
        }
        if (std::memcmp(&result[0], &result[1], sizeof(tagSH2)) || std::memcmp(memory[0], memory[1], 128)) {
          YuiMsg("jit_store_test_failed case=%u pc=%08x/%08x cycles=%u/%u r0=%08x/%08x", c,
            result[0].SysReg[3], result[1].SysReg[3], result[0].SysReg[4], result[1].SysReg[4],
            result[0].GenReg[0], result[1].GenReg[0]);
          return -1;
        }
        ++store_cases;
      }
      // Owners were only this test's; the KiB is unowned again.
      bool owned = false;
      for (u32 i = 0; i < 512; ++i)
        owned |= compiler->LookupParentTable[(adress_mask(pc) & ~511u) + i].size() != 0;
      if (!owned) compiler->code_pages[adress_mask(pc) >> 9] = 0;
      YuiMsg("jit_store_test_pass cases=%u", store_cases);
    }
#endif
#ifdef VITA_SH2_STACK_SPEC
    { /* Differential: stack-heavy programs compiled as high-RAM blocks with and
       * without block-entry validated R15 speculation leave identical state and
       * memory; an invalid R15 must return before executing anything. */
      constexpr u32 stack_off = 0x80000;               // 0x06080000, inside the validation window
      struct StackRestore { u8 bytes[0x800]; StackRestore() { std::memcpy(bytes, HighWram + 0x80000 - 0x400, sizeof(bytes)); }
                            ~StackRestore() { std::memcpy(HighWram + 0x80000 - 0x400, bytes, sizeof(bytes)); } } stack_restore;
      struct Toggle { ~Toggle() { sh2a9::RegisterRegion::stack_spec_enabled = true; } } toggle;
      u32 seed = 0x53545350u;
      auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
      unsigned spec_cases = 0, spec_bails = 0;
      u8 init_code[128], init_stack[0x800];
      for (unsigned c = 0; c < 400; ++c) {
        u16 program[40]; unsigned n = 0;
        const unsigned body = 2 + rnd() % 30;
        for (unsigned k = 0; k < body; ++k) {
          const unsigned a = 1 + rnd() % 6, b = 1 + rnd() % 6, d = rnd() % 16;
          switch (rnd() % 10) {
            case 0: program[n++] = 0x2f04 | a << 4 | (rnd() % 3); break;           // MOV.x Ra,@-R15
            case 1: program[n++] = 0x60f4 | a << 8 | (rnd() % 3); break;           // MOV.x @R15+,Ra
            case 2: program[n++] = 0x50f0 | a << 8 | d; break;                     // MOV.L @(d,R15),Ra
            case 3: program[n++] = 0x1f00 | a << 4 | d; break;                     // MOV.L Ra,@(d,R15)
            case 4: program[n++] = (rnd() & 1 ? 0x85f0 : 0x84f0) | d; break;        // MOV.W/B @(d,R15),R0
            case 5: program[n++] = (rnd() & 1 ? 0x81f0 : 0x80f0) | d; break;        // MOV.W/B R0,@(d,R15)
            case 6: program[n++] = 0x7f00 | u8(s8(4 * (int(rnd() % 9) - 4))); break; // ADD #imm,R15
            case 7: program[n++] = 0x300c | a << 8 | b << 4; break;                // ADD Rb,Ra
            case 8: program[n++] = 0x4f22; break;                                  // STS.L PR,@-R15 (template)
            default: program[n++] = 0x62f2 | a << 8; break;                        // MOV.L @R15,Ra
          }
        }
        program[n++] = 0x000b; program[n++] = 0x0009;                            // RTS; NOP
        for (unsigned i = 0; i < sizeof(init_code); i += 2) T2WriteWord(HighWram, offset + i, (u16)rnd());
        for (unsigned i = 0; i < n; ++i) T2WriteWord(HighWram, offset + i * 2, program[i]);
        for (unsigned i = 0; i < sizeof(init_stack); i += 4) T2WriteLong(HighWram, stack_off - 0x400 + i, rnd());
        std::memcpy(init_code, HighWram + offset, sizeof(init_code));
        std::memcpy(init_stack, HighWram + stack_off - 0x400, sizeof(init_stack));
        tagSH2 start = {};
        for (unsigned i = 0; i < 16; ++i) start.GenReg[i] = rnd();
        start.GenReg[15] = 0x06000000 + stack_off + 4 * (rnd() % 64);
        start.CtrlReg[0] = 0x3f0 | (rnd() & 1);
        start.SysReg[2] = 0x06001230; start.SysReg[3] = pc; start.SysReg[4] = rnd() & 0xffff;
        start.getmembyte = reinterpret_cast<uintptr_t>(&TestByte);
        start.getmemword = reinterpret_cast<uintptr_t>(&TestWord);
        start.getmemlong = reinterpret_cast<uintptr_t>(&TestLong);
        struct SW { static bool In(u32 a, unsigned w) { a &= 0xfffff; return a >= stack_off - 0x400 && a + w <= stack_off + 0x400; }
                    static void B(u32 a, u32 v) { if (In(a, 1)) T2WriteByte(HighWram, a & 0xfffff, v); }
                    static void Wd(u32 a, u32 v) { if (In(a, 2)) T2WriteWord(HighWram, a & 0xfffff, v); }
                    static void L(u32 a, u32 v) { if (In(a, 4)) T2WriteLong(HighWram, a & 0xfffff, v); } };
        start.setmembyte = reinterpret_cast<uintptr_t>(&SW::B);
        start.setmemword = reinterpret_cast<uintptr_t>(&SW::Wd);
        start.setmemlong = reinterpret_cast<uintptr_t>(&SW::L);
        start.spec_high = reinterpret_cast<uintptr_t>(HighWram);
        start.spec_pages = reinterpret_cast<uintptr_t>(compiler->code_pages);
        tagSH2 result[2]; static u8 memory[2][0x800];
        // Every 4th case: compiled code in a KiB at or next to the stack. Its
        // blocks must bail (nothing executed) when a planned store touches it.
        const int marked = c % 4 == 3 ? int(stack_off >> 10) + int(rnd() % 3) - 1 : -1;
        if (marked >= 0) compiler->code_pages[marked] = 1;
        for (unsigned mode = 0; mode < 2; ++mode) {
          std::memcpy(HighWram + offset, init_code, sizeof(init_code));
          std::memcpy(HighWram + stack_off - 0x400, init_stack, sizeof(init_stack));
          sh2a9::RegisterRegion::stack_spec_enabled = mode != 0;
          compiler->spec_deny.clear();
          Block *block = compiler->CompileBlock(pc, compiler->LookupParentTable);
          if (!block) { YuiMsg("jit_stack_spec_test_failed compile case=%u", c); return -1; }
          result[mode] = start;
          reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&result[mode]);
          if (result[mode].spec_bail) {             // like the dispatcher: deny, recompile, rerun
            tagSH2 untouched = result[mode]; untouched.spec_bail = 0;
            if (result[mode].spec_bail != pc || std::memcmp(&untouched, &start, sizeof(tagSH2)) ||
                std::memcmp(HighWram + stack_off - 0x400, init_stack, sizeof(init_stack))) {
              YuiMsg("jit_stack_spec_test_failed bail_side_effect case=%u", c); return -1;
            }
            ++spec_bails;
            for (u32 i = 0; i < 64; ++i) compiler->LookupParentTable[adress_mask(pc) + i].clear();
            compiler->SpecDeny(pc);
            block = compiler->CompileBlock(pc, compiler->LookupParentTable);
            if (!block) { YuiMsg("jit_stack_spec_test_failed recompile case=%u", c); return -1; }
            result[mode] = start;
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&result[mode]);
          }
          std::memcpy(memory[mode], HighWram + stack_off - 0x400, sizeof(memory[mode]));
          for (u32 i = 0; i < 64; ++i) compiler->LookupParentTable[adress_mask(pc) + i].clear();
        }
        if (marked >= 0) compiler->code_pages[marked] = 0;
        if (std::memcmp(&result[0], &result[1], sizeof(tagSH2)) || std::memcmp(memory[0], memory[1], sizeof(memory[0]))) {
          YuiMsg("jit_stack_spec_test_failed case=%u pc=%08x/%08x r15=%08x/%08x bail=%08x", c,
            result[0].SysReg[3], result[1].SysReg[3], result[0].GenReg[15], result[1].GenReg[15], result[1].spec_bail);
          return -1;
        }
        ++spec_cases;
      }
      { /* Invalid R15: the validated block must return with nothing executed. */
        const u16 prog[] = {0x2f16, 0x51f1, 0x7f08, 0x000b, 0x0009};
        for (unsigned i = 0; i < 5; ++i) T2WriteWord(HighWram, offset + i * 2, prog[i]);
        compiler->spec_deny.clear();
        Block *block = compiler->CompileBlock(pc, compiler->LookupParentTable);
        tagSH2 st = {}, before;
        for (unsigned i = 0; i < 16; ++i) st.GenReg[i] = 0x1111 * i;
        st.GenReg[15] = 0x05a00100; st.SysReg[3] = pc; st.SysReg[4] = 77;
        before = st;
        reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&st);
        const bool ok = st.spec_bail == pc && (st.spec_bail = 0, !std::memcmp(&st, &before, sizeof(st)));
        for (u32 i = 0; i < 64; ++i) compiler->LookupParentTable[adress_mask(pc) + i].clear();
        if (!ok) { YuiMsg("jit_stack_spec_test_failed bail pc=%08x", st.SysReg[3]); return -1; }
      }
      compiler->spec_deny.clear();
      bool owned = false;
      for (u32 i = 0; i < 512; ++i)
        owned |= compiler->LookupParentTable[(adress_mask(pc) & ~511u) + i].size() != 0;
      if (!owned) compiler->code_pages[adress_mask(pc) >> 9] = 0;
      YuiMsg("jit_stack_spec_test_pass cases=%u bails=%u", spec_cases, spec_bails);
    }
#endif
#ifdef VITA_SH2_NATIVE_DISPATCH
    { /* sh2_dispatch: A -> B -> C (BRA-linked high-RAM blocks) chained in one
       * native call must equal C-driven single-block execution, and each exit
       * condition must stop the chain at exactly the right block boundary. */
      static const u16 progA[] = {0xe101, 0x7102, 0xa01c, 0x0009};  // @pc
      static const u16 progB[] = {0x7103, 0x4100, 0xa01c, 0x0009};  // @pc+0x40
      static const u16 progC[] = {0x7205, 0x000b, 0x0009};          // @pc+0x80, RTS to PR
      u8 saved[0xc0]; std::memcpy(saved, HighWram + offset, sizeof(saved));
      for (unsigned i = 0; i < 4; ++i) T2WriteWord(HighWram, offset + i * 2, progA[i]);
      for (unsigned i = 0; i < 4; ++i) T2WriteWord(HighWram, offset + 0x40 + i * 2, progB[i]);
      for (unsigned i = 0; i < 3; ++i) T2WriteWord(HighWram, offset + 0x80 + i * 2, progC[i]);
      Block *blk[3];
      for (unsigned b = 0; b < 3; ++b) {
        blk[b] = compiler->CompileBlock(pc + b * 0x40, compiler->LookupParentTable);
        if (!blk[b]) { YuiMsg("jit_dispatch_test_failed compile %u", b); return -1; }
        compiler->SetHigh((offset + b * 0x40) >> 1, blk[b]);
      }
      tagSH2 start = {};
      for (unsigned i = 0; i < 16; ++i) start.GenReg[i] = 0x100 * i;
      start.CtrlReg[0] = 0xf0; start.SysReg[2] = 0x00001230; start.SysReg[3] = pc; start.SysReg[5] = 0; // PR outside high RAM
      start.exitcount = 100000;
      start.chain_table = reinterpret_cast<uintptr_t>(compiler->LookupTable);
      // Reference: C-driven, one block per call.
      tagSH2 ref[4]; ref[0] = start;
      for (unsigned b = 0; b < 3; ++b) {
        ref[b + 1] = ref[b];
        reinterpret_cast<void (*)(tagSH2 *)>(blk[b]->code)(&ref[b + 1]);
      }
      auto same = [](const tagSH2 &x, const tagSH2 &y) {
        return !std::memcmp(x.GenReg, y.GenReg, sizeof(x.GenReg)) && !std::memcmp(x.CtrlReg, y.CtrlReg, sizeof(x.CtrlReg)) &&
               !std::memcmp(x.SysReg, y.SysReg, sizeof(x.SysReg));
      };
      bool ok = ref[1].SysReg[3] == pc + 0x40 && ref[2].SysReg[3] == pc + 0x80 && ref[3].SysReg[3] == 0x00001230;
      // Chained runs: {budget, exitcount, memcycle, pending level, loop flag on A} -> blocks run.
      struct Case { u32 budget, exitcount, memcycle, level; bool loop; unsigned expect; } cases[] = {
        {100, 100000, 0, 0, false, 3}, {1, 100000, 0, 0, false, 2}, {0, 100000, 0, 0, false, 1},
        {100, ref[1].SysReg[4], 0, 0, false, 1}, {100, ref[2].SysReg[4], 0, 0, false, 2},
#ifdef VITA_SH2_DISPATCH_MEMCYCLE
        // Memory cycles are folded into the count and the chain continues
        // while the sum stays below the target.
        {100, 100000, 1, 0, false, 3}, {100, ref[1].SysReg[4] + 5, 5, 0, false, 1},
        {100, ref[1].SysReg[4] + 6, 5, 0, false, 2},
#else
        {100, 100000, 1, 0, false, 1},
#endif
        {100, 100000, 0, 0x100, false, 1},
        // No BLOCK_LOOP cases: loop blocks never link their exits (EmmitCode),
        // so link_check has no loop test and setting the flag on the linked
        // blocks here would describe an unreachable state.
      };
      for (const Case &k : cases) {
        tagSH2 st = start;
        st.dispatch = reinterpret_cast<uintptr_t>(&sh2_dispatch);
        st.chain_budget = k.budget; st.exitcount = k.exitcount; st.SysReg[5] = k.level;
        st.chain_cur = reinterpret_cast<uintptr_t>(blk[0]);
        st.memcycle = k.memcycle;
        if (k.loop) blk[0]->flags |= BLOCK_LOOP;
        reinterpret_cast<void (*)(tagSH2 *)>(blk[0]->code)(&st);
        if (k.loop) blk[0]->flags &= ~BLOCK_LOOP;
        // Memory cycles of the first block are folded into the count once.
        tagSH2 want = ref[k.expect]; want.SysReg[5] = k.level; 
#ifdef VITA_SH2_DISPATCH_MEMCYCLE
        const bool folded = k.memcycle && !k.loop && k.budget != 0;
        if (folded) want.SysReg[4] += k.memcycle;
        const bool mem_ok = st.memcycle == (folded ? 0 : k.memcycle);
#else
        const bool mem_ok = st.memcycle == k.memcycle;
#endif
        const u32 ran = k.budget - st.chain_budget + 1;
        if (!same(st, want) || ran != k.expect || !mem_ok || st.chain_cur != reinterpret_cast<uintptr_t>(blk[k.expect - 1]) ||
            (k.budget == 0 && st.chain_budget != 0)) {
          YuiMsg("jit_dispatch_test_failed budget=%u exit=%u mem=%u level=%u loop=%u ran=%u pc=%08x want=%08x",
                 k.budget, k.exitcount, k.memcycle, k.level, unsigned(k.loop), ran, st.SysReg[3], want.SysReg[3]);
          ok = false;
        }
      }
      for (unsigned b = 0; b < 3; ++b) {
        compiler->SetHigh((offset + b * 0x40) >> 1, nullptr);
        for (u32 i = 0; i < 16; ++i) compiler->LookupParentTable[adress_mask(pc + b * 0x40) + i].clear();
      }
      std::memcpy(HighWram + offset, saved, sizeof(saved));
      bool owned = false;
      for (u32 i = 0; i < 512; ++i)
        owned |= compiler->LookupParentTable[(adress_mask(pc) & ~511u) + i].size() != 0;
      if (!owned) compiler->code_pages[adress_mask(pc) >> 9] = 0;
      if (!ok) { YuiMsg("jit_dispatch_test_failed"); return -1; }
      YuiMsg("jit_dispatch_test_pass cases=%u", unsigned(sizeof(cases) / sizeof(cases[0])));
    }
#endif
    // Region lowering must retain the loop detector's progress accounting.
    // These loops change a register and must never be fast-forwarded as idle.
    for (unsigned variant = 0; variant < 2; ++variant) {
      const u16 decrement[] = {0x4010, 0x8bfd}; // DT r0; BF to start
      const u16 arithmetic[] = {0x70ff, 0x4011, 0x89fc}; // ADD -1; CMP/PZ; BT
      const u16 *program = variant ? arithmetic : decrement;
      const unsigned length = variant ? 3 : 2;
      for (unsigned i = 0; i < length; ++i)
        T2WriteWord(HighWram, offset + i * 2, program[i]);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block || (block->flags & BLOCK_LOOP)) {
        YuiMsg("jit_counted_loop_test_failed flags variant=%u", variant);
        return -1;
      }
      tagSH2 actual = {};
      actual.GenReg[0] = 2;
      actual.CtrlReg[0] = 0x3f2;
      actual.SysReg[3] = pc;
      tagSH2 expected = actual;
      expected.GenReg[0] = 1;
      expected.CtrlReg[0] |= variant ? 1 : 0;
      expected.SysReg[4] = variant ? 5 : 4;
      reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
      if (std::memcmp(&actual, &expected, sizeof(actual))) {
        YuiMsg("jit_counted_loop_test_failed state variant=%u", variant);
        return -1;
      }
    }
    YuiMsg("jit_counted_loop_test_pass cases=2");
    unsigned division_cases = 0;
    const u32 div_edges[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff};
    for (unsigned pair = 0; pair < 3; ++pair) {
      const unsigned m = pair ? 15 : 0, n = pair == 1 ? 15 : 1;
      T2WriteWord(HighWram, offset, 0x3004 | (n << 8) | (m << 4));
      T2WriteWord(HighWram, offset + 2, 0xa001);
      T2WriteWord(HighWram, offset + 4, 0x0009);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block) return -1;
      for (unsigned flags = 0; flags < 8; ++flags)
        for (u32 a : div_edges)
          for (u32 b : div_edges) {
            tagSH2 actual = {};
            actual.GenReg[m] = b; actual.GenReg[n] = a;
            actual.CtrlReg[0] = 0xf2 | (flags & 1) | ((flags & 6) << 7);
            actual.SysReg[3] = pc;
            tagSH2 expected = actual;
            const unsigned q = (flags >> 1) & 1, sign = a >> 31;
            const unsigned mode = (flags >> 2) & 1;
            const u32 shifted = (a << 1) | (flags & 1);
            const u32 divisor = m == n ? shifted : b;
            const bool subtract = q == mode;
            const u64 wide = subtract ? u64(shifted) - divisor : u64(shifted) + divisor;
            const unsigned carry = subtract ? shifted < divisor : (wide >> 32);
            const unsigned new_q = sign ^ mode ^ carry;
            expected.GenReg[n] = u32(wide);
            expected.CtrlReg[0] = (actual.CtrlReg[0] & ~0x101u)
              | (new_q << 8) | unsigned(new_q == mode);
            expected.SysReg[3] = pc + 8;
            expected.SysReg[4] = 4;
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
            if (std::memcmp(&actual, &expected, sizeof(actual))) {
              YuiMsg("jit_div1_test_failed case=%u", division_cases);
              return -1;
            }
            ++division_cases;
          }
    }
    YuiMsg("jit_div1_test_pass cases=%u", division_cases);
    unsigned sequence_cases = 0;
    for (unsigned low = 0; low < 3; ++low) {
      for (unsigned step = 0; step < 4; ++step) {
        T2WriteWord(HighWram, offset + step * 4, 0x4024 | (low << 8));
        T2WriteWord(HighWram, offset + step * 4 + 2, 0x3104);
      }
      T2WriteWord(HighWram, offset + 16, 0xa001);
      T2WriteWord(HighWram, offset + 18, 0x0009);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block) return -1;
      for (unsigned flags = 0; flags < 8; ++flags)
        for (u32 a : div_edges)
          for (u32 b : div_edges) {
            tagSH2 actual = {};
            actual.GenReg[0] = b; actual.GenReg[1] = a;
            actual.GenReg[2] = a ^ b;
            actual.CtrlReg[0] = 0xf2 | (flags & 1) | ((flags & 6) << 7);
            actual.SysReg[3] = pc;
            tagSH2 expected = actual;
            for (unsigned step = 0; step < 4; ++step) {
              const u32 old_low = expected.GenReg[low], sr = expected.CtrlReg[0];
              expected.GenReg[low] = (old_low << 1) | (sr & 1);
              expected.CtrlReg[0] = (sr & ~1u) | (old_low >> 31);
              const u32 before = expected.GenReg[1], control = expected.CtrlReg[0];
              const u32 shifted = (before << 1) | (control & 1);
              const u32 divisor = expected.GenReg[0];
              const unsigned q = (control >> 8) & 1, mode = (control >> 9) & 1;
              const u64 wide = q == mode ? u64(shifted) - divisor : u64(shifted) + divisor;
              const unsigned carry = q == mode ? shifted < divisor : (wide >> 32);
              const unsigned next_q = (before >> 31) ^ mode ^ carry;
              expected.GenReg[1] = u32(wide);
              expected.CtrlReg[0] = (control & ~0x101u) | (next_q << 8) | unsigned(next_q == mode);
            }
            expected.SysReg[3] = pc + 22;
            expected.SysReg[4] = 11;
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
            if (std::memcmp(&actual, &expected, sizeof(actual))) {
              YuiMsg("jit_div1_sequence_test_failed case=%u", sequence_cases);
              return -1;
            }
            ++sequence_cases;
          }
    }
    YuiMsg("jit_div1_sequence_test_pass cases=%u", sequence_cases);
    unsigned carry_cases = 0;
    for (unsigned pair = 0; pair < 3; ++pair)
      for (unsigned variant = 0; variant < 2; ++variant) {
        const unsigned m = pair ? 15 : 0, n = pair == 1 ? 15 : 1;
        for (unsigned step = 0; step < 8; ++step)
          T2WriteWord(HighWram, offset + step * 2,
            ((step ^ variant) & 1 ? 0x300a : 0x300e) | n << 8 | m << 4);
        T2WriteWord(HighWram, offset + 16, 0xa001);
        T2WriteWord(HighWram, offset + 18, 0x0009);
        Block *block = compiler->CompileBlock(pc, nullptr);
        if (!block) return -1;
        for (unsigned t = 0; t < 2; ++t)
          for (u32 a : div_edges)
            for (u32 b : div_edges) {
              tagSH2 actual = {};
              actual.GenReg[m] = b; actual.GenReg[n] = a;
              actual.CtrlReg[0] = 0x3f2 | t;
              actual.SysReg[3] = pc;
              tagSH2 expected = actual;
              for (unsigned step = 0; step < 8; ++step) {
                const u64 lhs = expected.GenReg[n];
                const u64 rhs = u64(expected.GenReg[m]) + (expected.CtrlReg[0] & 1);
                const bool sub = (step ^ variant) & 1;
                const u64 value = sub ? lhs - rhs : lhs + rhs;
                expected.GenReg[n] = u32(value);
                expected.CtrlReg[0] = (expected.CtrlReg[0] & ~1u) |
                  unsigned(sub ? lhs < rhs : value >> 32);
              }
              expected.SysReg[3] = pc + 22;
              expected.SysReg[4] = 11;
              reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
              if (std::memcmp(&actual, &expected, sizeof(actual))) {
                YuiMsg("jit_carry_sequence_test_failed case=%u", carry_cases);
                return -1;
              }
              ++carry_cases;
            }
      }
    YuiMsg("jit_carry_sequence_test_pass cases=%u", carry_cases);
    unsigned multiply_cases = 0;
    for (unsigned pair = 0; pair < 3; ++pair) {
      const unsigned m = pair ? 15 : 0, n = pair == 1 ? 15 : 1;
      const u16 ops[] = {u16(0x0007 | n << 8 | m << 4),
        0x021a, 0x0227, 0x031a, 0x040a, 0xa001, 0x0009};
      for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
        T2WriteWord(HighWram, offset + i * 2, ops[i]);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block) return -1;
      for (u32 a : div_edges) for (u32 b : div_edges) {
        tagSH2 actual = {};
        actual.GenReg[m] = b; actual.GenReg[n] = a;
        actual.CtrlReg[0] = 0x3f3;
        actual.SysReg[0] = 0x12345678;
        actual.SysReg[3] = pc;
        tagSH2 expected = actual;
        expected.GenReg[2] = u32(u64(actual.GenReg[m]) * actual.GenReg[n]);
        expected.SysReg[1] = u32(u64(expected.GenReg[2]) * expected.GenReg[2]);
        expected.GenReg[3] = expected.SysReg[1];
        expected.GenReg[4] = expected.SysReg[0];
        expected.SysReg[3] = pc + 16;
        expected.SysReg[4] = 14;
        reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
        if (std::memcmp(&actual, &expected, sizeof(actual))) {
          YuiMsg("jit_multiply_sequence_test_failed case=%u", multiply_cases);
          return -1;
        }
        ++multiply_cases;
      }
    }
    YuiMsg("jit_multiply_sequence_test_pass cases=%u", multiply_cases);
#ifdef VITA_SH2_POLL_SKIP
    // Validate real compiler admission and native state for both observed shapes.
    // Synthetic reads avoid touching guest devices during startup tests.
    unsigned poll_cases = 0;
    for (unsigned variant = 0; variant < 2; ++variant) {
      const u16 word_poll[] = {0x6041, 0x2008, 0x8bfc};
      const u16 byte_poll[] = {0x6260, 0x622c, 0x2279, 0x3270, 0x8bfa};
      const auto *program = variant ? byte_poll : word_poll;
      const unsigned length = variant ? 5 : 3;
      for (unsigned i = 0; i < length; ++i)
        T2WriteWord(HighWram, offset + i * 2, program[i]);
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block || block->poll != (variant ? 0x106u : 0x114u)) {
        YuiMsg("jit_poll_test_failed admission variant=%u poll=%x flags=%x start=%x end=%x", variant,
          block ? block->poll : 0, block ? block->flags : 0,
          block ? block->b_addr : 0, block ? block->e_addr : 0);
        return -1;
      }
      for (u32 value : {0u, 1u, 0x80u, 0xffffu}) {
        T2WriteWord(LowWram, offset + 24, value);
        if (variant) T2WriteByte(LowWram, offset + 24, value);
        tagSH2 actual = {};
        actual.getmembyte = reinterpret_cast<uintptr_t>(TestByte);
        actual.getmemword = reinterpret_cast<uintptr_t>(TestWord);
        actual.getmemlong = reinterpret_cast<uintptr_t>(TestLong);
        actual.GenReg[variant ? 6 : 4] = 0x00200000 + offset + 24;
        actual.GenReg[7] = 0x80;
        actual.SysReg[3] = pc;
        actual.CtrlReg[0] = 0xf0;
        tagSH2 expected = actual;
        const u32 loaded = variant ? (value & 0x80) : u32(s32(s16(value)));
        const bool t = variant ? loaded == 0x80 : loaded == 0;
        expected.GenReg[variant ? 2 : 0] = loaded;
        expected.CtrlReg[0] |= t;
        expected.SysReg[3] = t ? pc + length * 2 : pc;
        expected.SysReg[4] = length + (t ? 0 : 2);
        reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
        if (std::memcmp(&actual, &expected, sizeof(actual))) {
          YuiMsg("jit_poll_test_failed state variant=%u value=%08x cycles=%u", variant, value, actual.SysReg[4]);
          return -1;
        }
        ++poll_cases;
      }
    }
    YuiMsg("jit_poll_test_pass cases=%u", poll_cases);
    {
      // Compare the real ExecuteCount path with the same native block executing
      // every iteration. Preserve global context/cycles before guest startup.
      struct ContextRestore {
        DynarecSh2 *context = DynarecSh2::CurrentContext;
        u32 cycles = CurrentSH2->cycles;
        ~ContextRestore() {
          DynarecSh2::CurrentContext = context;
          CurrentSH2->cycles = cycles;
          poll_memory_cycles = 0;
          CompileBlocks::getInstance()->SetHigh(0xff000 >> 1, nullptr);
        }
      } context_restore;
      struct TestCpu : DynarecSh2 {
        void ClearCarry() { pre_exe_count_ = 0; }
        u32 Carry() const { return pre_exe_count_; }
      } cpu;
      cpu.SetCurrentContext();
      unsigned slice_cases = 0;
      const unsigned budgets[] = {1, 2, 7, 63, 1, 255, 3, 1000, 2};
      for (unsigned variant = 0; variant < 2; ++variant) {
        const u16 word_poll[] = {0x6041, 0x2008, 0x8bfc};
        const u16 byte_poll[] = {0x6260, 0x622c, 0x2279, 0x3270, 0x8bfa};
        const auto *program = variant ? byte_poll : word_poll;
        const unsigned length = variant ? 5 : 3;
        for (unsigned i = 0; i < length; ++i)
          T2WriteWord(HighWram, offset + i * 2, program[i]);
        Block *block = compiler->CompileBlock(pc, nullptr);
        if (!block || !block->poll) return -1;
        // CompileBlock creates code; Execute normally publishes its lookup.
        // Explicitly select this block so both sides execute the same code.
        compiler->SetHigh((pc & 0xfffff) >> 1, block);
        const u32 recipe = block->poll;
        for (unsigned cost : {0u, 2u, 4u}) {
          tagSH2 snapshots[9] = {};
          u32 carries[9] = {};
          poll_memory_cycles = cost;
          for (unsigned optimized = 0; optimized < 2; ++optimized) {
            block->poll = optimized ? recipe : 0;
            cpu.ClearCarry();
            tagSH2 initial = {};
            initial.getmembyte = reinterpret_cast<uintptr_t>(PollByte);
            initial.getmemword = reinterpret_cast<uintptr_t>(PollWord);
            initial.getmemlong = reinterpret_cast<uintptr_t>(TestLong);
            initial.GenReg[variant ? 6 : 4] = 0x00200000 + offset + 24;
            initial.GenReg[7] = 0x80;
            initial.SysReg[3] = pc;
            initial.CtrlReg[0] = 0xf0;
            *cpu.getDynaSh() = initial;
            T2WriteWord(LowWram, offset + 24, 1);
            if (variant) T2WriteByte(LowWram, offset + 24, 0);
            for (unsigned b = 0; b < 9; ++b) {
              cpu.ExecuteCount(budgets[b]);
              if (!optimized) {
                snapshots[b] = *cpu.getDynaSh();
                carries[b] = cpu.Carry();
              } else if (std::memcmp(&snapshots[b], cpu.getDynaSh(), sizeof(tagSH2)) ||
                         carries[b] != cpu.Carry()) {
                YuiMsg("jit_poll_slice_test_failed variant=%u cost=%u budget_index=%u", variant, cost, b);
                return -1;
              } else ++slice_cases;
            }
            // A stale exitcount must not accelerate standalone ExecuteBlock.
            cpu.SET_COUNT(0);
            cpu.memcycle_ = 0;
            cpu.getDynaSh()->exitcount = 10000;
            cpu.Execute();
            if (cpu.GET_COUNT() != length + 2) {
              YuiMsg("jit_poll_slice_test_failed standalone variant=%u", variant);
              return -1;
            }
            // Observe a changed predicate on the very next native entry.
            if (variant) T2WriteByte(LowWram, offset + 24, 0x80);
            else T2WriteWord(LowWram, offset + 24, 0);
            cpu.SET_COUNT(0);
            cpu.memcycle_ = 0;
            cpu.Execute();
            if (cpu.GET_PC() != pc + length * 2 || cpu.GET_COUNT() != length) {
              YuiMsg("jit_poll_slice_test_failed changed_value variant=%u", variant);
              return -1;
            }
          }
          block->poll = recipe;
        }
      }
      YuiMsg("jit_poll_slice_test_pass cases=%u", slice_cases);
    }
#endif
#ifdef VITA_SH2_RESIDENT_LOOPS
    unsigned resident_cases = 0;
    for (unsigned variant = 0; variant < 2; ++variant) {
      const u16 decrement[] = {0x4010, 0x8bfd};
      const u16 arithmetic[] = {0x70ff, 0x4011, 0x89fc};
      const u16 *program = variant ? arithmetic : decrement;
      const unsigned length = variant ? 3 : 2;
      for (unsigned i = 0; i < length; ++i)
        T2WriteWord(HighWram, offset + i * 2, program[i]);
      Block *block = compiler->CompileBlock(pc, compiler->LookupParentTable);
      if (!block || !(block->flags & BLOCK_RESIDENT_LOOP) || (block->flags & BLOCK_LOOP)) {
        YuiMsg("jit_resident_test_failed admission variant=%u", variant);
        return -1;
      }
      for (u32 value : {0u, 1u, 2u, 17u, 0x80000000u, 0xffffffffu})
        for (u32 budget : {0u, 1u, 4u, 7u, 64u})
          for (u32 irq : {0u, 0xf0u}) for (u32 mask : {0u, 0xf0u}) {
            tagSH2 actual = {};
            for (unsigned r = 0; r < 16; ++r) actual.GenReg[r] = 0xa5000000u + r;
            actual.GenReg[0] = value;
            actual.CtrlReg[0] = 0x302 | mask;
            actual.SysReg[3] = pc;
            actual.SysReg[5] = irq;
            actual.exitcount = budget;
            tagSH2 expected = actual;
            do {
              --expected.GenReg[0];
              const bool t = variant ? int32_t(expected.GenReg[0]) >= 0 : expected.GenReg[0] == 0;
              expected.CtrlReg[0] = (expected.CtrlReg[0] & ~1u) | u32(t);
              const bool taken = variant ? t : !t;
              expected.SysReg[4] += length - 1 + (taken ? 3 : 1);
              if (!taken) { expected.SysReg[3] += length * 2; break; }
              if (expected.SysReg[4] >= budget || mask < irq) break;
            } while (true);
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
            if (std::memcmp(&actual, &expected, sizeof(actual))) {
              YuiMsg("jit_resident_test_failed state variant=%u value=%08x budget=%u irq=%u mask=%u",
                variant, value, budget, irq, mask);
              return -1;
            }
            ++resident_cases;
          }
    }
    for (unsigned low = 0; low < 2; ++low) {
      u8 *ram = low ? LowWram : HighWram;
      const u32 source_pc = (low ? 0x00200000 : 0x06000000) + offset;
      Block **table = low ? compiler->LookupTableLow : compiler->LookupTable;
      T2WriteWord(ram, offset, 0x4010);
      T2WriteWord(ram, offset + 2, 0x8bfd);
      Block *block = compiler->CompileBlock(source_pc, low ? nullptr : compiler->LookupParentTable);
      if (!block || !(block->flags & BLOCK_RESIDENT_LOOP)) return -1;
      table[offset >> 1] = block;
      T2WriteWord(ram, offset + 2, 0x8b00); // redirect the former back edge
      SH2WriteNotify(source_pc + 2, 2);
      if (table[offset >> 1]) {
        YuiMsg("jit_resident_test_failed invalidation low=%u", low);
        return -1;
      }
      block = compiler->CompileBlock(source_pc, low ? nullptr : compiler->LookupParentTable);
      if (!block || (block->flags & BLOCK_RESIDENT_LOOP)) {
        YuiMsg("jit_resident_test_failed changed_source low=%u", low);
        return -1;
      }
    }
    YuiMsg("jit_resident_test_pass cases=%u invalidation=2", resident_cases);
#endif
    // Cross the 64-instruction region limit, spill all sixteen guest registers,
    // then execute an existing branch/delay-slot template. Checks the integrated
    // compiler, not merely the standalone arithmetic emitter.
    {
      tagSH2 actual = {};
      actual.CtrlReg[0] = 0x3f3;
      actual.SysReg[3] = pc;
      tagSH2 expected = actual;
      unsigned words = 0;
      for (unsigned n = 0; n < 16; ++n) {
        T2WriteWord(HighWram, offset + 2 * words++, 0xe000 | (n << 8) | (n + 1));
        expected.GenReg[n] = n + 1;
      }
      for (unsigned k = 0; k < 96; ++k) {
        const unsigned n = k & 15, m = (k + 1) & 15;
        T2WriteWord(HighWram, offset + 2 * words++, 0x300c | (n << 8) | (m << 4));
        expected.GenReg[n] += expected.GenReg[m];
      }
      T2WriteWord(HighWram, offset + 2 * words++, 0xa000);
      T2WriteWord(HighWram, offset + 2 * words++, 0x0009);
      expected.SysReg[3] = pc + 2 * words;
      expected.SysReg[4] = words + 1; // BRA takes two cycles, others one.
      Block *block = compiler->CompileBlock(pc, nullptr);
      if (!block) { YuiMsg("jit_region_test_failed compile"); return -1; }
      reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
      if (std::memcmp(&actual, &expected, sizeof(actual))) {
        YuiMsg("jit_region_test_failed pc=%08x cycles=%u", actual.SysReg[3], actual.SysReg[4]);
        return -1;
      }
      YuiMsg("jit_region_test_pass instructions=%u", words);
    }
    // Exercise actual compiler lowering and VM publication, including fallback
    // callback preservation and the delay-slot path.
    unsigned load_cases = 0;
    T2WriteLong(LowWram, offset + 16, 0x89abcdef);
    T2WriteLong(HighWram, offset + 256, 0x89abcdef);
    for (unsigned kind = 0; kind < 3; ++kind)
      for (unsigned postincrement = 0; postincrement < 2; ++postincrement)
      for (u32 address : {0x00200000 + offset + 16, pc + 256,
                          0x20200000 + offset + 16, 0x05f80000u})
        for (unsigned delayed = 0; delayed < 2; ++delayed)
          for (unsigned dest = 0; dest < 2; ++dest) {
            const u16 load = 0x6010 | (postincrement << 2) | (dest << 8) | kind;
            const u16 program[] = {u16(delayed ? 0xa000 : load),
                                   u16(delayed ? load : 0xa000), 0x0009};
            for (unsigned k = 0; k < 3; ++k)
              T2WriteWord(HighWram, offset + k * 2, program[k]);
            tagSH2 actual = {};
            actual.GenReg[1] = address;
            actual.SysReg[3] = pc;
            actual.getmembyte = reinterpret_cast<uintptr_t>(&TestByte);
            actual.getmemword = reinterpret_cast<uintptr_t>(&TestWord);
            actual.getmemlong = reinterpret_cast<uintptr_t>(&TestLong);
            tagSH2 expected = actual;
            expected.GenReg[dest] = kind == 0 ? 0xffffff89 :
                                   kind == 1 ? 0xffff89ab : 0x89abcdef;
            // Synthetic MMIO returns low byte/word of its sentinel instead.
            if (address == 0x05f80000u)
              expected.GenReg[dest] = kind == 0 ? 0xffffffef :
                                     kind == 1 ? 0xffffcdef : 0x89abcdef;
            if (postincrement && dest != 1) expected.GenReg[1] += 1u << kind;
            expected.SysReg[3] = pc + (delayed ? 4 : 6);
            expected.SysReg[4] = delayed ? 3 : 4;
            Block *block = compiler->CompileBlock(pc, nullptr);
            if (!block) return -1;
            load_calls = 0;
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
            unsigned expected_calls = 1;
#ifdef VITA_SH2_RAM_LOADS
#ifdef VITA_SH2_FUSED_DELAY
            const bool direct = true;          // a BRA delay slot is a region too
#else
            const bool direct = !delayed;
#endif
            if (direct && (address >> 20 == 2 || address >> 20 == 0x60))
              expected_calls = 0;
#endif
            if (std::memcmp(&actual, &expected, sizeof(actual)) || load_calls != expected_calls) {
              YuiMsg("jit_load_test_failed case=%u calls=%u expected_calls=%u pc=%08x cycles=%u",
                     load_cases, load_calls, expected_calls, actual.SysReg[3], actual.SysReg[4]);
              return -1;
            }
            ++load_cases;
          }
    YuiMsg("jit_load_test_pass cases=%u", load_cases);
    unsigned immediate_logic_cases=0;
    for(unsigned kind : {0xc9u,0xcau,0xcbu})
      for(unsigned imm : {0u,0x55u,0x80u,0xffu})
        for(u32 value : {0u,0xffffffffu,0x80000080u,0x12345678u})
          for(unsigned constant=0;constant<2;++constant) {
            unsigned words=0;
            if(constant) T2WriteWord(HighWram,offset+2*words++,0xe080);
            T2WriteWord(HighWram,offset+2*words++,(kind<<8)|imm);
            T2WriteWord(HighWram,offset+2*words++,0xa000);
            T2WriteWord(HighWram,offset+2*words++,0x0009);
            Block *block=compiler->CompileBlock(pc,nullptr);
            if(!block) return -1;
            tagSH2 actual={}; actual.GenReg[0]=value;
            actual.CtrlReg[0]=0x3f3; actual.SysReg[3]=pc;
            tagSH2 expected=actual;
            u32 operand=constant ? 0xffffff80u : value;
            expected.GenReg[0]=kind==0xc9 ? operand&imm :
                              kind==0xca ? operand^imm : operand|imm;
            expected.SysReg[3]=pc+2*words; expected.SysReg[4]=words+1;
            reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
            if(std::memcmp(&actual,&expected,sizeof(actual))) {
              YuiMsg("jit_immediate_logic_test_failed case=%u",immediate_logic_cases);
              return -1;
            }
            ++immediate_logic_cases;
          }
    YuiMsg("jit_immediate_logic_test_pass cases=%u",immediate_logic_cases);
    unsigned pc_load_cases=0;
    for(unsigned low=0;low<2;++low) for(unsigned alignment : {0u,2u})
      for(unsigned width : {2u,4u}) for(unsigned alias=0;alias<2;++alias) {
        u8 *ram=low ? LowWram : HighWram;
        const u32 source=(low ? 0x00200000u : 0x06000000u)+offset+alignment+(alias ? 0x20000000u : 0);
        const u32 literal=(source&~0xfffffu)+offset+16;
        const u32 base=(source+4)&(width==4 ? ~3u : ~0u);
        const u16 load=(width==4 ? 0xd300 : 0x9300)|((literal-base)/width);
        T2WriteWord(ram,offset+alignment,load);
        T2WriteWord(ram,offset+alignment+2,0xa000);
        T2WriteWord(ram,offset+alignment+4,0x0009);
        Block *block=compiler->CompileBlock(source,nullptr);
        if(!block) return -1;
        for(u32 value : {0x89abcdefu,0x12345678u}) {
          T2WriteLong(ram,offset+16,value); // same compiled body, new literal
          tagSH2 actual={}; actual.SysReg[3]=source;
          actual.getmemword=reinterpret_cast<uintptr_t>(&TestWord);
          actual.getmemlong=reinterpret_cast<uintptr_t>(&TestLong);
          tagSH2 expected=actual;
          expected.GenReg[3]=width==4 ? value : u32(s32(s16(value>>16)));
          expected.SysReg[3]=source+6; expected.SysReg[4]=4;
          load_calls=0;
          reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&actual);
          unsigned calls=1;
#ifdef VITA_SH2_PC_LOADS
          if(!alias) calls=0;
#endif
          if(std::memcmp(&actual,&expected,sizeof(actual)) || load_calls!=calls) {
            YuiMsg("jit_pc_load_test_failed case=%u calls=%u expected=%u",pc_load_cases,load_calls,calls);
            return -1;
          }
          ++pc_load_cases;
        }
      }
    YuiMsg("jit_pc_load_test_pass cases=%u",pc_load_cases);
#ifdef VITA_SH2_PC_LOADS
    {
      struct AliasRestore {
        DynarecSh2 *context=DynarecSh2::CurrentContext;
        u32 cycles=CurrentSH2->cycles;
        ~AliasRestore() { DynarecSh2::CurrentContext=context;
          CurrentSH2->cycles=cycles;
          auto *c=CompileBlocks::getInstance();
          c->SetHigh(offset>>1, nullptr); c->LookupTableLow[offset>>1]=nullptr;
        }
      } alias_restore;
      struct AliasCpu : DynarecSh2 { void ClearCarry() { pre_exe_count_=0; } } cpu;
      cpu.SetSlave(false);
      cpu.SetCurrentContext();
      unsigned cases=0;
      for(unsigned low=0;low<2;++low) {
        u8 *ram=low ? LowWram : HighWram;
        const u32 source=(low ? 0x00200000u : 0x06000000u)+offset;
        Block **table=low ? compiler->LookupTableLow : compiler->LookupTable;
        table[offset>>1]=nullptr;
        T2WriteWord(ram,offset,0xd303);
        T2WriteWord(ram,offset+2,0xa000); T2WriteWord(ram,offset+4,0x0009);
        T2WriteLong(ram,offset+16,0x89abcdef);
        // Each dispatch route must independently encounter both directions of
        // alias mismatch, rather than Execute always repairing ExecuteCount's
        // entry first. Repeated aliases also exercise cache hits.
        for(unsigned counted=0;counted<2;++counted)
        for(unsigned alias : {0u,1u,1u,0u,0u,1u}) {
          tagSH2 initial={}; initial.SysReg[3]=source+(alias ? 0x20000000u : 0);
          initial.CtrlReg[0]=0xf0;
          initial.getmemlong=reinterpret_cast<uintptr_t>(&memGetLong);
          *cpu.getDynaSh()=initial; cpu.memcycle_=0; cpu.ClearCarry();
          if(counted) cpu.ExecuteCount(4); else cpu.Execute();
          const unsigned extra=alias ? (low ? 4u : 2u) : 0u;
          if(cpu.GetGenRegPtr()[3]!=0x89abcdef || cpu.GET_PC()!=initial.SysReg[3]+6 ||
             cpu.GET_COUNT()!=(counted ? 4+extra : 4) ||
             (!counted && cpu.memcycle_!=extra) ||
             !table[offset>>1] || table[offset>>1]->b_addr!=initial.SysReg[3]) {
            YuiMsg("jit_pc_alias_test_failed case=%u pc=%08x cycles=%u memory=%u",
                   cases,cpu.GET_PC(),cpu.GET_COUNT(),cpu.memcycle_); return -1;
          }
          ++cases;
        }
      }
      YuiMsg("jit_pc_alias_test_pass cases=%u",cases);
    }
#endif
    unsigned poll_step_cases = 0;
    for (unsigned width = 0; width < 3; ++width)
      for (unsigned masked = 0; masked < 2; ++masked) {
        if (masked && width != 0) continue;
        const u16 plain[] = {u16(0x6010 | width), 0x2008, 0x8bfc};
        const u16 mask_ops[] = {0x6010, 0x600c, 0x2039, 0x3030, 0x8bfa};
        const u16 *ops = masked ? mask_ops : plain;
        const unsigned words = masked ? 5 : 3;
        for (unsigned k = 0; k < words; ++k) T2WriteWord(HighWram, offset + k * 2, ops[k]);
        Block *block = compiler->CompileBlock(pc, nullptr);
        const u32 descriptor = sh2a9::PollStep::Decode(ops, words);
        if (!block || !descriptor) return -1;
#ifdef VITA_SH2_POLL_STEP
        if (block->poll_step != descriptor) return -1;
#endif
#ifdef VITA_SH2_POLL_FUSION
        if (!(block->flags & BLOCK_POLL_FUSED)) return -1;
#endif
        for (u32 address : {0x00200000 + offset + 16, pc + 256, 0x05f80000u})
          for (u32 value : {0u, 1u, 0x80u, 0x89abcdefu, 0xffffffffu})
            for (u32 mask : {0u, 0x80u, 0xffu, 0xffffffffu}) {
              T2WriteLong(LowWram, offset + 16, value);
              T2WriteLong(HighWram, offset + 256, value);
              tagSH2 native = {};
              native.GenReg[1] = address; native.GenReg[3] = mask;
              native.CtrlReg[0] = 0x3f3;
              native.SysReg[3] = pc; native.SysReg[4] = 0xfffffffeu;
              native.getmembyte = reinterpret_cast<uintptr_t>(&TestByte);
              native.getmemword = reinterpret_cast<uintptr_t>(&TestWord);
              native.getmemlong = reinterpret_cast<uintptr_t>(&TestLong);
              tagSH2 semantic = native;
              reinterpret_cast<void (*)(tagSH2 *)>(block->code)(&native);
              sh2a9::PollStep::Run(descriptor, semantic.GenReg, semantic.CtrlReg[0],
                semantic.SysReg[3], semantic.SysReg[4], [](u32 a, unsigned bytes) {
                  return bytes == 1 ? TestByte(a) : bytes == 2 ? TestWord(a) : TestLong(a);
                });
              if (std::memcmp(&native, &semantic, sizeof(native))) {
                YuiMsg("jit_poll_step_test_failed case=%u", poll_step_cases); return -1;
              }
              ++poll_step_cases;
            }
      }
    YuiMsg("jit_poll_step_test_pass cases=%u", poll_step_cases);
#if defined(VITA_SH2_POLL_STEP) || defined(VITA_SH2_POLL_FUSION)
    {
      const u16 source[] = {0x6011, 0x2008, 0x8bfc, 0x0009};
      for (unsigned k = 0; k < 4; ++k) T2WriteWord(HighWram, offset + k * 2, source[k]);
      Block *block = compiler->CompileBlock(pc, compiler->LookupParentTable);
      if (!block || (!block->poll_step && !(block->flags & BLOCK_POLL_FUSED))) return -1;
      compiler->SetHigh(offset >> 1, block);
      T2WriteWord(HighWram, offset + 4, 0xa000); // Break the polling recipe.
      SH2WriteNotify(pc + 4, 2);
      if (compiler->LookupTable[offset >> 1]) return -1;
      block = compiler->CompileBlock(pc, compiler->LookupParentTable);
      if (!block || block->poll_step || (block->flags & BLOCK_POLL_FUSED)) return -1;
      YuiMsg("jit_poll_step_source_test_pass");
    }
#endif
    constexpr u32 low_pc = 0x00200000 + offset;
    const u16 low_program[] = {0xe005, 0x7001, 0xa000, 0x0009};
    for (unsigned i = 0; i < 4; ++i) T2WriteWord(LowWram, offset + i * 2, low_program[i]);
    Block *low_block = compiler->CompileBlock(low_pc, nullptr);
    if (!low_block) return -1;
    compiler->LookupTableLow[offset >> 1] = low_block;
    // A nearby non-overlapping write must retain this block.
    SH2WriteNotify(low_pc + 16, 2);
    if (compiler->LookupTableLow[offset >> 1] != low_block) {
      YuiMsg("jit_invalidation_test_failed nonoverlap"); return -1;
    }
    // Change an instruction inside the block, not at its entry point.
    T2WriteWord(LowWram, offset + 2, 0x7002);
    SH2WriteNotify(low_pc + 2, 2);
    if (compiler->LookupTableLow[offset >> 1]) {
      YuiMsg("jit_invalidation_test_failed stale_block"); return -1;
    }
    low_block = compiler->CompileBlock(low_pc, nullptr);
    if (!low_block) return -1;
    tagSH2 low_state = {};
    low_state.SysReg[3] = low_pc;
    reinterpret_cast<void (*)(tagSH2 *)>(low_block->code)(&low_state);
    if (low_state.GenReg[0] != 7 || low_state.SysReg[3] != low_pc + 8 || low_state.SysReg[4] != 5) {
      YuiMsg("jit_invalidation_test_failed recompile"); return -1;
    }
    YuiMsg("jit_invalidation_test_pass low_ram");
    for (unsigned i = 0; i < 4; ++i) T2WriteWord(HighWram, offset + 2 + i * 2, low_program[i]);
    Block *high_block = compiler->CompileBlock(pc + 2, compiler->LookupParentTable);
    if (!high_block) return -1;
    compiler->SetHigh((offset + 2) >> 1, high_block);
    SH2WriteNotify(pc + 1, 0);
    if (compiler->LookupTable[(offset + 2) >> 1] != high_block) {
      YuiMsg("jit_invalidation_test_failed empty_range"); return -1;
    }
    // Byte range begins outside this block but overlaps its first instruction.
    SH2WriteNotify(pc + 1, 2);
    if (compiler->LookupTable[(offset + 2) >> 1]) {
      YuiMsg("jit_invalidation_test_failed odd_range"); return -1;
    }
    YuiMsg("jit_invalidation_test_pass high_ram");
    {
      struct WriteContextRestore {
        DynarecSh2 *saved=DynarecSh2::CurrentContext;
        ~WriteContextRestore() { DynarecSh2::CurrentContext=saved; }
      } write_restore;
      DynarecSh2 cpu; cpu.SetCurrentContext(); cpu.SetSlave(false);
      unsigned cases=0;
      for(unsigned width : {1u,2u,4u}) for(unsigned alias=0;alias<2;++alias)
        for(unsigned overlap=0;overlap<2;++overlap) {
          const u16 program[]={0xe001,0x7001,0xa000,0x0009};
          for(unsigned i=0;i<4;++i) T2WriteWord(HighWram,offset+2*i,program[i]);
          Block *b=compiler->CompileBlock(pc,compiler->LookupParentTable);
          if(!b) return -1;
          compiler->SetHigh(offset>>1, b);
          const u32 target=pc+(overlap ? (width==1 ? 3 : 2) : 480);
          const u32 address=target|(alias ? 0x20000000u : 0);
          const u32 value=width==1 ? 2 : width==2 ? 0x7002 : 0x7002a000;
          cpu.memcycle_=17;
          if(width==1) memSetByte(address,value);
          else if(width==2) memSetWord(address,value);
          else memSetLong(address,value);
          const u32 actual=width==1 ? T2ReadByte(HighWram,target&0xfffff) :
              width==2 ? T2ReadWord(HighWram,target&0xfffff) : T2ReadLong(HighWram,target&0xfffff);
          if(actual!=value || cpu.memcycle_!=17+(alias ? 2 : 0) ||
             compiler->LookupTable[offset>>1]!=(overlap ? nullptr : b)) {
            YuiMsg("jit_write_invalidation_failed case=%u",cases); return -1;
          }
          if(overlap) {
            b=compiler->CompileBlock(pc,compiler->LookupParentTable);
            if(!b) return -1;
            tagSH2 state={}; state.SysReg[3]=pc;
            reinterpret_cast<void (*)(tagSH2 *)>(b->code)(&state);
            if(state.GenReg[0]!=3 || state.SysReg[3]!=pc+8 || state.SysReg[4]!=5)
              return -1;
          }
          ++cases;
        }
      // A longword may own code only in its SECOND halfword. The leaf must
      // reject that case too, not merely check the first source word.
      for(unsigned alias=0;alias<2;++alias) {
        compiler->Init();
        const u16 program[]={0xe001,0x7001,0xa000,0x0009};
        for(unsigned i=0;i<4;++i) T2WriteWord(HighWram,offset+2+2*i,program[i]);
        Block *b=compiler->CompileBlock(pc+2,compiler->LookupParentTable);
        if(!b || compiler->LookupParentTable[offset>>1].size()!=0) return -1;
        compiler->SetHigh((offset+2)>>1, b);
        cpu.memcycle_=17;
        memSetLong(pc|(alias ? 0x20000000u : 0),0x0009e003);
        if(compiler->LookupTable[(offset+2)>>1] ||
           cpu.memcycle_!=17+(alias ? 2 : 0) ||
           T2ReadLong(HighWram,offset)!=0x0009e003) return -1;
        b=compiler->CompileBlock(pc+2,compiler->LookupParentTable);
        if(!b) return -1;
        tagSH2 state={}; state.SysReg[3]=pc+2;
        reinterpret_cast<void (*)(tagSH2 *)>(b->code)(&state);
        if(state.GenReg[0]!=4 || state.SysReg[3]!=pc+10 || state.SysReg[4]!=5)
          return -1;
        ++cases;
      }
      YuiMsg("jit_write_invalidation_pass cases=%u",cases);
    }
    // These test blocks must not be reused for restored guest bytes.
    compiler->Init();
    YuiMsg("jit_compiler_test_pass cases=10");
    return 0;
  } catch (const std::bad_alloc &) {
    YuiMsg("jit_compiler_test_failed allocation");
    return -1;
  }
}
