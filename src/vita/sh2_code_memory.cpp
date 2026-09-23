/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh2_code_memory.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <new>
#include <cstdint>
#include <cstring>
#ifdef VITA_M68K_NATIVE_GUARDS
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <pthread.h>
#include <semaphore.h>
#include <cerrno>
#endif
extern "C" void YuiMsg(const char *, ...);
static SceUID arena_uid = -1;
static unsigned char *arena;
static constexpr std::size_t arena_size = vitacode::Layout::Total;

unsigned char *VitaSh2CodeArena() {
  if (arena) return arena;
  arena_uid = sceKernelAllocMemBlockForVM("SaturnSH2", arena_size);
  if (arena_uid < 0) { YuiMsg("jit_alloc_error=%08x", arena_uid); throw std::bad_alloc(); }
  if (sceKernelGetMemBlockBase(arena_uid, reinterpret_cast<void **>(&arena)) < 0) {
    sceKernelFreeMemBlock(arena_uid); arena_uid = -1; throw std::bad_alloc();
  }
  YuiMsg("jit_arena_bytes=%u", static_cast<unsigned>(arena_size));
  YuiMsg("jit_partition sh2_bytes=%u sh2_blocks=%u m68k_bytes=%u",
         static_cast<unsigned>(vitacode::Layout::Sh2), vitacode::Layout::Sh2Blocks,
         static_cast<unsigned>(vitacode::Layout::M68k));
  return arena;
}

unsigned char *VitaM68kCodeArena() {
  if constexpr (vitacode::Layout::M68k == 0) return nullptr;
  return VitaSh2CodeArena() + vitacode::Layout::Sh2;
}

/* Single emulation thread owns compilation and execution. Match the working
 * Vita VM publication sequence: open for writes, close, synchronize, execute.
 * Keep mutable block metadata outside the executable domain. */
VitaCodeWrite::VitaCodeWrite(void *p, std::size_t n, vitacode::Region region) : address(p), length(n) {
  const auto begin = reinterpret_cast<std::uintptr_t>(arena);
  const auto target = reinterpret_cast<std::uintptr_t>(p);
  if (!arena || target < begin || !vitacode::Layout::Contains(region, target - begin, n)) {
    YuiMsg("jit_invalid_write"); sceKernelExitProcess(1);
  }
  int rc = sceKernelOpenVMDomain();
  if (rc < 0) { YuiMsg("jit_open_error=%08x", rc); sceKernelExitProcess(1); }
}
VitaCodeWrite::~VitaCodeWrite() {
  int rc = sceKernelCloseVMDomain();
  if (rc >= 0) rc = sceKernelSyncVMDomain(arena_uid, address, length);
  if (rc < 0) { YuiMsg("jit_publish_error=%08x", rc); sceKernelExitProcess(1); }
}

#ifdef VITA_M68K_NATIVE_GUARDS
namespace {
struct CrossCoreProbe {
  unsigned char *code;
  sem_t ready, done;
  unsigned actual = 0;
  int mask, affinity = -1;
  bool stop = false;
};
void ProbeWait(sem_t *semaphore) {
  int result;
  do { result = sem_wait(semaphore); } while (result && errno == EINTR);
  if (result) __builtin_trap();
}
void ProbePost(sem_t *semaphore) {
  if (sem_post(semaphore)) __builtin_trap();
}
void *CrossCoreEntry(void *argument) {
  auto &probe = *static_cast<CrossCoreProbe *>(argument);
  probe.affinity = sceKernelChangeThreadCpuAffinityMask(0, probe.mask);
  if (probe.affinity >= 0 && sceKernelGetThreadCpuAffinityMask(0) != probe.mask)
    probe.affinity = -1;
  ProbePost(&probe.done);
  if (probe.affinity < 0) return nullptr;
  for (;;) {
    ProbeWait(&probe.ready);
    if (probe.stop) return nullptr;
    probe.actual = reinterpret_cast<unsigned (*)()>(probe.code)();
    ProbePost(&probe.done);
  }
}
// Startup only, before either guest CPU worker exists. Main owns publication;
// the consumer acknowledges completion BEFORE the same address is rewritten.
// This proves only the tested handoff, not concurrent VM-domain mutation.
int CrossCoreSmoke(unsigned char *code) {
  const int original_mask = sceKernelGetThreadCpuAffinityMask(0);
  if (original_mask < 0) {
    YuiMsg("jit_cross_core_affinity_query_failed rc=%08x", original_mask); return -1;
  }
  struct RestoreAffinity {
    int mask;
    ~RestoreAffinity() {
      if (sceKernelChangeThreadCpuAffinityMask(0, mask) < 0) __builtin_trap();
    }
  } restore{original_mask};
  const int pin = sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
  const int observed = sceKernelGetThreadCpuAffinityMask(0);
  if (pin < 0 || observed != SCE_KERNEL_CPU_MASK_USER_0) {
    YuiMsg("jit_cross_core_affinity_failed original=%08x pin_rc=%08x observed=%08x",
           original_mask, pin, observed);
    return -1;
  }
  const int masks[] = {SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2};
  for (int mask : masks) {
    CrossCoreProbe probe{};
    probe.code = code; probe.mask = mask;
    if (sem_init(&probe.ready, 0, 0)) return -1;
    if (sem_init(&probe.done, 0, 0)) { sem_destroy(&probe.ready); return -1; }
    pthread_t worker;
    const int create = pthread_create(&worker, nullptr, CrossCoreEntry, &probe);
    if (create) { sem_destroy(&probe.done); sem_destroy(&probe.ready); return -1; }
    ProbeWait(&probe.done);
    bool passed = probe.affinity >= 0;
    if (passed) {
      for (unsigned i = 0; i < 64; ++i) {
        const unsigned expected = (37 * i + 11) & 255;
        const uint32_t instructions[] = {0xe3a00000u | expected, 0xe12fff1e};
        {
          VitaSh2CodeWrite write(code, sizeof(instructions));
          std::memcpy(code, instructions, sizeof(instructions));
        }
        ProbePost(&probe.ready);
        ProbeWait(&probe.done);
        if (probe.actual != expected) {
          YuiMsg("jit_cross_core_mismatch mask=%x iteration=%u expected=%u actual=%u",
                 mask, i, expected, probe.actual);
          passed = false; break;
        }
      }
      probe.stop = true; ProbePost(&probe.ready);
    }
    if (pthread_join(worker, nullptr)) __builtin_trap();
    sem_destroy(&probe.done); sem_destroy(&probe.ready);
    if (!passed) {
      YuiMsg("jit_cross_core_failed mask=%x affinity_rc=%08x", mask, probe.affinity);
      return -1;
    }
    YuiMsg("jit_cross_core_pass publisher_mask=%x consumer_mask=%x publications=64",
           SCE_KERNEL_CPU_MASK_USER_0, mask);
  }
  return 0;
}
} // namespace
#endif

/* Validate ARM/Thumb interworking and instruction-cache republication before
 * executing any translated guest code. A9 MPCore TRM 2.1: the SCU does not
 * maintain instruction-cache coherency. A data write alone is insufficient. */
extern "C" int VitaSh2CodeSmokeTest() {
  try {
    unsigned char *code = VitaSh2CodeArena();
    using Function = unsigned (*)();
    Function volatile entry = reinterpret_cast<Function>(code); // ARM: bit 0 clear
    const unsigned values[] = {42u, 7u, 99u};
    for (unsigned value : values) {
      const std::uint32_t instructions[] = {
        0xe3a00000u | value, // mov r0, #value
        0xe12fff1eu,         // bx lr (return to Thumb caller)
      };
      {
        VitaSh2CodeWrite write(code, sizeof(instructions));
        std::memcpy(code, instructions, sizeof(instructions));
      }
      const unsigned actual = entry();
      if (actual != value) {
        YuiMsg("jit_smoke_failed expected=%u actual=%u", value, actual);
        return -1;
      }
    }
    YuiMsg("jit_smoke_pass publications=3");
#ifdef VITA_M68K_NATIVE_GUARDS
    if (CrossCoreSmoke(code) != 0) return -1;
#endif
    return 0;
  } catch (const std::bad_alloc &) {
    return -1;
  }
}
