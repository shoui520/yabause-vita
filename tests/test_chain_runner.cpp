/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/sh2_dynarec/chain_runner.h"
#include "../src/core/sh2_dynarec/cached_dispatch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/mman.h>
extern "C" const uint32_t prologue[];
struct Block { uint8_t *code; uint32_t begin,end,id,flags; };
static_assert(offsetof(Block,flags)==16 && offsetof(Block,code)==0);
static uint32_t *state, *memory_cycles;
static std::vector<Block *> *active_high;
static Block *active_block;
static Block *replacement_block;
static unsigned calls, mode;

static void Callback() {
  uintptr_t stack;
  asm volatile("mov %0,sp" : "=r"(stack));
  assert(!(stack&7));
  ++calls;
  state[22]+=2;
  *memory_cycles += 4 + (calls%3);
  if (mode==1 && calls==2) state[24]=0x10;
  if (mode==2 && calls==2) (*active_high)[(state[22]&0xfffff)>>1]=nullptr;
  if (mode==3 && calls==2) state[22]=0x05f80000;
  if (mode==4 && calls==2) active_block->flags=1;
  // A callback can replace an already compiled target, not only invalidate it.
  // The following dispatch must consume the authoritative table again.
  if (mode==5 && calls==2) (*active_high)[(state[22]&0xfffff)>>1]=replacement_block;
}

static void Reference(Sh2ChainContext &c) {
  c.completed=0; c.guest_cycles_low=0; c.guest_cycles_high=0; c.last_block=nullptr;
  uint64_t cycles=0;
  for (;;) {
    if (c.state[23]>=c.target_cycles) { c.reason=SH2_CHAIN_BUDGET; break; }
    if (c.completed>=c.max_blocks) { c.reason=SH2_CHAIN_QUOTA; break; }
    uint32_t pc=c.state[22], region=pc&0x0ff00000;
    if ((pc>>24)==0xc0 || (region!=0 && region!=0x00200000 && region!=0x06000000) ||
        (region==0 && c.rom_helpers)) { c.reason=SH2_CHAIN_MAPPING; break; }
    auto *block=static_cast<Block *>(sh2a9::CachedBlock(pc,c.rom_helpers,c.rom,c.low,c.high));
    if (!block) { c.reason=SH2_CHAIN_MISS; break; }
    if (block->flags&1) { c.reason=SH2_CHAIN_LOOP; break; }
    uint32_t before=c.state[23];
    reinterpret_cast<void (*)(uint32_t *)>(block->code)(c.state);
    ++c.completed; c.last_block=block;
    cycles+=uint32_t(c.state[23]-before);
    if ((c.state[16]&0xf0)<c.state[24]) { c.reason=SH2_CHAIN_INTERRUPT; break; }
    if (block->flags&1) { c.reason=SH2_CHAIN_POST_BLOCK; break; }
    c.state[23]+=*c.memory_cycles;
    *c.memory_cycles=0;
  }
  c.guest_cycles_low=cycles; c.guest_cycles_high=cycles>>32;
}

int main() {
  auto *code=static_cast<uint32_t *>(mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
      MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  assert(code!=MAP_FAILED);
  std::memcpy(code,prologue,28);
  // A callback changes architectural PC, clobbering r8 deliberately afterwards.
  // This models exits which store a destination without retaining it in r8.
  const uint32_t body[]={0xe597a064,0xe12fff3a,0xe3a04004,0xe3a05005,0xe3a06006,
                        0xe3a08008,0xe5970004,0xe0899000,0xe587905c,0xe12fff1b};
  std::memcpy(code+7,body,sizeof(body));
  __builtin___clear_cache(reinterpret_cast<char *>(code),reinterpret_cast<char *>(code+17));
  Block block={reinterpret_cast<uint8_t *>(code),0,0,0,0};
  auto *replacement_code=code+128;
  std::memcpy(replacement_code,code,17*sizeof(uint32_t));
  // Load a different cycle delta (state[2]) so stale target reuse is observable.
  replacement_code[13]=0xe5970008;
  __builtin___clear_cache(reinterpret_cast<char *>(replacement_code),
                         reinterpret_cast<char *>(replacement_code+17));
  Block replacement={reinterpret_cast<uint8_t *>(replacement_code),0,0,0,0};
  replacement_block=&replacement;
  constexpr unsigned count=0x100000/2;
  std::vector<Block *> rom(count,&block),low(count,&block),high(count,&block);
  unsigned cases=0;
  active_block=&block;
  for (mode=0;mode<6;++mode)
    for (uint32_t pc : {0x06001000u,0x00201000u,0x00001000u,0x26001000u,
                        0xc0001000u,0x06101000u})
      for (uint32_t start : {0u,10u,0xfffffffau})
        for (uint32_t target : {0u,1u,10u,100u,0xffffffffu})
          for (uint32_t quota : {0u,1u,2u,4u,17u})
            for (unsigned flags : {0u,1u})
              for (unsigned helpers : {0u,1u})
               for (uint32_t delta : {3u,0xfffffff0u}) {
                uint32_t a[40]={},b[40], mem_a=0,mem_b=0;
                a[1]=delta; a[2]=delta+7; a[22]=pc; a[23]=start;
                a[25]=reinterpret_cast<uintptr_t>(&Callback);
                std::memcpy(b,a,sizeof(a));
                block.flags=flags;
                Sh2ChainContext expected={a,reinterpret_cast<void *const *>(rom.data()),
                  reinterpret_cast<void *const *>(low.data()),reinterpret_cast<void *const *>(high.data()),
                  &mem_a,target,helpers,quota,0,0,0,nullptr,0};
                Sh2ChainContext actual=expected; actual.state=b; actual.memory_cycles=&mem_b;
                active_high=&high; state=a; memory_cycles=&mem_a; calls=0;
                Reference(expected);
                unsigned expected_calls=calls;
                // Restore the only slot the callback may have invalidated.
                high[((pc+4)&0xfffff)>>1]=&block;
                block.flags=flags;
                state=b; memory_cycles=&mem_b; calls=0;
                VitaSh2RunCached(&actual);
                assert(calls==expected_calls && mem_a==mem_b);
                assert(!std::memcmp(a,b,sizeof(a)));
                assert(actual.completed==expected.completed && actual.reason==expected.reason);
                assert(actual.last_block==expected.last_block);
                assert(actual.guest_cycles_low==expected.guest_cycles_low);
                assert(actual.guest_cycles_high==expected.guest_cycles_high);
                high[((pc+4)&0xfffff)>>1]=&block;
                ++cases;
              }
  munmap(code,4096);
  std::printf("SH-2 native runner: %u differential scheduling/callback cases passed\n",cases);
}
