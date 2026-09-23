/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
extern "C" {
extern const uint32_t prologue[], epilogue[];
void TestChainEntry(uint32_t *, const void *, const void *);
unsigned TestChainPreservation(const void *, uint32_t *, const void *);
}
static unsigned calls;
__attribute__((noinline)) static uint32_t Callback() {
  uintptr_t stack;
  asm volatile("mov %0,sp" : "=r"(stack));
  assert(!(stack & 7));
  ++calls;
  asm volatile("mov r1,#17\nmov r2,#19\nmov r3,#23\nmov r12,#29"
               ::: "r1","r2","r3","r12","cc");
  return 0x12345678;
}
int main() {
  auto *code = static_cast<uint32_t *>(mmap(nullptr,4096,
    PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  assert(code != MAP_FAILED);
  // Actual assembler templates, not a separately reconstructed prologue.
  assert(prologue[0] == 0xe92d5ff0u);
  assert(epilogue[2] == 0xe12fff1bu); // bx r11
  std::memcpy(code,prologue,28);
  const uint32_t body[] = {
    0xe597a064u, 0xe12fff3au, 0xe5870000u, // callback; store r0
    0xe2888002u, 0xe2899001u // advance architectural PC and count
  };
  std::memcpy(code+7,body,sizeof(body));
  std::memcpy(code+12,epilogue,12);
  __builtin___clear_cache(reinterpret_cast<char *>(code),reinterpret_cast<char *>(code+15));
  for (unsigned i=0;i<10000;++i) {
    uint32_t normal[40]={}, chained[40];
    normal[22]=0xfffffffcu + i;
    normal[23]=0xffffffffu - i;
    normal[25]=reinterpret_cast<uintptr_t>(&Callback);
    std::memcpy(chained,normal,sizeof(normal));
    calls=0;
    reinterpret_cast<void (*)(uint32_t *)>(code)(normal);
    reinterpret_cast<void (*)(uint32_t *)>(code)(normal);
    assert(calls==2);
    calls=0;
    TestChainEntry(chained,code+7,code+7);
    assert(calls==2 && normal[0]==0x12345678);
    assert(normal[22]==uint32_t(0xfffffffcu+i+4));
    assert(normal[23]==uint32_t(0xffffffffu-i+2));
    assert(!std::memcmp(normal,chained,sizeof(normal)));
    assert(!TestChainPreservation(code,normal,code+7));
    assert(!TestChainPreservation(reinterpret_cast<const void *>(&TestChainEntry),chained,code+7));
  }
  munmap(code,4096);
  std::puts("SH-2 chain ABI: 10000 normal/private callback chains passed");
}
