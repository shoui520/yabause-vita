/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production producer with VITA_ROTATION_TARGET: the composition request must
 * claim exactly the admitted job whose atlas rectangle it samples. */
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
#define VITA_ROTATION_TARGET
#define YGL_ROTATION_QUEUE_ONLY
#include "../src/video/opengl/rotation_gpu.inc"

int main(void) {
  unsigned char *vram = calloc(1, 0x80000);
  assert(vram);
  RBGDrawInfo r = {.info={.isbitmap=1,.colornumber=3,.cellw=512,.cellh=256,.alpha=255,.transparencyenable=1}, .c={16,48},
                  .rotate_mval_h=1, .rotate_mval_v=1, .hres=352, .vres=224};
  vdp2rotationparameter_struct p = {.coefenab=1, .dx=1, .kx=1, .ky=1};
  assert(YglVitaRotationTarget(16, 48, 352, 224) == 0);      /* nothing admitted */
  assert(YglVitaRotationBegin(&r, &p, vram) == 0);
  assert(vita_rotation_jobs[0]->target == 0);                 /* atlas by default */
  r.c.x = 0; r.c.y = 300; r.hres = 320; r.vres = 240;
  assert(YglVitaRotationBegin(&r, &p, vram) == 1);
  assert(YglVitaRotationTarget(16, 48, 352, 223) == 0);       /* size must match */
  assert(YglVitaRotationTarget(17, 48, 352, 224) == 0);       /* origin must match */
  assert(YglVitaRotationTarget(0, 300, 320, 240) == 2 && vita_rotation_jobs[1]->target == 1);
  assert(vita_rotation_jobs[0]->target == 0);
  assert(YglVitaRotationTarget(16, 48, 352, 224) == 1 && vita_rotation_jobs[0]->target == 1);
  vita_rotation_count = 0;                                     /* consumer flush */
  r.c.x = 16; r.c.y = 48; r.hres = 352; r.vres = 224;
  assert(YglVitaRotationBegin(&r, &p, vram) == 0);
  assert(vita_rotation_jobs[0]->target == 0);                 /* flag reset per job */
  assert(YglVitaRotationTarget(0, 300, 320, 240) == 0);       /* stale slot not matched */
  vita_rotation_count = 0;
  (void)vita_rotation_written;
  for (unsigned i = 0; i < VITA_ROTATION_JOBS; ++i) free(vita_rotation_jobs[i]);
  free(vram);
  puts("rotation_target_test_pass");
  return 0;
}
