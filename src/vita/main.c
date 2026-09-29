/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <psp2/kernel/processmgr.h>
#include "diag_timers.h"
#include <malloc.h>
#ifdef VITA_DIAG_TIMERS
#include <string.h>
void YuiMsg(const char *, ...);
uint64_t vita_diag_us[DT_COUNT], vita_diag_calls[DT_COUNT];
uint64_t vita_diag_dsp_insns;
uint64_t vita_diag_exec_us[2], vita_diag_exec_n[2];
uint64_t vita_diag_sprite_us[8], vita_diag_sprite_texels[8], vita_diag_sprite_calls[8];
int scePowerGetArmClockFrequency(void);
#include <arm_neon.h>
int sceDmacMemcpy(void *dst, const void *src, unsigned size);
static void VitaDiagDramStores(void) {
  static unsigned runs;
  if (runs++ >= 3) return;
  { /* cost of one empty DIAG_T0/DIAG_T1 pair */
    uint64_t e0 = sceKernelGetProcessTimeWide(), acc = 0;
    for (int k = 0; k < 10000; ++k) { uint64_t t = sceKernelGetProcessTimeWide(); acc += sceKernelGetProcessTimeWide() - t; }
    uint64_t e1 = sceKernelGetProcessTimeWide();
    YuiMsg("diag_timer_overhead pair_ns=%llu (10000 pairs, acc=%llu)", (unsigned long long)((e1 - e0) * 1000 / 10000),
           (unsigned long long)acc);
  }
  { /* 1.5 MiB copy: CPU memcpy vs DMA engine */
    const unsigned bytes = 1536u * 1024u;
    static uint8_t *a, *b;
    if (!a) { a = (uint8_t *)memalign(64, bytes); b = (uint8_t *)memalign(64, bytes); if (a) memset(a, 3, bytes); }
    if (a && b) {
      uint64_t c0 = sceKernelGetProcessTimeWide();
      memcpy(b, a, bytes);
      uint64_t c1 = sceKernelGetProcessTimeWide();
      int rc = sceDmacMemcpy(b, a, bytes);
      uint64_t c2 = sceKernelGetProcessTimeWide();
      YuiMsg("diag_copy 1.5MiB memcpy=%llu dmac=%llu rc=%d us", (unsigned long long)(c1 - c0),
             (unsigned long long)(c2 - c1), rc);
    }
  }
  const unsigned n = 1u << 20;                         /* 1 Mi texels = 4 MiB */
  static uint32_t *big;
  if (!big) big = (uint32_t *)memalign(64, n * 4);
  if (!big) return;
  uint32x4_t v = vdupq_n_u32(0x12345678);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (unsigned k = 0; k < n; k += 4) vst1q_u32(big + k, v);
  uint64_t t1 = sceKernelGetProcessTimeWide();
  for (unsigned k = 0; k < n; k += 8) { uint32x4x2_t w = {{v, v}}; vst1q_u32_x2(big + k, w); }
  uint64_t t2 = sceKernelGetProcessTimeWide();
  memset(big, 1, n * 4);
  uint64_t t3 = sceKernelGetProcessTimeWide();
  for (unsigned k = 0; k < n; ++k) big[k] = k;
  uint64_t t4 = sceKernelGetProcessTimeWide();
  YuiMsg("diag_dram 4MiB neon16=%llu neon32=%llu memset=%llu scalar=%llu us", (unsigned long long)(t1 - t0),
         (unsigned long long)(t2 - t1), (unsigned long long)(t3 - t2), (unsigned long long)(t4 - t3));
}
void VitaDiagTimersReport(void) {
  VitaDiagDramStores();
  { /* measured core clock: 8 dependent 1-cycle ADDs per iteration */
    uint64_t t0 = sceKernelGetProcessTimeWide();
    unsigned x = 0;
    for (unsigned i = 0; i < 2000000; ++i)
      __asm__ volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
                       "add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1" : "+r"(x));
    uint64_t us = sceKernelGetProcessTimeWide() - t0;
    YuiMsg("diag_clock measured_mhz=%llu reported_arm_mhz=%d x=%u", us ? (unsigned long long)(16000000ull / us) : 0ull,
           scePowerGetArmClockFrequency(), x);
  }
  static const char *const names[DT_COUNT] = {"sprite_decode", "vdp2_bitmap", "atlas_push", "vdp1_draw",
    "sh2_master_exec", "sh2_slave_exec", "hblank", "scu_exec", "smpc_cd_exec", "vdp2_draw", "gpu",
    "scu_dma", "scu_dsp"};
  for (unsigned i = 0; i < DT_COUNT; ++i)
    if (vita_diag_calls[i])
      YuiMsg("diag_timer name=%s us=%llu calls=%llu", names[i], (unsigned long long)vita_diag_us[i],
             (unsigned long long)vita_diag_calls[i]);
  for (unsigned m = 0; m < 8; ++m)
    if (vita_diag_sprite_calls[m])
      YuiMsg("diag_sprite mode=%u us=%llu texels=%llu calls=%llu", m, (unsigned long long)vita_diag_sprite_us[m],
             (unsigned long long)vita_diag_sprite_texels[m], (unsigned long long)vita_diag_sprite_calls[m]);
  memset(vita_diag_sprite_us, 0, sizeof(vita_diag_sprite_us)); memset(vita_diag_sprite_texels, 0, sizeof(vita_diag_sprite_texels));
  memset(vita_diag_sprite_calls, 0, sizeof(vita_diag_sprite_calls));
  { extern uint64_t vita_diag_exec_us[2], vita_diag_exec_n[2];
    YuiMsg("diag_exec master_us=%llu master_n=%llu slave_us=%llu slave_n=%llu", (unsigned long long)vita_diag_exec_us[0],
           (unsigned long long)vita_diag_exec_n[0], (unsigned long long)vita_diag_exec_us[1], (unsigned long long)vita_diag_exec_n[1]);
    memset(vita_diag_exec_us, 0, sizeof(vita_diag_exec_us)); memset(vita_diag_exec_n, 0, sizeof(vita_diag_exec_n)); }
  { extern uint32_t vita_diag_slice[2][8];
    for (unsigned c = 0; c < 2; ++c)
      YuiMsg("diag_slices cpu=%u early=%u idle=%u xforward=%u recorded=%u inslice_forward=%u other=%u", c,
             vita_diag_slice[c][0], vita_diag_slice[c][1], vita_diag_slice[c][2], vita_diag_slice[c][3],
             vita_diag_slice[c][4], vita_diag_slice[c][5]);
    memset(vita_diag_slice, 0, sizeof(vita_diag_slice)); }
  { extern void VitaDiagDspReport(void); VitaDiagDspReport(); }
  YuiMsg("diag_dsp instructions=%llu", (unsigned long long)vita_diag_dsp_insns); vita_diag_dsp_insns = 0;
  memset(vita_diag_us, 0, sizeof(vita_diag_us)); memset(vita_diag_calls, 0, sizeof(vita_diag_calls));
}
#endif
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "telemetry.h"
#include "threads.h"
#include "telemetry_policy.h"
#include "sound_budget_api.h"
#include "audio_pcm.h"
#ifdef VITA_ASYNC_AUDIO_OUTPUT
#include "audio_output_queue.h"
#endif
#include "yui.h"
#include "vidsoft.h"
#include "m68kc68k.h"
#include "osdcore.h"
#include "peripheral.h"
#include "memory.h"
#include "present.h"
#include "frame_capture.h"
#include "input_replay.h"
#include "vdp2.h"
static unsigned capture_frame, presented_frames;
int YuiCapturePending(void) {
  return capture_frame && presented_frames + 1 == capture_frame;
}
#ifdef YABAUSE_VITAGL
#include <vitaGL.h>
#include "vidogl.h"
#include "ygl.h"
static SceUID graphics_owner = -1;
static int graphics_ready;
#endif
#ifdef YABAUSE_GXM_COMPOSITOR
#include "gxm/device.h"
#include "titan/titan.h"
extern int TitanGxmFrame(VitaGxmCompositeFrame *frame);
static VitaGxmDevice *gxm;
static unsigned gxm_width, gxm_height;
static int gxm_compositor_enabled = 1;
int YuiUseGxmCompositor(void) { return gxm_compositor_enabled; }
#endif

#define DATA "ux0:data/yabause-vita/"
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
static FILE *logfile;
static SceUID display_block = -1;
static uint32_t *screens;
static unsigned screen_index;
static int audio_port = -1;
static VitaAudioPcm audio_pcm;
static int audio_verify;
static unsigned audio_packets;
static uint64_t audio_hash;
static uint64_t present_us;
#ifdef VITA_ROTATION_ROUTE
#include "rotation_route.h"
volatile int vita_rotation_cpu;
volatile unsigned vita_rotation_eligible;
static volatile uint32_t route_wait_us; /* render thread adds, frontend takes */
static VitaRotationRoute rotation_route;
#endif
static uint64_t copy_us;
static VitaPresentMap present_map;
static PerPad_struct *pad;
static VitaInputReplay input_replay;
extern void VitaReportStages(void);
#include "c68k_runtime.h"
#ifdef YABAUSE_GXM_PROBE
extern int VitaGxmProbe(void);
#endif
#ifdef VITA_SH2_DYNAREC
extern int VitaSh2CodeSmokeTest(void);
extern int VitaSh2CompilerTest(void);
extern void VitaSh2ReportExecution(void);
#endif
extern int VitaC68kReadTest(void);

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
void YuiMsg(const char *fmt, ...) {
  if (!logfile) return;
  VT_SCOPE(VT_LOGGING);
  pthread_mutex_lock(&log_mutex);
  va_list ap; va_start(ap, fmt); vfprintf(logfile, fmt, ap); va_end(ap);
  fputc('\n', logfile); fflush(logfile);
  pthread_mutex_unlock(&log_mutex);
}
void YuiErrorMsg(const char *s) { YuiMsg("error=%s", s); }
int YuiUseOGLOnThisThread(void) {
#ifdef YABAUSE_VITAGL
  return graphics_ready && sceKernelGetThreadId() == graphics_owner ? 0 : -1;
#else
  return -1;
#endif
}
int YuiRevokeOGLOnThisThread(void) { return 0; }

/* Reference renderer presentation. Display manual: CDRAM, 256-byte base,
 * pitch multiple of 64; never write the buffer currently being scanned out. */
void YuiSwapBuffers(void) {
  VT_SCOPE(VT_PRESENT);
#ifdef YABAUSE_VITAGL
  if (YuiUseOGLOnThisThread() != 0) {
    YuiMsg("fatal: graphics submission from non-owner thread");
    abort();
  }
  uint64_t start = sceKernelGetProcessTimeWide();
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    YuiMsg("vitagl_frame_error=%04x", (unsigned)error);
    abort();
  }
  if (capture_frame && ++presented_frames == capture_frame) {
    VT_SCOPE(VT_CAPTURE);
    YuiMsg("capture_state tvmd=%04x renderer=vitagl", Vdp2Regs->TVMD);
    extern RBGDrawInfo g_rgb0, g_rgb1;
    extern vdp2rotationparameter_struct paraA, paraB;
    YuiMsg("capture_rotation mode=%u bgon=%04x rbg0_bitmap=%d format=%d rbg1_bitmap=%d format=%d coefA=%d deltaA=%g coefB=%d deltaB=%g",
      Vdp2Regs->RPMD, Vdp2Regs->BGON,
      g_rgb0.info.isbitmap, g_rgb0.info.colornumber,
      g_rgb1.info.isbitmap, g_rgb1.info.colornumber,
      paraA.coefenab, (double)paraA.deltaKAx, paraB.coefenab, (double)paraB.deltaKAx);
    uint64_t capture_start = sceKernelGetProcessTimeWide();
    uint8_t *pixels = malloc(960 * 544 * 4);
    int result = -1;
    if (pixels) {
      GLint read_fbo, draw_fbo;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glReadBuffer(GL_BACK);
      /* Checked vitaGL ends this scene and waits before reading its pixels. */
      glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      GLenum capture_error = glGetError();
      glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
      if (capture_error == GL_NO_ERROR) {
        FILE *image = fopen(DATA "frame.ppm", "wb");
        if (image) {
          result = VitaWriteRgbFrame(image, pixels, 960, 544);
          if (fclose(image) != 0) result = -1;
        }
      }
      free(pixels);
    }
    YuiMsg("capture_%s frame=%u elapsed_us=%llu timing_sample_invalid=1",
      result == 0 ? "complete" : "failed", presented_frames,
      sceKernelGetProcessTimeWide() - capture_start);
    capture_frame = 0;
    start = sceKernelGetProcessTimeWide();
  }
  vglSwapBuffers(GL_FALSE);
#ifdef VITA_DIAG_GPU_STAGES
  { extern void YglVitaGpuStage(int); YglVitaGpuStage(5); }
#endif
#ifdef VITA_ROTATION_ROUTE
  __atomic_add_fetch(&route_wait_us, (uint32_t)(sceKernelGetProcessTimeWide() - start), __ATOMIC_RELAXED);
#endif
  present_us += sceKernelGetProcessTimeWide() - start;
#else
  extern pixel_t *dispbuffer;
  int w, h, interlace;
  VIDSoft.GetNativeResolution(&w, &h, &interlace);
  if (!screens || !dispbuffer || VitaPresentMapInit(&present_map, w, h) != 0) return;
  uint64_t start = sceKernelGetProcessTimeWide();
  uint32_t *out = screens + screen_index * 512 * 272;
#ifdef YABAUSE_GXM_COMPOSITOR
  if (gxm_compositor_enabled) {
    VitaGxmCompositeFrame frame;
    int rc = TitanGxmFrame(&frame);
    if (rc >= 0 && (!gxm || gxm_width != frame.width || gxm_height != frame.height)) {
      rc = VitaGxmDeviceDestroy(&gxm);
      if (rc >= 0) rc = VitaGxmDeviceCreate(frame.width, frame.height, &gxm);
      if (rc >= 0) rc = VitaGxmBindDisplay(gxm, screens,
        (2 * 512 * 272 * 4 + 0x3ffff) & ~0x3ffff);
      if (rc >= 0) {
        gxm_width = frame.width; gxm_height = frame.height;
        YuiMsg("renderer=gxm_composition cpu_rasterization=1 width=%u height=%u",
          gxm_width, gxm_height);
      }
    }
    if (rc >= 0) rc = VitaGxmComposite(gxm, &frame, out);
    if (rc >= 0) goto present;
    // Fail visibly to the complete reference path. Never present a partial
    // native frame or report that a software fallback is GPU rendering.
    YuiMsg("gxm_composition_failed=%08x renderer=software", rc);
    gxm_compositor_enabled = 0;
    TitanRender(dispbuffer);
  }
#endif
  VitaPresentCopy(&present_map, out, dispbuffer);
  copy_us += sceKernelGetProcessTimeWide() - start;
#ifdef YABAUSE_GXM_COMPOSITOR
present:
#endif
  if (capture_frame && ++presented_frames == capture_frame) {
    uint64_t capture_start = sceKernelGetProcessTimeWide();
    YuiMsg("capture_state tvmd=%04x renderer=reference", Vdp2Regs->TVMD);
    int result = -1;
    FILE *image = fopen(DATA "frame.ppm", "wb");
    if (image) {
      result = VitaWriteRgbRows(image, (const uint8_t *)out, 480, 272, 512, 0);
      if (fclose(image) != 0) result = -1;
    }
    YuiMsg("capture_%s frame=%u elapsed_us=%llu timing_sample_invalid=1",
      result == 0 ? "complete" : "failed", presented_frames,
      sceKernelGetProcessTimeWide() - capture_start);
    capture_frame = 0;
    start = sceKernelGetProcessTimeWide();
  }
  ;
  SceDisplayFrameBuf fb = {sizeof(fb), out, 512,
    SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, 480, 272};
  int rc = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
  if (rc < 0) YuiMsg("display_error=%08x", rc);
  { VT_SCOPE(VT_VBLANK_WAIT); sceDisplayWaitVblankStart(); }
  screen_index ^= 1;
  present_us += sceKernelGetProcessTimeWide() - start;
#endif
}

#ifdef VITA_ASYNC_AUDIO_OUTPUT
static VitaAudioOutputQueue audio_queue;
static int audio_device_output(void *ctx,const int16_t *samples) {
  (void)ctx;
  int rc;
  { VT_SCOPE(VT_AUDIO_OUTPUT); rc=sceAudioOutOutput(audio_port,samples); }
  VitaTelemetryReport();
  return rc;
}
static void audio_queue_event(void *ctx,int event) {
  (void)ctx;
  if(event==VAQ_START) {
    /* Kernel Overview: equal-priority threads do not time-share. SCSP can
     * spin waiting for CPU0 on this same core; playback must be able to
     * resume after device/IO waits, including while owning a shared lock.
     * This worker blocks on PCM/device waits and does no guest emulation. */
    int previous=sceKernelGetThreadCurrentPriority();
    /* SDK user-priority range is64..191; never request a system priority. */
    int priority_rc=previous>64 && previous<=191 ?
      sceKernelChangeThreadPriority(0,previous-1) : -1;
    int observed_priority=sceKernelGetThreadCurrentPriority();
    if(priority_rc<0 || observed_priority!=previous-1) {
      YuiMsg("audio_output_priority_failed previous=%d observed=%d result=%08x",
        previous,observed_priority,priority_rc);
      abort();
    }
    int rc=sceKernelChangeThreadCpuAffinityMask(0,0x20000);
    YuiMsg("audio_output_priority previous=%d observed=%d",previous,observed_priority);
    YuiMsg("audio_output_affinity cpu=1 result=%08x observed=%08x",rc,sceKernelGetThreadCpuAffinityMask(0));
    VitaTelemetryThread("audio_output");
  } else if(event==VAQ_STOP) VitaTelemetryReport();
  else if(event==VAQ_WAIT_BEGIN) VitaTelemetryEnter(VT_QUEUE_WAIT);
  else if(event==VAQ_WAIT_END) VitaTelemetryLeave(VT_QUEUE_WAIT);
}
#endif
static int audio_init(void) {
  memset(&audio_pcm,0,sizeof(audio_pcm));
  audio_verify=capture_frame!=0;
  audio_packets=0;
  audio_hash=UINT64_C(14695981039346656037);
  audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 512, 44100,
    SCE_AUDIO_OUT_MODE_STEREO);
  if(audio_port<0) return -1;
#ifdef VITA_ASYNC_AUDIO_OUTPUT
  if(VitaAudioQueueOpen(&audio_queue,NULL,audio_device_output,audio_queue_event)<0) {
    sceAudioOutReleasePort(audio_port); audio_port=-1; return -1;
  }
#endif
  return 0;
}
static void audio_deinit(void) {
  if (audio_port >= 0) {
    /* Sony AudioOutput Overview: NULL waits for the last buffer to finish. */
#ifdef VITA_ASYNC_AUDIO_OUTPUT
    int rc=VitaAudioQueueClose(&audio_queue);
    YuiMsg("audio_queue_closed submitted=%llu completed=%llu result=%08x",audio_queue.submitted,audio_queue.completed,rc);
#else
    sceAudioOutOutput(audio_port,NULL);
#endif
    sceAudioOutReleasePort(audio_port);
  }
  audio_port = -1;
}
static int audio_reset(void) {
  audio_pcm.used = 0;
#ifdef VITA_ASYNC_AUDIO_OUTPUT
  if(audio_port>=0) return VitaAudioQueueDrain(&audio_queue);
#endif
  return 0;
}
static int audio_format(int hz) { (void)hz; return 0; }
static void audio_submit(const int16_t *samples) {
  if(audio_verify) {
    /* Capture-only prefix check, canonical little-endian PCM bytes. */
    for(unsigned i=0;i<VITA_AUDIO_FRAMES*2;++i) {
      unsigned value=(uint16_t)samples[i];
      audio_hash=(audio_hash^(value&255))*UINT64_C(1099511628211);
      audio_hash=(audio_hash^(value>>8))*UINT64_C(1099511628211);
    }
    if(++audio_packets%128==0)
      YuiMsg("audio_pcm_prefix packets=%u hash=%016llx timing_sample_invalid=1",audio_packets,(unsigned long long)audio_hash);
  }
#if defined(VITA_DIAG_AUDIO_UNPACED)
  /* Diagnostic throughput builds only: generated samples are hashed above but
   * not played, so blocking playback (the real-time pacer) is bypassed. */
  (void)samples;
#elif defined(VITA_ASYNC_AUDIO_OUTPUT)
  int rc=VitaAudioQueuePush(&audio_queue,samples);
  if(rc<0) { YuiMsg("audio_queue_failed=%08x",rc); abort(); }
#else
  VT_SCOPE(VT_AUDIO_OUTPUT);
  sceAudioOutOutput(audio_port,samples);
#endif
}
static void audio_update(u32 *left, u32 *right, u32 n) {
  VT_SCOPE(VT_AUDIO_CONVERT);
  VitaAudioPcmPush(&audio_pcm,(s32 *)left,(s32 *)right,n,
                   ScspConvert32uto16s,audio_submit);
}
static u32 audio_space(void) { return 2048; }
static void audio_mute(void) { int v[2] = {0, 0}; sceAudioOutSetVolume(audio_port, 3, v); }
static void audio_unmute(void) { int v[2] = {32768, 32768}; sceAudioOutSetVolume(audio_port, 3, v); }
static void audio_volume(int v) { int a[2] = {v * 32768 / 100, v * 32768 / 100}; sceAudioOutSetVolume(audio_port, 3, a); }
static SoundInterface_struct sound_vita = {1, "Vita audio", audio_init,
  audio_deinit, audio_reset, audio_format, audio_update, audio_space,
  audio_mute, audio_unmute, audio_volume};
M68K_struct *M68KCoreList[] = {&M68KDummy, &M68KC68K, NULL};
SH2Interface_struct *SH2CoreList[] = {&SH2Interpreter, &SH2DebugInterpreter,
#ifdef VITA_SH2_DYNAREC
  &SH2Dyn,
#endif
  NULL};
PerInterface_struct *PERCoreList[] = {&PERDummy, NULL};
CDInterface *CDCoreList[] = {&DummyCD, &ISOCD, NULL};
SoundInterface_struct *SNDCoreList[] = {&SNDDummy, &sound_vita, NULL};
VideoInterface_struct *VIDCoreList[] = {
#ifdef YABAUSE_VITAGL
  &VIDOGL,
#endif
  &VIDSoft, NULL};

static void input(unsigned frame) {
  static const unsigned buttons[] = {SCE_CTRL_UP, SCE_CTRL_RIGHT, SCE_CTRL_DOWN,
    SCE_CTRL_LEFT, 0, SCE_CTRL_RTRIGGER, SCE_CTRL_CIRCLE,
    SCE_CTRL_CROSS, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER, 0,
    SCE_CTRL_START, SCE_CTRL_SQUARE};
  /* Saturn order: Up Right Down Left R C B A Y Z L Start X.
   * Physical shoulders are Z/C; Saturn L/R remain unbound. */
  SceCtrlData ctrl = {0}; sceCtrlPeekBufferPositive(0, &ctrl, 1);
  unsigned replay_mask = 0, previous = input_replay.next;
  int scripted = VitaInputReplayMask(&input_replay, frame, &replay_mask);
  if (input_replay.next != previous)
    YuiMsg("input_replay frame=%u mask=%03x", frame, replay_mask);
  for (unsigned i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i) {
    if (scripted ? (replay_mask & (1u << i)) : (ctrl.buttons & buttons[i]))
      PerKeyDown(i);
    else PerKeyUp(i);
  }
}

/* Lists the saves in internal backup RAM: 64-byte blocks of the odd bytes;
 * a save's first block starts 0x80000000, then its 11-byte name. */
static void VitaLogBackupRam(const char *path) {
  unsigned saves = 0;
  if (!BupRam) return;
  for (u32 block = 2; block < 512; ++block) {
    const u32 base = block * 128;
    if (T1ReadByte(BupRam, base + 1) != 0x80 || T1ReadByte(BupRam, base + 3) ||
        T1ReadByte(BupRam, base + 5) || T1ReadByte(BupRam, base + 7)) continue;
    char name[12];
    for (int i = 0; i < 11; ++i) {
      const u8 c = T1ReadByte(BupRam, base + 9 + 2 * i);
      name[i] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    name[11] = 0;
    YuiMsg("backup_ram_save block=%u name=%s", (unsigned)block, name);
    ++saves;
  }
  YuiMsg("backup_ram path=%s saves=%u", path, saves);
}

int main(void) {
  SceAppUtilInitParam ap = {0}; SceAppUtilBootParam bp = {0};
  sceAppUtilInit(&ap, &bp);
  sceIoMkdir(DATA, 0777);
  logfile = fopen(DATA "run.log", "w");
  YuiMsg("start title=YABA00001 build=%s %s", __DATE__, __TIME__);
  YabThreadSetCurrentThreadAffinityMask(1);
  char run_id[80] = {0};
  FILE *receipt = fopen(DATA "run-id.txt", "r");
  if (receipt) { fgets(run_id, sizeof(run_id), receipt); fclose(receipt); }
  run_id[strcspn(run_id, "\r\n")] = 0;
  YuiMsg("run_id=%s", run_id);
  if (!VitaTelemetryCheckThreadIsolation()) {
    YuiMsg("thread_isolation_failed: refusing shared thread-local state");
    fclose(logfile);
    return 1;
  }
  YuiMsg("thread_isolation_pass");
  int telemetry_enabled = VT_OVERVIEW;
  FILE *telemetry_request = fopen(DATA "telemetry-mode.txt", "r");
  if (telemetry_request) {
    char requested_run[80] = {0}, mode[16] = {0};
    if (fscanf(telemetry_request, "%79s %15s", requested_run, mode) == 2 &&
        run_id[0] && strcmp(requested_run, run_id) == 0) {
      if (strcmp(mode, "off") == 0) telemetry_enabled = 0;
      if (strcmp(mode, "detailed") == 0) telemetry_enabled = VT_DETAILED;
      if (strcmp(mode, "sampled") == 0) telemetry_enabled = VT_SAMPLED;
    }
    fclose(telemetry_request);
  }
  VitaTelemetryConfigure(telemetry_enabled);
  int blocking_sound = 0;
  FILE *sound_request = fopen(DATA "sound-wait.txt", "r");
  if (sound_request) {
    char requested_run[80] = {0}, mode[16] = {0};
    if (fscanf(sound_request, "%79s %15s", requested_run, mode) == 2 &&
        run_id[0] && !strcmp(run_id, requested_run) && !strcmp(mode, "blocking"))
      blocking_sound = 1;
    fclose(sound_request);
  }
  VitaSoundBudgetConfigure(blocking_sound);
  YuiMsg("sound_wait=%s", blocking_sound ? "blocking" : "spin");
  unsigned benchmark_frames = 0, benchmark_start_frame = 0;
  FILE *benchmark_request = fopen(DATA "benchmark-frames.txt", "r");
  if (benchmark_request) {
    char requested_run[80] = {0}; unsigned requested_frames = 0, requested_start = 0;
    if (fscanf(benchmark_request, "%79s %u %u", requested_run, &requested_frames, &requested_start) >= 2 &&
        run_id[0] && !strcmp(run_id, requested_run) && requested_frames <= 1000000 &&
        requested_start < requested_frames) {
      benchmark_frames = requested_frames;
      benchmark_start_frame = requested_start;
    }
    fclose(benchmark_request);
  }
  YuiMsg("telemetry_mode=%s coverage=partial", telemetry_enabled == VT_DETAILED ? "detailed" : telemetry_enabled == VT_SAMPLED ? "sampled" : telemetry_enabled ? "overview" : "off");
  FILE *input_request = fopen(DATA "input-replay.txt", "r");
  if (input_request) {
    char requested_run[80] = {0};
    if (fgets(requested_run, sizeof(requested_run), input_request)) {
      requested_run[strcspn(requested_run, "\r\n")] = 0;
      if (run_id[0] && strcmp(requested_run, run_id) == 0) {
        if (VitaInputReplayRead(input_request, &input_replay) != 0) {
          fclose(input_request); remove(DATA "input-replay.txt");
          YuiMsg("invalid_input_replay"); goto done;
        }
        YuiMsg("input_replay_loaded events=%u", input_replay.count);
      }
    }
    fclose(input_request); remove(DATA "input-replay.txt");
  }
  FILE *capture_request = fopen(DATA "capture-frame.txt", "r");
  if (capture_request) {
    char requested_run[80] = {0}; unsigned requested_frame = 0;
    if (fscanf(capture_request, "%79s %u", requested_run, &requested_frame) == 2 &&
        strcmp(requested_run, run_id) == 0 && requested_frame <= 1000000)
      capture_frame = requested_frame;
    fclose(capture_request);
    remove(DATA "capture-frame.txt");
  }
  /* Preserve an existing overclock (nominal 500 / approximately 496 MHz).
   * The public API's documented ceiling is 444; no plugin is installed here. */
#ifdef VITA_POWER_MODE_C
  /* Sony Power Overview ch.6: Mode C = "high" GPU core clock with WLAN kept
   * (brightness limited, camera unavailable). Value from the SDK power.h:
   * SCE_POWER_CONFIGURATION_MODE_C 0x00010880U. Logged before/after because
   * the header describes the ARM clock in this mode as "normal". */
  {
    int arm = scePowerGetArmClockFrequency(), gpu = scePowerGetGpuClockFrequency();
    int rc = scePowerSetConfigurationMode(0x00010880);
    YuiMsg("power_mode_c rc=%d arm_mhz=%d->%d gpu_mhz=%d->%d", rc, arm,
           scePowerGetArmClockFrequency(), gpu, scePowerGetGpuClockFrequency());
  }
#endif
  if (scePowerGetArmClockFrequency() < 444) scePowerSetArmClockFrequency(444);
  YuiMsg("cpu_mhz=%d gpu_mhz=%d", scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
#ifdef VITA_SH2_DYNAREC
  if (VitaSh2CodeSmokeTest() != 0) goto done;
  if (VitaM68kNativeInit() != 0) goto done;
#endif
#ifdef YABAUSE_VITAGL
  graphics_owner = sceKernelGetThreadId();
  vglUseTripleBuffering(GL_FALSE);
  /* Keep transient vertex storage bounded without disabling retirement checks.
   * 8 MiB leaves 24 MiB of the RAM pool for indices, rings and shader state. */
  vglSetCircularPoolSize(8*1024*1024);
  /* Pinned vitaGL returns resolution-fallback, NOT success/failure. */
  GLboolean resolution_fallback = vglInitWithCustomSizes(0, 960, 544,
    32*1024*1024, 48*1024*1024, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
  graphics_ready = 1;
#ifdef VITA_DIAG_NO_VSYNC
  /* Diagnostic only: vitaGL defaults to vsync_interval=1 (vgl.c), waiting a
   * vblank in the display callback (gxm.c). Frame contents are unchanged. */
  vglWaitVblankStart(GL_FALSE);
  YuiMsg("diag_vsync=off");
#endif
  GLint viewport[4] = {0};
  glGetIntegerv(GL_VIEWPORT, viewport);
  GLenum init_error = glGetError();
  YuiMsg("vitagl_initialized viewport=%dx%d resolution_fallback=%d gl_error=%04x",
    viewport[2], viewport[3], resolution_fallback, (unsigned)init_error);
  if (viewport[2] != 960 || viewport[3] != 544 || init_error != GL_NO_ERROR)
    goto done;
#else
  display_block = sceKernelAllocMemBlock("SaturnDisplay", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
    (2 * 512 * 272 * 4 + 0x3ffff) & ~0x3ffff, NULL);
  if (display_block < 0 || sceKernelGetMemBlockBase(display_block, (void **)&screens) < 0) {
    YuiMsg("display_allocation_failed=%08x", display_block); goto done;
  }
  memset(screens, 0, 2 * 512 * 272 * 4);
#endif
  char disc[1024] = {0};
  FILE *config = fopen(DATA "boot-path.txt", "r");
  if (config) { fgets(disc, sizeof(disc), config); fclose(config); disc[strcspn(disc, "\r\n")] = 0; }
  yabauseinit_struct init = {0};
  init.sh2coretype = 0; init.vidcoretype = VIDCORE_SOFT; init.sndcoretype = 1;
#ifdef YABAUSE_VITAGL
  init.vidcoretype = VIDCORE_OGL;
#endif
#ifdef VITA_SH2_DYNAREC
  init.sh2coretype = 3;
#endif
  init.percoretype = 0; init.m68kcoretype = M68KCORE_C68K;
  init.cdcoretype = disc[0] ? CDCORE_ISO : CDCORE_DUMMY;
  init.biospath = DATA "bios.bin"; init.cdpath = disc[0] ? disc : NULL;
  init.buppath = DATA "backup.bin"; init.carttype = 0;
  init.regionid = 1; init.videoformattype = VIDEOFORMATTYPE_NTSC;
  init.clocksync = 1; init.basetime = 946684800; init.numthreads = 1;
  init.scsp_sync_count_per_frame = 1;
  YuiMsg("init sh2=%s renderer=%s audio=enabled disc=%s",
    init.sh2coretype == 3 ? "arm-dynarec" : "interpreter",
    init.vidcoretype == VIDCORE_SOFT ? "software" : "vitagl", disc[0] ? "yes" : "no");
#ifdef YABAUSE_GXM_PROBE
  int gxm_result = VitaGxmProbe();
  if (gxm_result < 0) { YuiMsg("gxm_resource_probe_failed=%08x", gxm_result); goto done; }
  YuiMsg("gxm_resource_probe_pass");
#endif
  if (YabauseInit(&init) != 0) { YuiMsg("init_failed"); goto done; }
#ifdef VITA_ROTATION_ROUTE
  VitaRotationRouteInit(&rotation_route);
#endif
  VitaLogBackupRam(init.buppath);
#ifdef YABAUSE_VITAGL
  if (VIDCore->ColorRamWriteWord != YglOnUpdateColorRamWord) {
    YuiMsg("fatal: accelerated renderer palette notification is not connected");
    YabauseDeInit();
    goto done;
  }
  VIDOGL.SetSettingValue(VDP_SETTING_RESOLUTION_MODE, RES_ORIGINAL);
  VIDOGL.Resize(0, 0, 960, 544, 1, ORIGINAL);
  if (capture_frame) {
    extern int YglVitaValidatePalette(void);
    extern int YglVitaValidateRotation(void);
    if (YglVitaValidatePalette() != 0) { YabauseDeInit(); goto done; }
    if (YglVitaValidateRotation() != 0) { YabauseDeInit(); goto done; }
  }
#endif
#ifdef VITA_SH2_DYNAREC
  if (VitaSh2CompilerTest() != 0) { YabauseDeInit(); goto done; }
#ifdef VITA_SCU_DSP_JIT
  { extern int ScuDspJitSelfTest(void); if (ScuDspJitSelfTest() != 0) { YabauseDeInit(); goto done; } }
#endif
#endif
  if (VitaC68kReadTest() != 0) { YabauseDeInit(); goto done; }
  YuiMsg("init_complete");
  YuiMsg("core_timer frequency=%llu frame_ticks=%llu", yabsys.tickfreq, yabsys.OneFrameTime);
  pad = PerPadAdd(&PORTDATA1);
  const unsigned keys[] = {PERPAD_UP,PERPAD_RIGHT,PERPAD_DOWN,PERPAD_LEFT,
    PERPAD_RIGHT_TRIGGER,PERPAD_C,PERPAD_B,PERPAD_A,PERPAD_Y,PERPAD_Z,
    PERPAD_LEFT_TRIGGER,PERPAD_START,PERPAD_X};
  for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i) PerSetKey(i, keys[i], pad);
  uint64_t start = sceKernelGetProcessTimeWide(), last = start;
  uint64_t benchmark_start = start;
  unsigned frames = 0, batch = 0;
  VitaTelemetryThread("emulation");
  VitaTelemetryEnter(VT_FRONTEND);
  while (1) {
    SceCtrlData exit_input = {0};
    sceCtrlPeekBufferPositive(0, &exit_input, 1);
    const unsigned exit_chord = SCE_CTRL_START | SCE_CTRL_SELECT |
      SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    if ((exit_input.buttons & exit_chord) == exit_chord) break;
    VitaTelemetryEnter(VT_INPUT);
    input(frames);
    VitaTelemetryLeave(VT_INPUT);
    VitaTelemetryEnter(VT_SCHEDULER);
    const int exec_result = YabauseExec();
    VitaTelemetryLeave(VT_SCHEDULER);
    if (exec_result < 0) { YuiMsg("execution_failed"); break; }
    ++frames; ++batch;
    uint64_t now = sceKernelGetProcessTimeWide();
#ifdef VITA_ROTATION_ROUTE
    {
      static uint64_t route_last;
      const uint32_t wall = route_last ? (uint32_t)(now - route_last) : 0;
      route_last = now;
      const int was = rotation_route.cpu;
      vita_rotation_cpu = VitaRotationRouteFrame(&rotation_route, wall,
          __atomic_exchange_n(&route_wait_us, 0, __ATOMIC_RELAXED),
          __atomic_exchange_n(&vita_rotation_eligible, 0, __ATOMIC_RELAXED));
      if (was != rotation_route.cpu)
        YuiMsg("rotation_route frame=%u cpu=%d probing=%d base_period_us=%u holdoff=%u switches=%u",
               frames, rotation_route.cpu, rotation_route.probing, rotation_route.base_period,
               rotation_route.holdoff, rotation_route.switches);
    }
#endif
    if (benchmark_frames && frames == benchmark_start_frame)
      benchmark_start = now;
    if (benchmark_frames && frames == benchmark_frames) {
      if (benchmark_start_frame)
        YuiMsg("benchmark_complete frames=%u start_frame=%u elapsed_us=%llu",
               frames, benchmark_start_frame, now - benchmark_start);
      else
        YuiMsg("benchmark_complete frames=%u elapsed_us=%llu", frames, now - benchmark_start);
#ifdef VITA_PGO_GENERATE
      { extern void VitaPgoDump(void); VitaPgoDump(); YuiMsg("pgo_dumped"); }
#endif
    }
    if (now - last >= 2000000) {
      VitaTelemetryReport();
      VitaReportStages();
#ifdef YABAUSE_VITAGL
      if (capture_frame) {
        /* Capture runs only: identify checked vitaGL allocation recovery.
         * This exported counter belongs to our pinned private dependency. */
        extern int unsafe_allocator_counter;
        extern SceGxmShaderPatcher *gxm_shader_patcher;
        /* VIDOGLSync clears the submission alias; YglTM owns the atlas. */
        YglTextureManager *atlas = YglTM;
        YuiMsg("graphics_memory ram_free=%u vram_free=%u recovery_cycles=%d atlas=%ux%u rows=%u gpu_atlas_height=%u",
          (unsigned)vglMemFree(VGL_MEM_RAM), (unsigned)vglMemFree(VGL_MEM_VRAM),
          unsafe_allocator_counter, atlas ? atlas->width : 0,
          atlas ? atlas->height : 0, atlas ? atlas->yMax : 0,
          atlas ? YglTextureHeight(atlas) : 0);
        YuiMsg("graphics_allocations patch_host=%u patch_buffer=%u patch_vertex=%u patch_fragment=%u",
          sceGxmShaderPatcherGetHostMemAllocated(gxm_shader_patcher),
          sceGxmShaderPatcherGetBufferMemAllocated(gxm_shader_patcher),
          sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_shader_patcher),
          sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_shader_patcher));
      }
#endif
#ifdef VITA_SH2_DYNAREC
      VitaSh2ReportExecution();
#ifdef VITA_DIAG_TIMERS
      VitaDiagTimersReport();
#endif
#endif
#ifdef VITA_SH2_IDLE_SLICE_SKIP
      { extern void VitaSh2IdleReport(void); VitaSh2IdleReport(); }
#endif
#ifdef VITA_STACK_PROFILE
      { extern void ScspAccessReport(void); ScspAccessReport(); }
      { extern void VitaSh2MemReport(void); VitaSh2MemReport(); }
#endif
#ifdef VITA_SCSP_MIX_AFTER_SYNC
      { extern u32 g_scsp_mix_waits, g_scsp_mix_spins;
        YuiMsg("scsp_mix_gate waits=%u spins=%u", g_scsp_mix_waits, g_scsp_mix_spins);
        g_scsp_mix_waits = g_scsp_mix_spins = 0; }
#endif
#ifdef VITA_M68K_IDLE_ORBIT
      {
        extern u32 g_orbit_stats[7];
        YuiMsg("m68k_orbit probes=%u found=%u revalidated=%u deferred_chunks=%u skipped_periods=%u materialized=%u failed=%u",
          g_orbit_stats[0], g_orbit_stats[1], g_orbit_stats[6], g_orbit_stats[2], g_orbit_stats[3], g_orbit_stats[4], g_orbit_stats[5]);
        memset(g_orbit_stats, 0, sizeof(g_orbit_stats));
        extern u32 g_orbit_abort[16], g_orbit_abort_addr[16];
        YuiMsg("m68k_orbit_abort ok=%u rb=%u/%06x rw=%u/%06x wb=%u/%06x ww=%u/%06x irq=%u bad=%u long=%u/%06x overlay=%u changed=%u/%06x",
          g_orbit_abort[0], g_orbit_abort[1], g_orbit_abort_addr[1], g_orbit_abort[2], g_orbit_abort_addr[2],
          g_orbit_abort[3], g_orbit_abort_addr[3], g_orbit_abort[4], g_orbit_abort_addr[4],
          g_orbit_abort[5], g_orbit_abort[6], g_orbit_abort[7], g_orbit_abort_addr[7],
          g_orbit_abort[8], g_orbit_abort[9], g_orbit_abort_addr[9]);
        memset(g_orbit_abort, 0, sizeof(g_orbit_abort));
      }
#endif
#ifdef VITA_SCSP_EARLY_FRAME
      { extern u32 g_early_stats[5];
        YuiMsg("scsp_early commits=%u rollbacks=%u self_rollbacks=%u commit_waits=%u completed=%u",
               g_early_stats[0], g_early_stats[1], g_early_stats[2], g_early_stats[3], g_early_stats[4]);
        memset(g_early_stats, 0, sizeof(g_early_stats)); }
#endif
      YuiMsg("progress frames=%u elapsed_us=%llu fps=%.3f presentation_us=%llu copy_us=%llu master_pc=%08x slave_pc=%08x",
        frames, now-start, batch * 1000000.0/(now-last), present_us, copy_us,
        SH2Core->GetPC(MSH2), SH2Core->GetPC(SSH2));
      last = now; batch = 0; present_us = 0; copy_us = 0;
    }
  }
  VitaTelemetryLeave(VT_FRONTEND);
  VitaTelemetryReport();
  YabauseDeInit();
done:
#ifdef YABAUSE_VITAGL
  /* Pinned vitaGL has no public teardown entry point. Drain before process
   * exit; the OS reclaims its context/pools. Never initialize another owner. */
  if (graphics_ready) glFinish();
#endif
#ifdef YABAUSE_GXM_COMPOSITOR
  if (VitaGxmDeviceDestroy(&gxm) < 0) YuiMsg("gxm_cleanup_failed_at_exit");
#endif
  YuiMsg("exit");
  if (logfile) fclose(logfile);
  sceAppUtilShutdown(); sceKernelExitProcess(0); return 0;
}
