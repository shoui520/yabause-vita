/* SPDX-License-Identifier: GPL-2.0-or-later
 * Execute the production CPU producer. GPU precision is validated on device. */
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
typedef struct YglTextureManager YglTextureManager;
enum { OVERMODE_SELPATNAME = 1 };
#define YGL_ROTATION_QUEUE_ONLY
#define VITA_ROTATION_UNIFORMS
#define VITA_ROTATION_FLOAT_COEFFICIENTS
#define VITA_ROTATION_MAP_CACHE
#include "../src/video/opengl/rotation_gpu.inc"
#include "../src/video/opengl/rotation_variant.h"
int main(void) {
  const int formats[] = {0,1,3,4};
  for (int g=0; g<4; ++g) {
    assert(YglRotationVariant(formats[g],1,0,0,0)==g*7);
    for (int cells=1; cells<=2; ++cells) {
      for (int aux=0; aux<=1; ++aux)
        assert(YglRotationVariant(formats[g],0,cells,1,aux)==g*7+1+(cells-1)*3+aux);
      assert(YglRotationVariant(formats[g],0,cells,2,0)==g*7+3+(cells-1)*3);
      assert(YglRotationVariant(formats[g],0,cells,2,1)==g*7+3+(cells-1)*3);
    }
  }
  assert(YglRotationVariant(2,1,1,1,0)==-1);
  assert(YglRotationVariant(5,1,1,1,0)==-1);
  assert(YglRotationVariant(0,0,3,1,0)==-1);
  assert(YglRotationVariant(0,0,1,3,0)==-1);
  assert(YglRotationVariant(0,0,1,1,2)==-1);
  unsigned char *vram = malloc(0x80000); assert(vram);
  for (unsigned i=0;i<0x80000;++i) vram[i] = (unsigned char)(i*17);
  RBGDrawInfo r = {.info={.isbitmap=1,.colornumber=3,.cellw=512,.cellh=256,.alpha=255,.transparencyenable=1}, .c={8,16},
                  .rotate_mval_h=1, .rotate_mval_v=1, .hres=352, .vres=224};
  vdp2rotationparameter_struct p = {.coefenab=1, .dx=1, .kx=1, .ky=1};
  r.vram_generation = 17;
  assert(YglVitaRotationBegin(&r,&p,vram)==0);
  assert(vita_rotation_jobs[0]->map_generation == r.vram_generation);
  r.vram_generation = 18;
  assert(vita_rotation_jobs[0]->map_generation == 17);
  assert(memcmp(vita_rotation_jobs[0]->vram,vram,0x80000)==0);
  unsigned char first=vram[0]; vram[0]^=255;
  assert(vita_rotation_jobs[0]->vram[0]==first);
  p.Xsp=-123.125f; p.Ysp=511.25f; p.kx=-1.0f; p.ky=0.75f;
  YglVitaRotationRow(0,223,&p);
  const float expected[]={p.Xsp,p.Ysp,p.kx,p.ky};
  assert(memcmp(vita_rotation_jobs[0]->rows[223],expected,sizeof(expected))==0);
  YglVitaRotationRow(0,0,NULL);
  assert(vita_rotation_jobs[0]->rows[0][2] < -1000000.0f);
  assert(offsetof(VitaRotationJob,rows)==0x80000);
  assert(sizeof(vita_rotation_jobs[0]->rows)==512*4*4);
  p.deltaKAx=0.25f; assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  p.deltaKAx=0; r.info.colornumber=2; assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  r.info.colornumber=3; r.info.isbitmap=0; assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  r.info.isbitmap=1; r.vres=513; assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  r.vres=224; r.info.LineColorBase=1; assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  r.info.LineColorBase=0; r.info.colornumber=4;
  assert(YglVitaRotationBegin(&r,&p,vram)==1);
  assert(vita_rotation_jobs[0]->pixel[2]==3);
  assert(vita_rotation_jobs[1]->pixel[2]==4);
  assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  assert(vita_rotation_count==2 && !vita_rotation_written);
  vita_rotation_count = 0;
  r.info.isbitmap=0; r.info.colornumber=1; r.info.patternwh=2;
  r.info.patterndatasize=1; r.info.planew=2; r.info.supplementdata=0x301;
  p.ShiftPaneX=10; p.ShiftPaneY=9; p.MaxH=4096; p.MaxV=2048;
  for (unsigned i=0;i<16;++i) p.PlaneAddrv[i]=i*0x800;
  assert(YglVitaRotationBegin(&r,&p,vram)==0);
  assert(vita_rotation_jobs[0]->bitmap[1]==4096 && vita_rotation_jobs[0]->bitmap[2]==2048);
  assert(vita_rotation_jobs[0]->pattern[3]==0 && vita_rotation_jobs[0]->map[3]==2);
  for (unsigned i=0;i<16;++i) assert(vita_rotation_jobs[0]->planes[i]==p.PlaneAddrv[i]);
  r.info.specialcolormode=3;
  assert(YglVitaRotationBegin(&r,&p,vram)==-1);
  VitaRotationJob *job = vita_rotation_jobs[0];
  memset(job->rows, 0, sizeof(job->rows));
  assert(YglVitaRotationConstK(job));
  job->rows[1][2] = 1;
  assert(!YglVitaRotationConstK(job));
  job->rows[1][2] = 0;
  for (unsigned mode = 0; mode < 4; ++mode) {
    job->bitmap[3] = mode;
    assert(YglVitaRotationFloatSafe(job) == (mode == 1 ? -1 : 1));
  }
  job->bitmap[3] = 0;
  job->bitmap[1] = 4097;
  assert(YglVitaRotationFloatSafe(job) == -1);
  job->bitmap[1] = 4096;
  job->map[0] = 8;
  assert(YglVitaRotationFloatSafe(job) == -1);
  job->map[0] = 10;
  const uint32_t rejected[] = {1, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc12345u};
  for (unsigned i = 0; i < sizeof(rejected)/sizeof(rejected[0]); ++i) {
    memcpy(&job->rows[0][0], &rejected[i], 4);
    assert(YglVitaRotationFloatSafe(job) == -2);
  }
  job->rows[0][0] = -0.1234567f;
  assert(YglVitaRotationFloatSafe(job) == 1);
  free(vram);
  for(unsigned i=0;i<2;++i) free(vita_rotation_jobs[i]);
  puts("GPU rotation producer: snapshot ownership, exact float payload, bounded admission and CPU fallback passed");
}
