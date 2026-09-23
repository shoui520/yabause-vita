/* SPDX-License-Identifier: GPL-2.0-or-later */
// Real production runtime + generated A32 + real C68K executor; only Vita VM
// publication/time/logging are replaced by Linux equivalents for QEMU.
#include "../src/vita/c68k_runtime.h"
#include "../src/vita/sh2_code_memory.h"
#include "../src/core/c68k/c68k.h"
#include "../src/core/c68k/native_guard.h"
#include "../src/core/c68k/native_source.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
static unsigned char *arena;
static unsigned publications;
static uint64_t native_executed;
static std::thread::id publisher;
unsigned char *VitaM68kCodeArena() { return arena; }
VitaCodeWrite::VitaCodeWrite(void *p, size_t n, vitacode::Region region) : address(p), length(n) {
  assert(std::this_thread::get_id() == publisher);
  assert(region == vitacode::Region::M68k && n && n <= 4096);
  assert(static_cast<unsigned char *>(p) >= arena &&
         static_cast<unsigned char *>(p) + n <= arena + vitacode::Layout::M68k);
}
VitaCodeWrite::~VitaCodeWrite() {
  __builtin___clear_cache(static_cast<char *>(address), static_cast<char *>(address) + length);
  ++publications;
}
extern "C" uint64_t sceKernelGetProcessTimeWide() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void YuiMsg(const char *format, ...) {
  char line[1024]; va_list args; va_start(args, format);
  vsnprintf(line, sizeof(line), format, args); va_end(args);
  if (const char *at = strstr(line, " executed=")) native_executed = strtoull(at + 10, nullptr, 10);
  puts(line);
}
int main() {
  publisher = std::this_thread::get_id();
  arena = static_cast<unsigned char *>(mmap(nullptr, vitacode::Layout::M68k,
    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(arena != MAP_FAILED && VitaM68kNativeInit() == 0);
  std::vector<uint8_t> ram(0x80000);
  ram[0] = 42; ram[1] = 0x70; // MOVEQ #42,D0
  ram[2] = 0x71; ram[3] = 0x4e; // NOP
  ram[4] = 0xfa; ram[5] = 0x60; // BRA.S back to zero
  C68k_Init(&C68K, nullptr);
  C68k_Set_Fetch(&C68K, 0, ram.size() - 1, (pointer)ram.data());
  C68K.DirectReadRam = ram.data();
  C68kNativeSourceBind(ram.data());
  std::atomic<unsigned> finish{0}, resume{0};
  std::thread worker([&] {
    VitaM68kNativeStart();
    for (unsigned frame = 1; frame <= 120; ++frame) {
      C68k_Set_PC(&C68K, 0);
      c68k_struc oracle = C68K;
      // Non-global CPU instances reject native admission: same interpreter
      // supplies the reference while the global CPU can use cached A32.
      const auto expected = C68k_Exec(&oracle, 180);
      const auto actual = C68k_Exec(&C68K, 180);
      assert(actual == expected);
      assert(C68k_Get_SR(&C68K) == C68k_Get_SR(&oracle));
      auto compared = C68K;
      // gen_moveq stores the whole sign-extended byte in flag_N; GET_SR and
      // condition decoding observe bit 7 only. Native Flush uses canonical N.
      compared.flag_N &= 0x80; oracle.flag_N &= 0x80;
      if (memcmp(&compared, &oracle, sizeof(C68K))) {
        for (size_t offset = 0; offset < sizeof(C68K); ++offset) {
          const auto a = reinterpret_cast<const unsigned char *>(&compared)[offset];
          const auto b = reinterpret_cast<const unsigned char *>(&oracle)[offset];
          if (a != b) fprintf(stderr, "frame=%u offset=%zu native=%02x oracle=%02x\n", frame, offset, a, b);
        }
        assert(false);
      }
      assert(C68K.D[0] == (frame <= 60 ? 42u : 7u));
      VitaM68kNativePark();
      finish.store(frame, std::memory_order_release);
      while (resume.load(std::memory_order_acquire) != frame) std::this_thread::yield();
      VitaM68kNativeResume();
    }
    VitaM68kNativeStop();
  });
  for (unsigned frame = 1; frame <= 120; ++frame) {
    while (finish.load(std::memory_order_acquire) != frame) std::this_thread::yield();
    if (frame == 60) {
      C68K_NATIVE_GUARD;
      ram[0] = 7;
      C68kNativeSourceChanged(0, 1);
    }
    VitaM68kNativePublish();
    resume.store(frame, std::memory_order_release);
  }
  worker.join();
  assert(publications >= 2 && native_executed > 0);
  C68kNativeSourceBind(nullptr);
  printf("68K production runtime: 120 frame handoffs, full CPU-state oracle, RAM invalidation passed; publications=%u native=%llu\n",
         publications, static_cast<unsigned long long>(native_executed));
  // Runtime artifacts retire during static teardown; OS releases arena last.
}
