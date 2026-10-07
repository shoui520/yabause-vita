/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VIDCore proxy and render thread (VITA_RENDER_THREAD).
 *
 * The renderer library is compiled against a shadow VDP instance (every
 * vdp1.cpp / vdp2.cpp / vidshared.c symbol renamed rs_*, render_shadow_*),
 * so it only ever reads rs_* state. Each renderer entry point the core calls
 * becomes a command for the render thread, which owns vitaGL: the command
 * carries the emulated VDP registers as of the call and references snapshot
 * buffers of VDP1 RAM, VDP2 RAM, colour RAM and the per-line register copies
 * as of the call. The render thread points the rs_* state at them and runs
 * the real renderer function, in call order. The renderer therefore sees
 * exactly what it would have seen running inline.
 *
 * Snapshots are incremental: the VDP write handlers stamp each written 4 KiB
 * page (each Vdp2Lines row) with the current snapshot epoch; a buffer
 * brought up to date copies only what was stamped since its last update, and
 * an area with no write since the newest buffer reuses that buffer.
 *
 * Core-visible effects of the renderer stay on the core side:
 *  - VDP1 drawing: the renderer's Vdp1DrawStart walks the command list on
 *    the shadow; the core's own walk (Vdp1DrawCommands on the real state)
 *    runs on the emulation thread with callbacks that make exactly the
 *    renderer's register writes (clipping, local coordinates, EDSR on a bad
 *    sprite command) and draw nothing.
 *  - VRAM bank update flags: set by the core, cleared by the renderer; each
 *    command carries the core's sets since the previous command.
 *  - Colour RAM notifications: queued in order with the written words and
 *    the colour mode of the write.
 * Calls that return renderer state or touch the VDP1 framebuffer wait for
 * the render thread to reach them. */
#ifdef VITA_RENDER_THREAD
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "core.h"
#include "vdp1.h"
#include "vdp2.h"
#include "memory.h"
#include "vidogl.h"
#include "threads.h"

extern VideoInterface_struct VIDOGL;
extern void Vdp1DrawCommands(u8 *ram, Vdp1 *regs, u8 *back_framebuffer);
extern u8 *Vdp1FrameBuffer[];
extern void YuiMsg(const char *, ...);
extern void VitaGraphicsAdoptOwner(void);   /* main.c */

/* Shadow instance (render_shadow_*). */
extern u8 *rs_Vdp1Ram, *rs_Vdp2Ram, *rs_Vdp2ColorRam;
extern u32 rs_vdp1_page_ver[128], rs_vdp2_page_ver[128];
extern Vdp1 *rs_Vdp1Regs;
extern Vdp2 *rs_Vdp2Regs;
extern Vdp2 rs_Vdp2Lines[270];
extern Vdp1External_struct rs_Vdp1External;
extern Vdp2External_struct rs_Vdp2External;
extern Vdp2Internal_struct rs_Vdp2Internal;
extern u8 rs_A0_Updated, rs_A1_Updated, rs_B0_Updated, rs_B1_Updated;
extern u8 *rs_Vdp1FrameBuffer[];
extern VideoInterface_struct *rs_VIDCore;
#if defined(VITA_ROTATION_VRAM_REUSE) || defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
#define RP_GENERATION 1
extern u32 rs_Vdp2RamGeneration;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
#define RP_GENERATION_EPOCH 1
extern u32 rs_Vdp2RamGenerationEpoch;
#endif
#endif

/* ---- snapshot buffers -------------------------------------------------- */
enum { AREA_VDP1, AREA_VDP2, AREA_CRAM, AREA_LINES, AREAS };
#define POOL 3
typedef struct { u8 *data; u32 synced; int refs; } Buf;
static Buf pool[AREAS][POOL];
static int newest[AREAS] = {-1, -1, -1, -1};
static const u32 area_size[AREAS] = {0x80000, 0x80000, 0x1000, sizeof(Vdp2) * 270};

/* Kernel semaphores only (pthread condition variables lost wakeups here:
 * both threads were found waiting on an empty ring). lock: binary; items:
 * queued commands; space: free ring slots; release: snapshot buffers freed;
 * done: a waited-for command completed (only the emulation thread waits). */
static SceUID sem_lock, sem_items, sem_space, sem_release, sem_done;
static int buf_waiters;
#define LOCK() sceKernelWaitSema(sem_lock, 1, NULL)
#define UNLOCK() sceKernelSignalSema(sem_lock, 1)

static const u8 *AreaSource(int a) {
  switch (a) {
    case AREA_VDP1: return Vdp1Ram;
    case AREA_VDP2: return Vdp2Ram;
    case AREA_CRAM: return Vdp2ColorRam;
    default: return (const u8 *)Vdp2Lines;
  }
}
static u32 AreaTouched(int a) {
  switch (a) {
    case AREA_VDP1: return vdp1_touched;
    case AREA_VDP2: return vdp2_touched;
    case AREA_CRAM: return cram_touched;
    default: return lines_touched;
  }
}
/* Bring buffer b of area a up to date (the caller holds a reference). */
static void AreaUpdate(int a, Buf *b) {
  const u8 *src = AreaSource(a);
  if (!src) { memset(b->data, 0, area_size[a]); b->synced = 0; return; }
  switch (a) {
    case AREA_VDP1: case AREA_VDP2: {
      const u32 *ver = a == AREA_VDP1 ? vdp1_page_ver : vdp2_page_ver;
      for (unsigned p = 0; p < 128; ++p)
        if (ver[p] >= b->synced) memcpy(b->data + p * 4096, src + p * 4096, 4096);
      break;
    }
    case AREA_CRAM: memcpy(b->data, src, area_size[a]); break;
    default:
      for (unsigned l = 0; l < 270; ++l)
        if (vdp2_line_ver[l] >= b->synced) memcpy(b->data + l * sizeof(Vdp2), src + l * sizeof(Vdp2), sizeof(Vdp2));
      break;
  }
  b->synced = vdp_snap_epoch + 1;
}
/* Reference a buffer holding area a as of now (called with mtx held). */
static int AreaAcquire(int a) {
  int n = newest[a];
  if (n >= 0 && pool[a][n].synced && AreaTouched(a) < pool[a][n].synced) { ++pool[a][n].refs; return n; }
  for (;;) {
    int pick = -1;
    if (n >= 0 && pool[a][n].refs == 0) pick = n;                 /* fewest pages to copy */
    else for (int i = 0; i < POOL; ++i) if (pool[a][i].refs == 0) { pick = i; break; }
    if (pick >= 0) {
      Buf *b = &pool[a][pick];
      b->refs = 1;
      UNLOCK();
      AreaUpdate(a, b);
      LOCK();
      newest[a] = pick;
      return pick;
    }
    ++buf_waiters;
    UNLOCK();
    sceKernelWaitSema(sem_release, 1, NULL);
    LOCK();
  }
}

/* ---- commands --------------------------------------------------------- */
enum { FN_INIT, FN_DEINIT, FN_RESIZE, FN_ISFULL, FN_V1RESET, FN_V1START, FN_V1END, FN_FBREAD, FN_FBWRITE,
       FN_ERASE, FN_FCHANGE, FN_V2RESET, FN_V2START, FN_V2END, FN_V2SCREENS, FN_GLSIZE, FN_SETTING, FN_SYNC,
       FN_NATIVE, FN_DISPOFF, FN_CRAM, FN_FBWRITES, FN_CALL, FN_QUIT };
typedef struct {
  int fn, full;
  u32 a[6];
  void *p[3];
  int buf[AREAS];
  Vdp1 v1; Vdp2 v2;
  Vdp1External_struct e1; Vdp2External_struct e2; Vdp2Internal_struct in;
  u8 flags;
  u32 gen, gen_epoch;
  u32 v1ver[128], v2ver[128]; /* vdp1/vdp2_page_ver as of the snapshot */
  int ret;
  volatile int *done;
} Cmd;
#define RING 32
static Cmd ring[RING];
static unsigned ring_head, ring_tail;          /* push at head, pop at tail */
static pthread_t render_thread;
static int render_running;
static volatile int render_busy;

static Vdp1 r_v1;                               /* render-side register copies */
static Vdp2 r_v2;
static u8 r_cram[0x1000];                       /* notification scratch */

/* Colour RAM notifications, logged by the emulation thread in write order
 * and queued as one FN_CRAM command per run of consecutive writes: the run
 * is flushed before any other command is queued (so it keeps its place
 * among them) or when it reaches CRAM_BATCH entries. Entries of a queued
 * run stay untouched until the render thread has executed it: at most RING
 * runs are queued, plus the one being logged. */
typedef struct { u32 addr, words, mode; } CramEntry;
#define CRAM_BATCH 256
#define CRAM_LOG (CRAM_BATCH * 64)
static CramEntry cram_log[CRAM_LOG];
static unsigned cram_log_head, cram_run_start;  /* emulation thread only */
static u32 rp_cram_writes;

/* VDP1 framebuffer writes by the guest CPU, logged the same way and queued as
 * one FN_FBWRITES command (with one VDP snapshot) per run: a game that draws
 * through the framebuffer makes tens of thousands of these per frame, and a
 * command with its own snapshot for each one held it under 1 fps. A run is
 * flushed before any other command and before a colour RAM write is logged
 * (and the colour RAM run before a framebuffer write), so every call keeps
 * its order. */
typedef struct { u32 type, addr, val; } FbEntry;
#define FB_BATCH 2048
#define FB_LOG (FB_BATCH * 64)
static FbEntry fb_log[FB_LOG];
static unsigned fb_log_head, fb_run_start;      /* emulation thread only */
static int r_lines_buf = -1; static u32 r_lines_synced;


static void Execute(Cmd *c) {
  if (c->full) {
    rs_Vdp1Ram = pool[AREA_VDP1][c->buf[AREA_VDP1]].data;
    rs_Vdp2Ram = pool[AREA_VDP2][c->buf[AREA_VDP2]].data;
    /* Equal stamps of a page in two snapshots: no write between them, so
     * the renderer may compare stamps instead of the page's bytes. */
    memcpy(rs_vdp1_page_ver, c->v1ver, sizeof(c->v1ver));
    memcpy(rs_vdp2_page_ver, c->v2ver, sizeof(c->v2ver));
    rs_Vdp2ColorRam = pool[AREA_CRAM][c->buf[AREA_CRAM]].data;
    const Buf *lb = &pool[AREA_LINES][c->buf[AREA_LINES]];
    if (c->buf[AREA_LINES] != r_lines_buf || lb->synced != r_lines_synced) {
      memcpy(rs_Vdp2Lines, lb->data, sizeof(rs_Vdp2Lines));
      r_lines_buf = c->buf[AREA_LINES]; r_lines_synced = lb->synced;
    }
    r_v1 = c->v1; r_v2 = c->v2;
    rs_Vdp1Regs = &r_v1; rs_Vdp2Regs = &r_v2;
    rs_Vdp1External = c->e1; rs_Vdp2External = c->e2; rs_Vdp2Internal = c->in;
    if (c->flags & 1) rs_A0_Updated = 1;
    if (c->flags & 2) rs_A1_Updated = 1;
    if (c->flags & 4) rs_B0_Updated = 1;
    if (c->flags & 8) rs_B1_Updated = 1;
#ifdef RP_GENERATION
    rs_Vdp2RamGeneration = c->gen;
#ifdef RP_GENERATION_EPOCH
    rs_Vdp2RamGenerationEpoch = c->gen_epoch;
#endif
#endif
    rs_Vdp1FrameBuffer[0] = Vdp1FrameBuffer[0];
    rs_Vdp1FrameBuffer[1] = Vdp1FrameBuffer[1];
    rs_VIDCore = &VIDOGL;
  }
  switch (c->fn) {
    case FN_INIT: c->ret = VIDOGL.Init(); break;
    case FN_DEINIT: VIDOGL.DeInit(); break;
    case FN_RESIZE: VIDOGL.Resize((int)c->a[0], (int)c->a[1], c->a[2], c->a[3], (int)c->a[4], (int)c->a[5]); break;
    case FN_ISFULL: c->ret = VIDOGL.IsFullscreen(); break;
    case FN_V1RESET: c->ret = VIDOGL.Vdp1Reset(); break;
    case FN_V1START: VIDOGL.Vdp1DrawStart(); break;
    case FN_V1END: VIDOGL.Vdp1DrawEnd(); break;
    case FN_FBREAD: VIDOGL.Vdp1ReadFrameBuffer(c->a[0], c->a[1], c->p[0]); break;
    case FN_FBWRITE: VIDOGL.Vdp1WriteFrameBuffer(c->a[0], c->a[1], c->a[2]); break;
    case FN_ERASE: VIDOGL.Vdp1EraseWrite(); break;
    case FN_FCHANGE: VIDOGL.Vdp1FrameChange(); break;
    case FN_V2RESET: c->ret = VIDOGL.Vdp2Reset(); break;
    case FN_V2START: VIDOGL.Vdp2DrawStart(); break;
    case FN_V2END: VIDOGL.Vdp2DrawEnd(); break;
    case FN_V2SCREENS: VIDOGL.Vdp2DrawScreens(); break;
    case FN_GLSIZE: VIDOGL.GetGlSize((int *)c->p[0], (int *)c->p[1]); break;
    case FN_SETTING: VIDOGL.SetSettingValue((int)c->a[0], (int)c->a[1]); break;
    case FN_SYNC: VIDOGL.Sync(); break;
    case FN_NATIVE: VIDOGL.GetNativeResolution((int *)c->p[0], (int *)c->p[1], (int *)c->p[2]); break;
    case FN_DISPOFF: VIDOGL.Vdp2DispOff(); break;
    case FN_CALL: ((void (*)(void))c->p[0])(); break;
    case FN_FBWRITES:
      for (u32 i = 0; i < c->a[1]; ++i) {
        const FbEntry *e = &fb_log[(c->a[0] + i) % FB_LOG];
        VIDOGL.Vdp1WriteFrameBuffer(e->type, e->addr, e->val);
      }
      break;
    case FN_CRAM: {
      /* Each write in order: the written word(s) and the colour mode of that write. */
      u8 *saved = rs_Vdp2ColorRam;
      Vdp2Internal_struct saved_in = rs_Vdp2Internal;
      rs_Vdp2ColorRam = r_cram;
      for (u32 i = 0; i < c->a[1]; ++i) {
        const CramEntry *e = &cram_log[(c->a[0] + i) % CRAM_LOG];
        memcpy(r_cram + (e->addr & 0xFFC), &e->words, 4);
        rs_Vdp2Internal.ColorMode = (int)e->mode;
        VIDOGL.ColorRamWriteWord(e->addr);
      }
      rs_Vdp2ColorRam = saved; rs_Vdp2Internal = saved_in;
      break;
    }
    default: break;
  }
}

/* Wall time spent executing queued calls (excludes waiting for work). */
static volatile uint64_t render_exec_us;
/* Per-report diagnostics: calls and render-thread time by command, and the
 * emulation thread's blocking on the ring (space) and on waited calls. */
static volatile u32 rp_calls[FN_QUIT + 1];
static volatile uint64_t rp_exec_us[FN_QUIT + 1];
static uint64_t rp_space_wait_us, rp_done_wait_us, rp_snapshot_us;
static u32 rp_waited, rp_fast_reads;
static uint64_t rp_fast_us;   /* every 64th fast read timed, x64 */
void VitaRenderProxyReport(void) {
  static const char *names[FN_QUIT + 1] = {"init","deinit","resize","isfull","v1reset","v1start","v1end",
    "fbread","fbwrite","erase","fchange","v2reset","v2start","v2end","v2screens","glsize","setting","sync",
    "native","dispoff","cram","fbwrites","call","quit"};
  char line[512]; int n = 0;
  for (int f = 0; f <= FN_QUIT; ++f)
    if (rp_calls[f] && n < (int)sizeof(line) - 48)
      n += snprintf(line + n, sizeof(line) - n, " %s=%u/%lluus", names[f], (unsigned)rp_calls[f],
                    (unsigned long long)rp_exec_us[f]);
  YuiMsg("render_proxy space_wait_us=%llu done_wait_us=%llu waited_calls=%u snapshot_us=%llu cram_writes=%u fbread_fast=%u fast_us~%llu calls:%s",
         (unsigned long long)rp_space_wait_us, (unsigned long long)rp_done_wait_us, rp_waited,
         (unsigned long long)rp_snapshot_us, rp_cram_writes, rp_fast_reads, (unsigned long long)rp_fast_us * 64, n ? line : " none");
  for (int f = 0; f <= FN_QUIT; ++f) { rp_calls[f] = 0; rp_exec_us[f] = 0; }
  rp_space_wait_us = rp_done_wait_us = rp_snapshot_us = 0; rp_waited = rp_fast_reads = rp_cram_writes = 0; rp_fast_us = 0;
}
uint64_t VitaRenderBusyUs(void) { const uint64_t v = render_exec_us; render_exec_us = 0; return v; }

static void *RenderMain(void *arg) {
  (void)arg;
  YabThreadSetCurrentThreadAffinityMask(4);
  VitaGraphicsAdoptOwner();
#if defined(VITA_STACK_PROFILE) && defined(VITA_STACK_PROFILE_RENDER)
  { extern void VitaStackAdoptSecondThread(void); VitaStackAdoptSecondThread(); }  /* stack2_sample = render thread */
#endif
  for (;;) {
    sceKernelWaitSema(sem_items, 1, NULL);
    LOCK();
    Cmd *c = &ring[ring_tail % RING];
    render_busy = 1;
    UNLOCK();
    const int quit = c->fn == FN_QUIT;
    const int waited = c->done != NULL;
    if (!quit) {
      const uint64_t t0 = sceKernelGetProcessTimeWide();
      Execute(c);
      const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
      render_exec_us += dt;
      if (c->fn >= 0 && c->fn <= FN_QUIT) { ++rp_calls[c->fn]; rp_exec_us[c->fn] += dt; }
#ifdef VITA_STACK_PROFILE
      { extern u32 rp_exec_hist[8]; extern unsigned rp_exec_hn; rp_exec_hist[rp_exec_hn++ & 7] = (u32)c->fn << 24 | (u32)(dt > 0xFFFFFF ? 0xFFFFFF : dt); }
#endif
    }
    LOCK();
    if (c->full) for (int a = 0; a < AREAS; ++a) --pool[a][c->buf[a]].refs;
    if (waited) *c->done = c->ret;
    ++ring_tail;
    render_busy = 0;
    const int wake = buf_waiters;
    buf_waiters = 0;
    UNLOCK();
    if (wake) sceKernelSignalSema(sem_release, wake);
    sceKernelSignalSema(sem_space, 1);
    if (waited) sceKernelSignalSema(sem_done, 1);
    if (quit) break;
  }
  return NULL;
}

static void StartThread(void) {
  if (render_running) return;
  sem_lock = sceKernelCreateSema("render_lock", 0, 1, 1, NULL);
  sem_items = sceKernelCreateSema("render_items", 0, 0, RING, NULL);
  sem_space = sceKernelCreateSema("render_space", 0, RING, RING, NULL);
  sem_release = sceKernelCreateSema("render_release", 0, 0, 1024, NULL);
  sem_done = sceKernelCreateSema("render_done", 0, 0, 1, NULL);
  if (sem_lock < 0 || sem_items < 0 || sem_space < 0 || sem_release < 0 || sem_done < 0) abort();
  for (int a = 0; a < AREAS; ++a)
    for (int i = 0; i < POOL; ++i) {
      pool[a][i].data = (u8 *)malloc(area_size[a]);
      if (!pool[a][i].data) abort();
      pool[a][i].synced = 0; pool[a][i].refs = 0;
    }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  if (pthread_create(&render_thread, &attr, RenderMain, NULL) != 0) abort();
  pthread_attr_destroy(&attr);
  render_running = 1;
  YuiMsg("render_thread started pool=%d ring=%d", POOL, RING);
}

/* Queue a call; full = with the VDP state as of now. wait = until executed. */
static int rp_after_fbread;   /* the last queued command was a framebuffer read */
static int Submit(int fn, int full, int wait, const u32 *args, int nargs, void *p0, void *p1, void *p2);
static void CramFlush(void) {
  const u32 a[2] = {cram_run_start, cram_log_head - cram_run_start};
  cram_run_start = cram_log_head;
  Submit(FN_CRAM, 0, 0, a, 2, NULL, NULL, NULL);
}
static void FbFlush(void) {
  const u32 a[2] = {fb_run_start, fb_log_head - fb_run_start};
  fb_run_start = fb_log_head;
  Submit(FN_FBWRITES, 1, 0, a, 2, NULL, NULL, NULL);
}
static int Submit(int fn, int full, int wait, const u32 *args, int nargs, void *p0, void *p1, void *p2) {
  volatile int result = 0;
  if (fn != FN_CRAM && cram_log_head != cram_run_start) CramFlush();
  if (fn != FN_FBWRITES && fb_log_head != fb_run_start) FbFlush();
  rp_after_fbread = fn == FN_FBREAD;
#ifdef VITA_STACK_PROFILE
  { extern u32 rp_fn_hist; rp_fn_hist = rp_fn_hist << 5 | (u32)fn; }
#endif
  uint64_t t0 = sceKernelGetProcessTimeWide();
  sceKernelWaitSema(sem_space, 1, NULL);
  uint64_t t1 = sceKernelGetProcessTimeWide();
  rp_space_wait_us += t1 - t0;
  LOCK();
  Cmd *c = &ring[ring_head % RING];
  c->fn = fn; c->full = full; c->ret = 0; c->done = wait ? (volatile int *)&result : NULL;
  for (int i = 0; i < nargs; ++i) c->a[i] = args[i];
  c->p[0] = p0; c->p[1] = p1; c->p[2] = p2;
  if (full) {
    for (int a = 0; a < AREAS; ++a) c->buf[a] = AreaAcquire(a);
    memcpy(c->v1ver, vdp1_page_ver, sizeof(c->v1ver));
    memcpy(c->v2ver, vdp2_page_ver, sizeof(c->v2ver));
    ++vdp_snap_epoch;                              /* later writes: newer than these buffers */
    if (Vdp1Regs) c->v1 = *Vdp1Regs; else memset(&c->v1, 0, sizeof(c->v1));
    if (Vdp2Regs) c->v2 = *Vdp2Regs; else memset(&c->v2, 0, sizeof(c->v2));
    c->e1 = Vdp1External; c->e2 = Vdp2External; c->in = Vdp2Internal;
    c->flags = (A0_Updated ? 1 : 0) | (A1_Updated ? 2 : 0) | (B0_Updated ? 4 : 0) | (B1_Updated ? 8 : 0);
    A0_Updated = A1_Updated = B0_Updated = B1_Updated = 0;
#ifdef RP_GENERATION
    c->gen = Vdp2RamGeneration;
#ifdef RP_GENERATION_EPOCH
    c->gen_epoch = Vdp2RamGenerationEpoch;
#endif
#endif
  }
  ++ring_head;
  UNLOCK();
  if (full) rp_snapshot_us += sceKernelGetProcessTimeWide() - t1;
  sceKernelSignalSema(sem_items, 1);
  if (wait) {
    t0 = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(sem_done, 1, NULL);   /* result written before the signal */
    rp_done_wait_us += sceKernelGetProcessTimeWide() - t0;
    ++rp_waited;
  }
  return wait ? result : 0;
}
#define CALL(fn) Submit(fn, 1, 0, NULL, 0, NULL, NULL, NULL)
#define CALL_WAIT(fn) Submit(fn, 1, 1, NULL, 0, NULL, NULL, NULL)

/* Core-side VDP1 walk callbacks: the renderer's writes to the registers
 * (VIDOGLVdp1UserClipping / SystemClipping / LocalCoordinate, and EDSR on a
 * bad normal-sprite command), nothing drawn. */
static void CoreNormalSprite(u8 *ram, Vdp1 *regs, u8 *fb) {
  vdp1cmd_struct cmd;
  (void)ram; (void)fb;
  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  if (cmd.CMDSIZE & 0x8000) regs->EDSR |= 2;
}
static void CoreNoDraw(u8 *ram, Vdp1 *regs, u8 *fb) { (void)ram; (void)regs; (void)fb; }
static void CoreUserClipping(u8 *ram, Vdp1 *regs) {
  (void)ram; (void)regs;
  Vdp1Regs->userclipX1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xC);
  Vdp1Regs->userclipY1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xE);
  Vdp1Regs->userclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
  Vdp1Regs->userclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);
}
static void CoreSystemClipping(u8 *ram, Vdp1 *regs) {
  (void)ram; (void)regs;
  Vdp1Regs->systemclipX1 = 0;
  Vdp1Regs->systemclipY1 = 0;
  Vdp1Regs->systemclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
  Vdp1Regs->systemclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);
}
static void CoreLocalCoordinate(u8 *ram, Vdp1 *regs) {
  (void)ram; (void)regs;
  Vdp1Regs->localX = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xC);
  Vdp1Regs->localY = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xE);
}

static int ProxyInit(void) { StartThread(); return CALL_WAIT(FN_INIT); }
static void ProxyDeInit(void) {
  if (!render_running) return;
  CALL_WAIT(FN_DEINIT);
  Submit(FN_QUIT, 0, 1, NULL, 0, NULL, NULL, NULL);
  pthread_join(render_thread, NULL);
  render_running = 0;
}
static void ProxyResize(int x, int y, unsigned int w, unsigned int h, int on, int ar) {
  const u32 a[6] = {(u32)x, (u32)y, w, h, (u32)on, (u32)ar};
  Submit(FN_RESIZE, 1, 1, a, 6, NULL, NULL, NULL);
}
static int ProxyIsFullscreen(void) { return CALL_WAIT(FN_ISFULL); }
static int ProxyVdp1Reset(void) { return CALL_WAIT(FN_V1RESET); }
static void ProxyVdp1DrawStart(void) {
  CALL(FN_V1START);
  /* The core's own walk, for its register and status effects. */
  Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, NULL);
}
static void ProxyVdp1DrawEnd(void) { CALL(FN_V1END); }
/* Guest reads of the VDP1 framebuffer come in long runs (Burning Rangers:
 * ~10k per VDP1 frame). A read is answered here, on the emulation thread,
 * when the renderer's inputs to it are exactly those a queued read would
 * see: nothing queued or running (this thread is the only producer, so the
 * render thread cannot start anything during the read), the last command it ran was a
 * framebuffer read (so its shadow state and read-back pixels are still that
 * read's), and the registers the read consults (TVMR, system clip, SPCTL)
 * equal that read's snapshot. The renderer then serves it without GL
 * (VIDOGLVdp1ReadFrameBufferNoGL) or declines, and the read is queued. */
extern int VIDOGLVdp1ReadFrameBufferNoGL(u32 type, u32 addr, void *out);
#ifdef VITA_STACK_PROFILE
extern int _Ygl_pfb_null(void);
u32 rp_fn_hist;
u32 rp_exec_hist[8]; unsigned rp_exec_hn;
#endif
static u16 rp_read_tvmr, rp_read_spctl;
static u16 rp_read_clip_x2, rp_read_clip_y2;
static void ProxyReadFrameBuffer(u32 type, u32 addr, void *out) {
#ifdef VITA_STACK_PROFILE
  int rp_fast_fail = 9;
#endif
  if (rp_after_fbread && Vdp1Regs && Vdp2Regs && Vdp1Regs->TVMR == rp_read_tvmr &&
      Vdp1Regs->systemclipX2 == rp_read_clip_x2 && Vdp1Regs->systemclipY2 == rp_read_clip_y2 &&
      Vdp2Regs->SPCTL == rp_read_spctl) {
    const int timed = (rp_fast_reads & 63) == 0;
    const uint64_t t0 = timed ? sceKernelGetProcessTimeWide() : 0;
    /* Only this thread queues commands, so an empty ring stays empty for the
     * whole read; the acquire pairs with the render thread's release of the
     * ring lock after its last command, making that command's writes visible. */
    const int served = __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) == ring_head &&
                       VIDOGLVdp1ReadFrameBufferNoGL(type, addr, out);
#ifdef VITA_STACK_PROFILE
    rp_fast_fail = __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) != ring_head ? 1 : 2 + _Ygl_pfb_null() * 2;
#endif
    if (timed) rp_fast_us += sceKernelGetProcessTimeWide() - t0;
    if (served) { ++rp_fast_reads; return; }
  }
  const u32 a[2] = {type, addr};
#ifdef VITA_STACK_PROFILE
  { static unsigned n, pend; static u32 last[8]; static uint64_t us[8];
    const int why = !rp_after_fbread ? 0 : __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) != ring_head ? 1 :
      (Vdp1Regs->TVMR != rp_read_tvmr) ? 3 : (Vdp1Regs->systemclipX2 != rp_read_clip_x2 || Vdp1Regs->systemclipY2 != rp_read_clip_y2) ? 4 :
      (Vdp2Regs->SPCTL != rp_read_spctl) ? 5 : (_Ygl_pfb_null() ? 6 : rp_fast_fail);
    const unsigned q = ring_head - __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    const u32 hist = rp_fn_hist;
    Submit(FN_FBREAD, 1, 1, a, 2, out, NULL, NULL);
    if (addr == 0x1BD00 && (n & 63) == 7) { unsigned h = rp_exec_hn; YuiMsg("fbread_exec %u:%u %u:%u %u:%u %u:%u %u:%u wait=%llu",
      rp_exec_hist[(h-5)&7]>>24, rp_exec_hist[(h-5)&7]&0xFFFFFF, rp_exec_hist[(h-4)&7]>>24, rp_exec_hist[(h-4)&7]&0xFFFFFF,
      rp_exec_hist[(h-3)&7]>>24, rp_exec_hist[(h-3)&7]&0xFFFFFF, rp_exec_hist[(h-2)&7]>>24, rp_exec_hist[(h-2)&7]&0xFFFFFF,
      rp_exec_hist[(h-1)&7]>>24, rp_exec_hist[(h-1)&7]&0xFFFFFF, (unsigned long long)(sceKernelGetProcessTimeWide() - t0)); }
    if (addr == 0x1BD00 && (n & 63) == 7) YuiMsg("fbread_hist %d %d %d %d %d %d", (int)(hist >> 25 & 31), (int)(hist >> 20 & 31), (int)(hist >> 15 & 31), (int)(hist >> 10 & 31), (int)(hist >> 5 & 31), (int)(hist & 31));
    const unsigned k = n & 7; last[k] = addr | (u32)why << 28 | (q > 15 ? 15u : q) << 24; us[k] = sceKernelGetProcessTimeWide() - t0;
    if (++n % 64 == 0)
      YuiMsg("fbread_slow n=%u %07x/%u/%llu %07x/%u/%llu %07x/%u/%llu %07x/%u/%llu", n,
        last[0] & 0xFFFFFF, last[0] >> 24, us[0], last[1] & 0xFFFFFF, last[1] >> 24, us[1],
        last[2] & 0xFFFFFF, last[2] >> 24, us[2], last[3] & 0xFFFFFF, last[3] >> 24, us[3]);
    (void)pend; }
#else
  Submit(FN_FBREAD, 1, 1, a, 2, out, NULL, NULL);
#endif
  if (Vdp1Regs && Vdp2Regs) {
    rp_read_tvmr = Vdp1Regs->TVMR; rp_read_spctl = Vdp2Regs->SPCTL;
    rp_read_clip_x2 = Vdp1Regs->systemclipX2; rp_read_clip_y2 = Vdp1Regs->systemclipY2;
  } else rp_after_fbread = 0;
}
#ifdef VITA_FB_DIRECT_READ
/* ProxyReadFrameBuffer's answer to a word read when it serves the read on
 * this thread, for the SH-2 read helpers ahead of the VIDCore call; 0 when
 * the read must take the full path. */
extern int VIDOGLVdp1ReadWordNoGL(u32 addr, u16 *out);
int VitaFbReadWord(u32 addr, u16 *out) {
  if (!rp_after_fbread || !Vdp1Regs || !Vdp2Regs || VIDCore->Vdp1ReadFrameBuffer != ProxyReadFrameBuffer ||
      Vdp1Regs->TVMR != rp_read_tvmr || Vdp1Regs->systemclipX2 != rp_read_clip_x2 ||
      Vdp1Regs->systemclipY2 != rp_read_clip_y2 || Vdp2Regs->SPCTL != rp_read_spctl ||
      __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) != ring_head ||
      !VIDOGLVdp1ReadWordNoGL(addr & 0x3FFFF, out))
    return 0;
  ++rp_fast_reads;
  return 1;
}
#endif
static void ProxyWriteFrameBuffer(u32 type, u32 addr, u32 val) {
  /* Nothing returned: queued in order like the draws (reads drain the
   * queue, so the core never observes a pending write). */
  if (!render_running) return;
  if (cram_log_head != cram_run_start) CramFlush();
  FbEntry *e = &fb_log[fb_log_head % FB_LOG];
  e->type = type; e->addr = addr; e->val = val;
  rp_after_fbread = 0;   /* as when each write was its own command */
  if (++fb_log_head - fb_run_start == FB_BATCH) FbFlush();
}
static void ProxyEraseWrite(void) { CALL(FN_ERASE); }
static void ProxyFrameChange(void) { CALL(FN_FCHANGE); }
static int ProxyVdp2Reset(void) { return CALL_WAIT(FN_V2RESET); }
static void ProxyVdp2DrawStart(void) { CALL(FN_V2START); }
static void ProxyVdp2DrawEnd(void) { CALL(FN_V2END); }
static void ProxyVdp2DrawScreens(void) { CALL(FN_V2SCREENS); }
static void ProxyGetGlSize(int *w, int *h) { Submit(FN_GLSIZE, 0, 1, NULL, 0, w, h, NULL); }
static void ProxySetSetting(int type, int value) {
  const u32 a[2] = {(u32)type, (u32)value};
  Submit(FN_SETTING, 1, 1, a, 2, NULL, NULL, NULL);
}
static void ProxySync(void) { CALL(FN_SYNC); }
static void ProxyGetNativeResolution(int *w, int *h, int *i) { Submit(FN_NATIVE, 0, 1, NULL, 0, w, h, i); }
static void ProxyDispOff(void) { CALL(FN_DISPOFF); }
static void ProxyColorRamWriteWord(u32 addr) {
  if (!render_running || !Vdp2ColorRam) return;
  if (fb_log_head != fb_run_start) FbFlush();
  CramEntry *e = &cram_log[cram_log_head % CRAM_LOG];
  e->addr = addr;
  memcpy(&e->words, Vdp2ColorRam + (addr & 0xFFC), 4);
  e->mode = (u32)Vdp2Internal.ColorMode;
  ++rp_cram_writes;
  rp_after_fbread = 0;   /* as when each write was its own command */
  if (++cram_log_head - cram_run_start == CRAM_BATCH) CramFlush();
}

/* Run fn on the render thread after every queued call, and wait for it
 * (graphics work requested from another thread, e.g. the present after a
 * state load in YabLoadStateStream). */
int VitaRenderCall(void (*fn)(void)) {
  if (!render_running || pthread_equal(pthread_self(), render_thread)) return -1;
  Submit(FN_CALL, 0, 1, NULL, 0, (void *)fn, NULL, NULL);
  return 0;
}

/* Wait until every queued renderer call has run (frame capture, exit). */
static void NoOp(void) {}
void VitaRenderDrain(void) {
  if (render_running) VitaRenderCall(NoOp);
}

VideoInterface_struct VIDProxy = {
  VIDCORE_OGL, "OpenGL Video Interface (render thread)",
  ProxyInit, ProxyDeInit, ProxyResize, ProxyIsFullscreen,
  ProxyVdp1Reset, ProxyVdp1DrawStart, ProxyVdp1DrawEnd,
  CoreNormalSprite, CoreNoDraw, CoreNoDraw, CoreNoDraw, CoreNoDraw, CoreNoDraw,
  CoreUserClipping, CoreSystemClipping, CoreLocalCoordinate,
  ProxyReadFrameBuffer, ProxyWriteFrameBuffer, ProxyEraseWrite, ProxyFrameChange,
  ProxyVdp2Reset, ProxyVdp2DrawStart, ProxyVdp2DrawEnd, ProxyVdp2DrawScreens,
  ProxyGetGlSize, ProxySetSetting, ProxySync, ProxyGetNativeResolution, ProxyDispOff,
  .ColorRamWriteWord = ProxyColorRamWriteWord
};
#endif
