/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional GPU capture with psp2-cex-gpues4-prof (VITA_GPUPROF builds only).
 * A run requests it with DATA "gpuprof.txt" ("<run id> <skip> <frames>"):
 * after <skip> presented frames, GXM and vitaGL calls are recorded, a sampler
 * thread reads the GPU diagnostics, and the render thread names each
 * render-proxy command as a scope and each presented frame. After <frames>
 * more frames (or when the buffer is nearly full) the capture is written to
 * DATA "gpuprof.csv". Before that only object lifetimes, scenes and
 * presents are recorded (the identity model needs the whole history). */
#include "gpuprof.h"
#ifdef VITA_GPUPROF
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2_gpuprof_capture_diagnostic.h>
#include <psp2_gpuprof_gxm_observer.h>
#include <psp2_gpuprof_labels.h>
#include <psp2_gpuprof_vitagl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern void YuiMsg(const char *format, ...);

enum { CAPACITY = 131072 };   /* 248-byte events: 32.5 MB */
static Psp2GpuProfCaptureEvent *events;
static Psp2GpuProfCapture capture;
static Psp2GpuProfLabels labels;
static Psp2GpuProfInfo info;
static SceUID mutex = -1, sampler = -1;
static atomic_int stop_sampler;
static int enabled, recording, finished;
static unsigned frames_skip, frames_left;

static void observe(void *ud, const Psp2GpuProfGxmCall *call) {
  (void)ud;
  if (!recording)
    switch (call->kind) {
      case PSP2_GPUPROF_GXM_DRAW: case PSP2_GPUPROF_GXM_DRAW_INSTANCED: case PSP2_GPUPROF_GXM_DRAW_PRECOMPUTED:
      case PSP2_GPUPROF_GXM_VERTEX_BIND: case PSP2_GPUPROF_GXM_FRAGMENT_BIND: return;
      default: break;
    }
  psp2GpuProfCaptureGxmCall(&capture, call);
}
static const char *csv_path;
/* Development builds only: keeps the text segment's end clear of the next
 * 64 KiB boundary, where vita-elf-create needs room for its SCE data. */
static const char gpuprof_text_pad[16 * 1024] = {1};

static void lock(void *p) { (void)p; sceKernelLockMutex(mutex, 1, NULL); }
static void unlock(void *p) { (void)p; sceKernelUnlockMutex(mutex, 1); }
static uint64_t now(void) { return sceKernelGetSystemTimeWide(); }

static int sample_thread(SceSize size, void *arg) {
  (void)size; (void)arg;
  static const unsigned groups[] = {2,4,43,55,71,72,75,76,77,78,103,104,107,108,109,110};
  uint64_t id = 0;
  while (!atomic_load(&stop_sampler)) {
    const unsigned group = groups[id % 16];
    Psp2GpuProfDiagnosticConfig cfg = {.size = sizeof(cfg), .abi = PSP2_GPUPROF_ABI,
      .group = group, .tag_group = group >= 103 ? 102 : 70};
    Psp2GpuProfDiagnostic data = {0};
    Psp2GpuProfDiagnosticObservation o = {.sampler_thread = (uint32_t)sceKernelGetThreadId(),
      .sample_id = id++, .group = group, .tag_group = cfg.tag_group,
      .process_id = (uint32_t)sceKernelGetProcessId(), .driver_fingerprint = info.driver_fingerprint};
    int clock = scePowerGetGpuClockFrequency();
    o.clock_before_mhz = clock > 0 ? (unsigned)clock : 0;
    o.before_us = now();
    o.result = psp2GpuProfReadDiagnostic(&cfg, &data);
    o.after_us = now();
    clock = scePowerGetGpuClockFrequency();
    o.clock_after_mhz = clock > 0 ? (unsigned)clock : 0;
    psp2GpuProfCaptureDiagnostic(&capture, &o, o.result ? NULL : &data);
    sceKernelDelayThread(500);
  }
  return 0;
}

void VitaGpuProfInit(const char *request_path, const char *output_path, const char *run_id) {
  FILE *f = fopen(request_path, "r");
  if (!f || ((const volatile char *)gpuprof_text_pad)[0] != 1) { if (f) fclose(f); return; }
  char run[80] = {0}; unsigned skip = 0, frames = 0;
  const int ok = fscanf(f, "%79s %u %u", run, &skip, &frames) == 3 && run_id[0] && !strcmp(run, run_id) && frames;
  fclose(f);
  if (!ok) return;
  events = malloc(sizeof(*events) * CAPACITY);
  mutex = sceKernelCreateMutex("gpuprof", 0, 0, NULL);
  if (!events || mutex < 0) { YuiMsg("gpuprof_init_failed alloc"); return; }
  Psp2GpuProfCaptureConfig cfg = {events, CAPACITY, lock, unlock, NULL};
  int rc = psp2GpuProfCaptureInit(&capture, &cfg);
  if (!rc) rc = psp2GpuProfCaptureStart(&capture);
  /* vitaGL has not initialized GXM yet: a fresh GXM lifetime. */
  if (!rc) rc = psp2GpuProfCaptureGxmIdentitySeed365(&capture, (uint32_t)sceKernelGetProcessId(), 0, 0);
  if (rc) { YuiMsg("gpuprof_init_failed rc=%d", rc); return; }
  psp2GpuProfGxmSetObserver(observe, NULL);
  enabled = 1; frames_skip = skip; frames_left = frames; csv_path = output_path;
  YuiMsg("gpuprof_capture skip=%u frames=%u capacity=%u", skip, frames, (unsigned)CAPACITY);
}

static void StartSampler(void) {
  info.size = sizeof(info); info.abi = PSP2_GPUPROF_ABI;
  const int rc = psp2GpuProfGetInfo(&info);
  if (rc || info.status || !(info.capabilities & PSP2_GPUPROF_CAP_DIAGNOSTIC)) {
    YuiMsg("gpuprof_sampler_unavailable rc=%d status=%d caps=%x", rc, (int)info.status, (unsigned)info.capabilities);
    return;
  }
  sampler = sceKernelCreateThread("gpuprof-sampler", sample_thread, 0x10000100, 0x10000, 0, 0, NULL);
  if (sampler >= 0 && sceKernelStartThread(sampler, 0, NULL) < 0) { sceKernelDeleteThread(sampler); sampler = -1; }
}

void VitaGpuProfThreadBegin(void) {
  if (enabled) psp2GpuProfLabelsBegin(&labels, &capture, (uint32_t)sceKernelGetThreadId());
}
void VitaGpuProfStartSampler(void) {}
int VitaGpuProfScope(const char *name) {
  return recording && !finished ? psp2GpuProfScopePush(&labels, now(), name) : -1;
}
void VitaGpuProfScopeEnd(int scope) {
  if (recording && !finished && !scope) {
    psp2GpuProfDrawName(&labels, now(), NULL);
    psp2GpuProfScopePop(&labels, now());
  }
}

/* Render thread, once per presented frame. */
void VitaGpuProfFrame(uint64_t frame) {
  if (!enabled || finished) return;
  if (!recording) {
    if (frame < frames_skip) return;
    YuiMsg("gpuprof_recording frame=%llu events=%u", (unsigned long long)frame, (unsigned)capture.count);
    recording = 1;
    psp2GpuProfVitaGLCapture(&capture);
    StartSampler();
    return;
  }
  psp2GpuProfFrameName(&labels, now(), frame, "frame");
  if (--frames_left && capture.count < CAPACITY - CAPACITY / 8) return;
  finished = 1;
  atomic_store(&stop_sampler, 1);
  if (sampler >= 0) { sceKernelWaitThreadEnd(sampler, NULL, NULL); sceKernelDeleteThread(sampler); }
  int rc = psp2GpuProfCaptureStop(&capture);
  FILE *f = fopen(csv_path, "w");
  if (!f) rc = PSP2_GPUPROF_CAPTURE_IO;
  else setvbuf(f, NULL, _IOFBF, 1 << 20);
  if (!rc) rc = psp2GpuProfCaptureWriteCsv(&capture, f);
  if (f && fclose(f)) rc = PSP2_GPUPROF_CAPTURE_IO;
  YuiMsg("gpuprof_written rc=%d events=%llu dropped=%llu", rc, (unsigned long long)capture.count,
         (unsigned long long)capture.dropped);
}
#endif
