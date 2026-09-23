/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production producer with VITA_ROTATION_VRAM_REUSE: copy/reuse decisions from
 * the VDP2 VRAM write generation, which the GPU consumer relies on. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  int isbitmap, colornumber, LineColorBase, cellw, cellh, alpha, transparencyenable;
  int specialcolormode, patternwh, patterndatasize, planew, auxmode, supplementdata;
  int coloroffset, paladdr, specialcode, specialprimode, priority, specialfunction, specialcolorfunction;
} vdp2draw_struct;
typedef struct {
  int screenover, coefmode, coefenab, charaddr;
  float deltaKAx, dx, dy, Xp, Yp, Xsp, Ysp, kx, ky;
  int ShiftPaneX, ShiftPaneY, MaxH, MaxV;
  unsigned PlaneAddrv[16];
} vdp2rotationparameter_struct;
typedef struct {
  vdp2draw_struct info;
  struct { unsigned x,y; } c;
  float rotate_mval_h, rotate_mval_v;
  int hres, vres;
  uint32_t vram_generation;
} RBGDrawInfo;
typedef uint32_t u32;
typedef struct YglTextureManager YglTextureManager;
enum { OVERMODE_SELPATNAME = 1 };
#define VITA_ROTATION_VRAM_REUSE
#define YGL_ROTATION_QUEUE_ONLY
#include "../src/video/opengl/rotation_gpu.inc"

int main(void) {
  unsigned char *vram = malloc(0x80000);
  assert(vram);
  for (unsigned i = 0; i < 0x80000; ++i) vram[i] = (unsigned char)(i * 29 + 7);
  RBGDrawInfo r = {.info={.isbitmap=1,.colornumber=3,.cellw=512,.cellh=256,.alpha=255,.transparencyenable=1}, .c={0,0},
                  .rotate_mval_h=1, .rotate_mval_v=1, .hres=352, .vres=224, .vram_generation=0};
  vdp2rotationparameter_struct p = {.coefenab=1, .dx=1, .kx=1, .ky=1};
  /* First job ever copies, even at source generation 0. */
  assert(YglVitaRotationBegin(&r, &p, vram) == 0);
  VitaRotationJob *j0 = vita_rotation_jobs[0];
  assert(j0->vram_copied == 1 && j0->vram_generation == 1);
  assert(memcmp(j0->vram, vram, 0x80000) == 0);
  /* Same hand-off generation within a frame: reuse. */
  assert(YglVitaRotationBegin(&r, &p, vram) == 1);
  VitaRotationJob *j1 = vita_rotation_jobs[1];
  assert(j1->vram_copied == 0 && j1->vram_generation == 1);
  vita_rotation_count = 0; /* consumer flush */
  /* Next frame, no writes: no copy, job buffer untouched. */
  memset(j0->vram, 0xAB, 16);
  assert(YglVitaRotationBegin(&r, &p, vram) == 0);
  assert(j0->vram_copied == 0 && j0->vram_generation == 1 && j0->vram[0] == 0xAB);
  /* Any write (even one restoring the old byte) changes the generation. */
  vram[0x7ffff] ^= 1; ++r.vram_generation;
  assert(YglVitaRotationBegin(&r, &p, vram) == 1);
  assert(j1->vram_copied == 1 && j1->vram_generation == 2 && memcmp(j1->vram, vram, 0x80000) == 0);
  vita_rotation_count = 0;
  vram[0x7ffff] ^= 1; ++r.vram_generation;
  assert(YglVitaRotationBegin(&r, &p, vram) == 0);
  assert(j0->vram_copied == 1 && j0->vram_generation == 3 && memcmp(j0->vram, vram, 0x80000) == 0);
  /* Snapshot isolation: live writes after the copy never alter the job. */
  unsigned char copied = j0->vram[5]; vram[5] ^= 0x80;
  assert(j0->vram[5] == copied);
  /* Generation counter wrap: equality is all that matters. */
  vita_rotation_count = 0;
  r.vram_generation = 0xffffffffu;
  assert(YglVitaRotationBegin(&r, &p, vram) == 0 && j0->vram_copied == 1 && j0->vram_generation == 4);
  r.vram_generation = 0;
  assert(YglVitaRotationBegin(&r, &p, vram) == 1 && j1->vram_copied == 1 && j1->vram_generation == 5);
  vita_rotation_count = 0;
  /* Admission failures do not consume or advance the snapshot state. */
  r.info.colornumber = 2;
  assert(YglVitaRotationBegin(&r, &p, vram) == -1 && vita_rotation_generation == 5);
  r.info.colornumber = 3;
  assert(YglVitaRotationBegin(&r, &p, vram) == 0 && j0->vram_copied == 0 && j0->vram_generation == 5);
  vita_rotation_count = 0;
  (void)vita_rotation_uploaded; (void)vita_rotation_reuse_jobs; (void)vita_rotation_reuse_uploads;
  (void)vita_rotation_written;
  for (unsigned i = 0; i < VITA_ROTATION_JOBS; ++i) free(vita_rotation_jobs[i]);
  free(vram);
  puts("rotation_vram_reuse_test_pass");
  return 0;
}
