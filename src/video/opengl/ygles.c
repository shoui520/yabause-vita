/*  Copyright 2019 devMiyax(smiyaxdev@gmail.com)

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
 

#include <stdlib.h>
#include "../../vita/telemetry.h"
#include "../../vita/diag_timers.h"
#include <math.h>
#include "ygl.h"
#include "yui.h"
#include "vidshared.h"
#include "debug.h"
#include "frameprofile.h"

#define NUM_TEXTURE_BUFFER 1

static void YglUnbindPixelUnpack(void) {
#ifndef YABAUSE_VITAGL
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif
}

#define YGLDEBUG
//#define YGLDEBUG printf
//#define YGLDEBUG LOG
//#define YGLDEBUG yprintf
//#define YGLLOG yprintf

extern int YuiGetFB(void);
extern void RBGGenerator_onFinish();
extern u8 * Vdp1FrameBuffer[];
static int rebuild_frame_buffer = 0;

static int YglIsNeedFrameBuffer();
static int YglCalcTextureQ( float   *pnts,float *q);
static void YglRenderDestinationAlpha(void);;
u32 * YglGetColorRamPointer();
void YglRenderFrameBufferShadow();

void Ygl_uniformVDP2DrawFramebuffer_perline(void * p, float from, float to, u32 linetexture);

#define PI 3.1415926535897932384626433832795f

extern vdp2rotationparameter_struct  paraA;
#ifdef YABAUSE_VITAGL
extern int YuiCapturePending(void);
extern void YuiMsg(const char *format, ...);
#endif

#define ATLAS_BIAS (0.025f)

#if (defined(__ANDROID__) || defined(IOS)) && !defined(__LIBRETRO__)
PFNGLPATCHPARAMETERIPROC glPatchParameteri = NULL;
//PFNGLMEMORYBARRIERPROC glMemoryBarrier = NULL;
#endif

void YglScalef(YglMatrix *result, GLfloat sx, GLfloat sy, GLfloat sz)
{
    result->m[0][0] *= sx;
    result->m[0][1] *= sx;
    result->m[0][2] *= sx;
    result->m[0][3] *= sx;

    result->m[1][0] *= sy;
    result->m[1][1] *= sy;
    result->m[1][2] *= sy;
    result->m[1][3] *= sy;

    result->m[2][0] *= sz;
    result->m[2][1] *= sz;
    result->m[2][2] *= sz;
    result->m[2][3] *= sz;
}

void YglTranslatef(YglMatrix *result, GLfloat tx, GLfloat ty, GLfloat tz)
{
    result->m[0][3] += (result->m[0][0] * tx + result->m[0][1] * ty + result->m[0][2] * tz);
    result->m[1][3] += (result->m[1][0] * tx + result->m[1][1] * ty + result->m[1][2] * tz);
    result->m[2][3] += (result->m[2][0] * tx + result->m[2][1] * ty + result->m[2][2] * tz);
    result->m[3][3] += (result->m[3][0] * tx + result->m[3][1] * ty + result->m[3][2] * tz);
}

void YglRotatef(YglMatrix *result, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
   GLfloat sinAngle, cosAngle;
   GLfloat mag = sqrtf(x * x + y * y + z * z);

   sinAngle = sinf ( angle * PI / 180.0f );
   cosAngle = cosf ( angle * PI / 180.0f );
   if ( mag > 0.0f )
   {
      GLfloat xx, yy, zz, xy, yz, zx, xs, ys, zs;
      GLfloat oneMinusCos;
      YglMatrix rotMat;

      x /= mag;
      y /= mag;
      z /= mag;

      xx = x * x;
      yy = y * y;
      zz = z * z;
      xy = x * y;
      yz = y * z;
      zx = z * x;
      xs = x * sinAngle;
      ys = y * sinAngle;
      zs = z * sinAngle;
      oneMinusCos = 1.0f - cosAngle;

      rotMat.m[0][0] = (oneMinusCos * xx) + cosAngle;
      rotMat.m[0][1] = (oneMinusCos * xy) - zs;
      rotMat.m[0][2] = (oneMinusCos * zx) + ys;
      rotMat.m[0][3] = 0.0F;

      rotMat.m[1][0] = (oneMinusCos * xy) + zs;
      rotMat.m[1][1] = (oneMinusCos * yy) + cosAngle;
      rotMat.m[1][2] = (oneMinusCos * yz) - xs;
      rotMat.m[1][3] = 0.0F;

      rotMat.m[2][0] = (oneMinusCos * zx) - ys;
      rotMat.m[2][1] = (oneMinusCos * yz) + xs;
      rotMat.m[2][2] = (oneMinusCos * zz) + cosAngle;
      rotMat.m[2][3] = 0.0F;

      rotMat.m[3][0] = 0.0F;
      rotMat.m[3][1] = 0.0F;
      rotMat.m[3][2] = 0.0F;
      rotMat.m[3][3] = 1.0F;

      YglMatrixMultiply( result, &rotMat, result );
   }
}

void YglFrustum(YglMatrix *result, float left, float right, float bottom, float top, float nearZ, float farZ)
{
    float       deltaX = right - left;
    float       deltaY = top - bottom;
    float       deltaZ = farZ - nearZ;
    YglMatrix    frust;

    if ( (nearZ <= 0.0f) || (farZ <= 0.0f) ||
         (deltaX <= 0.0f) || (deltaY <= 0.0f) || (deltaZ <= 0.0f) )
         return;

    frust.m[0][0] = 2.0f * nearZ / deltaX;
    frust.m[0][1] = frust.m[0][2] = frust.m[0][3] = 0.0f;

    frust.m[1][1] = 2.0f * nearZ / deltaY;
    frust.m[1][0] = frust.m[1][2] = frust.m[1][3] = 0.0f;

    frust.m[2][0] = (right + left) / deltaX;
    frust.m[2][1] = (top + bottom) / deltaY;
    frust.m[2][2] = -(nearZ + farZ) / deltaZ;
    frust.m[2][3] = -1.0f;

    frust.m[3][2] = -2.0f * nearZ * farZ / deltaZ;
    frust.m[3][0] = frust.m[3][1] = frust.m[3][3] = 0.0f;

    YglMatrixMultiply(result, &frust, result);
}


void YglPerspective(YglMatrix *result, float fovy, float aspect, float nearZ, float farZ)
{
   GLfloat frustumW, frustumH;

   frustumH = tanf( fovy / 360.0f * PI ) * nearZ;
   frustumW = frustumH * aspect;

   YglFrustum( result, -frustumW, frustumW, -frustumH, frustumH, nearZ, farZ );
}

void YglOrtho(YglMatrix *result, float left, float right, float bottom, float top, float nearZ, float farZ)
{
    float       deltaX = right - left;
    float       deltaY = top - bottom;
    float       deltaZ = farZ - nearZ;
    YglMatrix    ortho;

    if ( (deltaX == 0.0f) || (deltaY == 0.0f) || (deltaZ == 0.0f) )
        return;

    YglLoadIdentity(&ortho);
    ortho.m[0][0] = 2.0f / deltaX;
    ortho.m[0][3] = -(right + left) / deltaX;
    ortho.m[1][1] = 2.0f / deltaY;
    ortho.m[1][3] = -(top + bottom) / deltaY;
    ortho.m[2][2] = -2.0f / deltaZ;
    ortho.m[2][3] = -(nearZ + farZ) / deltaZ;

    YglMatrixMultiply(result, &ortho, result);
}

void YglTransform(YglMatrix *mtx, float * inXyz, float * outXyz )
{
    outXyz[0] = inXyz[0] * mtx->m[0][0] + inXyz[0] * mtx->m[0][1]  + inXyz[0] * mtx->m[0][2] + mtx->m[0][3];
    outXyz[1] = inXyz[1] * mtx->m[1][0] + inXyz[1] * mtx->m[1][1]  + inXyz[1] * mtx->m[1][2] + mtx->m[1][3];
    outXyz[2] = inXyz[2] * mtx->m[2][0] + inXyz[2] * mtx->m[2][1]  + inXyz[2] * mtx->m[2][2] + mtx->m[2][3];
}

void YglMatrixMultiply(YglMatrix *result, YglMatrix *srcA, YglMatrix *srcB)
{
    YglMatrix    tmp;
    int         i;

    for (i=0; i<4; i++)
    {
        tmp.m[i][0] =   (srcA->m[i][0] * srcB->m[0][0]) +
                        (srcA->m[i][1] * srcB->m[1][0]) +
                        (srcA->m[i][2] * srcB->m[2][0]) +
                        (srcA->m[i][3] * srcB->m[3][0]) ;

        tmp.m[i][1] =   (srcA->m[i][0] * srcB->m[0][1]) +
                        (srcA->m[i][1] * srcB->m[1][1]) +
                        (srcA->m[i][2] * srcB->m[2][1]) +
                        (srcA->m[i][3] * srcB->m[3][1]) ;

        tmp.m[i][2] =   (srcA->m[i][0] * srcB->m[0][2]) +
                        (srcA->m[i][1] * srcB->m[1][2]) +
                        (srcA->m[i][2] * srcB->m[2][2]) +
                        (srcA->m[i][3] * srcB->m[3][2]) ;

        tmp.m[i][3] =   (srcA->m[i][0] * srcB->m[0][3]) +
                        (srcA->m[i][1] * srcB->m[1][3]) +
                        (srcA->m[i][2] * srcB->m[2][3]) +
                        (srcA->m[i][3] * srcB->m[3][3]) ;
    }
    memcpy(result, &tmp, sizeof(YglMatrix));
}


void YglLoadIdentity(YglMatrix *result)
{
    memset(result, 0x0, sizeof(YglMatrix));
    result->m[0][0] = 1.0f;
    result->m[1][1] = 1.0f;
    result->m[2][2] = 1.0f;
    result->m[3][3] = 1.0f;
}


YglTextureManager * YglTM;
//YglTextureManager * YglTM_vdp1;
Ygl * _Ygl;

typedef struct
{
   float s, t, r, q;
} texturecoordinate_struct;


extern int GlHeight;
extern int GlWidth;
extern int vdp1cor;
extern int vdp1cog;
extern int vdp1cob;


#define STD_Q2 (1.0f)
#define EPS (1e-10)
#define EQ(a,b) (abs((a)-(b)) < EPS)
#define IS_ZERO(a) ( (a) < EPS && (a) > -EPS)

// AXB = |A||B|sin
static INLINE float cross2d( float veca[2], float vecb[2] )
{
   return (veca[0]*vecb[1])-(vecb[0]*veca[1]);
}

/*-----------------------------------------
    b1+--+ a1
     /  / \
    /  /   \
  a2+-+-----+b2
      ans

  get intersection point for opssite edge.
--------------------------------------------*/
int FASTCALL YglIntersectionOppsiteEdge(float * a1, float * a2, float * b1, float * b2, float * out )
{
  float veca[2];
  float vecb[2];
  float vecc[2];
  float d1;
  float d2;

  veca[0]=a2[0]-a1[0];
  veca[1]=a2[1]-a1[1];
  vecb[0]=b1[0]-a1[0];
  vecb[1]=b1[1]-a1[1];
  vecc[0]=b2[0]-a1[0];
  vecc[1]=b2[1]-a1[1];
  d1 = cross2d(vecb,vecc);
  if( IS_ZERO(d1) ) return -1;
  d2 = cross2d(vecb,veca);

  out[0] = a1[0]+vecc[0]*d2/d1;
  out[1] = a1[1]+vecc[1]*d2/d1;

  return 0;
}





int YglCalcTextureQ(
   float   *pnts,
   float *q
)
{
   float p1[2],p2[2],p3[2],p4[2],o[2];
   float   q1, q3, q4, qw;
   float   dx, w;
   float   ww;
#if 0
   // fast calculation for triangle
   if (( pnts[2*0+0] == pnts[2*1+0] ) && ( pnts[2*0+1] == pnts[2*1+1] )) {
      q[0] = 1.0f;
      q[1] = 1.0f;
      q[2] = 1.0f;
      q[3] = 1.0f;
      return 0;

   } else if (( pnts[2*1+0] == pnts[2*2+0] ) && ( pnts[2*1+1] == pnts[2*2+1] ))  {
      q[0] = 1.0f;
      q[1] = 1.0f;
      q[2] = 1.0f;
      q[3] = 1.0f;
      return 0;
   } else if (( pnts[2*2+0] == pnts[2*3+0] ) && ( pnts[2*2+1] == pnts[2*3+1] ))  {
      q[0] = 1.0f;
      q[1] = 1.0f;
      q[2] = 1.0f;
      q[3] = 1.0f;
      return 0;
   } else if (( pnts[2*3+0] == pnts[2*0+0] ) && ( pnts[2*3+1] == pnts[2*0+1] )) {
      q[0] = 1.0f;
      q[1] = 1.0f;
      q[2] = 1.0f;
      q[3] = 1.0f;
      return 0;
   }
#endif
   p1[0]=pnts[0];
   p1[1]=pnts[1];
   p2[0]=pnts[2];
   p2[1]=pnts[3];
   p3[0]=pnts[4];
   p3[1]=pnts[5];
   p4[0]=pnts[6];
   p4[1]=pnts[7];

   // calcurate Q1
   if( YglIntersectionOppsiteEdge( p3, p1, p2, p4,  o ) == 0 )
   {
      dx = o[0]-p1[0];
      if( !IS_ZERO(dx) )
      {
         w = p3[0]-p2[0];
         if( !IS_ZERO(w) )
          q1 = fabs(dx/w);
         else
          q1 = 0.0f;
      }else{
         w = p3[1] - p2[1];
         if ( !IS_ZERO(w) )
         {
            ww = ( o[1] - p1[1] );
            if ( !IS_ZERO(ww) )
               q1 = fabs(ww / w);
            else
               q1 = 0.0f;
         } else {
            q1 = 0.0f;
         }
      }
   }else{
      q1 = 1.0f;
   }

   /* q2 = 1.0f; */

   // calcurate Q3
   if( YglIntersectionOppsiteEdge( p1, p3, p2,p4,  o ) == 0 )
   {
      dx = o[0]-p3[0];
      if( !IS_ZERO(dx) )
      {
         w = p1[0]-p2[0];
         if( !IS_ZERO(w) )
          q3 = fabs(dx/w);
         else
          q3 = 0.0f;
      }else{
         w = p1[1] - p2[1];
         if ( !IS_ZERO(w) )
         {
            ww = ( o[1] - p3[1] );
            if ( !IS_ZERO(ww) )
               q3 = fabs(ww / w);
            else
               q3 = 0.0f;
         } else {
            q3 = 0.0f;
         }
      }
   }else{
      q3 = 1.0f;
   }


   // calcurate Q4
   if( YglIntersectionOppsiteEdge( p3, p1, p4, p2,  o ) == 0 )
   {
      dx = o[0]-p1[0];
      if( !IS_ZERO(dx) )
      {
         w = p3[0]-p4[0];
         if( !IS_ZERO(w) )
          qw = fabs(dx/w);
         else
          qw = 0.0f;
      }else{
         w = p3[1] - p4[1];
         if ( !IS_ZERO(w) )
         {
            ww = ( o[1] - p1[1] );
            if ( !IS_ZERO(ww) )
               qw = fabs(ww / w);
            else
               qw = 0.0f;
         } else {
            qw = 0.0f;
         }
      }
      if ( !IS_ZERO(qw) )
      {
         w   = qw / q1;
      }
      else
      {
         w   = 0.0f;
      }
      if ( IS_ZERO(w) ) {
         q4 = 1.0f;
      } else {
         q4 = 1.0f / w;
      }
   }else{
      q4 = 1.0f;
   }

   qw = q1;
   if ( qw < 1.0f )   /* q2 = 1.0f */
      qw = 1.0f;
   if ( qw < q3 )
      qw = q3;
   if ( qw < q4 )
      qw = q4;

   if ( 1.0f != qw )
   {
      qw      = 1.0f / qw;

      q[0]   = q1 * qw;
      q[1]   = 1.0f * qw;
      q[2]   = q3 * qw;
      q[3]   = q4 * qw;
   }
   else
   {
      q[0]   = q1;
      q[1]   = 1.0f;
      q[2]   = q3;
      q[3]   = q4;
   }
   return 0;
}



//////////////////////////////////////////////////////////////////////////////

#if defined(YABAUSE_VITAGL)
#ifdef VITA_DIAG_GPU_STAGES
#include <psp2/kernel/processmgr.h>
/* Diagnostic only (default OFF): forced glFinish completion-wait samples.
 * vitaGL ends/restarts scenes before waiting, adding traffic and losing overlap.
 * Empty boundaries can cost milliseconds; already-completed work is not timed.
 * NOT pure GPU execution times, utilization, or a normal FPS benchmark. */
enum { VITA_GPU_STAGES = 8 };
static const char *const vita_gpu_stage_names[VITA_GPU_STAGES] =
  {"pre_rot", "rotation", "vdp1", "compose", "blit", "swap"};
static uint64_t vita_gpu_stage_us[VITA_GPU_STAGES];
static unsigned vita_gpu_stage_calls[VITA_GPU_STAGES], vita_gpu_stage_frames;
void YglVitaGpuStage(int stage) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  glFinish();
  vita_gpu_stage_us[stage] += sceKernelGetProcessTimeWide() - t0;
  ++vita_gpu_stage_calls[stage];
  if (stage == 5 && (++vita_gpu_stage_frames & 63) == 0) {
    char line[512]; int n = snprintf(line, sizeof(line), "gpu_stage_us frames=%u", vita_gpu_stage_frames);
    for (int i = 0; i < VITA_GPU_STAGES; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %s=%llu/%u", vita_gpu_stage_names[i],
                    (unsigned long long)vita_gpu_stage_us[i], vita_gpu_stage_calls[i]);
    YuiMsg("%s", line);
    memset(vita_gpu_stage_us, 0, sizeof(vita_gpu_stage_us));
    memset(vita_gpu_stage_calls, 0, sizeof(vita_gpu_stage_calls));
  }
}
#define VITA_GPU_STAGE(n) YglVitaGpuStage(n)
#else
#define VITA_GPU_STAGE(n) ((void)0)
#endif
#ifdef VITA_DIAG_ABLATE
/* Diagnostic only: skip GPU passes named by ux0:data/yabause-vita/ablate.txt. */
unsigned vt_ablate; /* also read by rotation_gpu.inc */
static void YglAblateLoad(void) {
  FILE *f = fopen("ux0:data/yabause-vita/ablate.txt", "r");
  if (f) { if (fscanf(f, "%u", &vt_ablate) != 1) vt_ablate = 0; fclose(f); }
  YuiMsg("diag_ablate mask=%u", vt_ablate);
}
#define ABL(bit) (vt_ablate & (bit))
#else
#define ABL(bit) 0
#endif
#include "atlas_vita.inc"
#include "feedback_vita.inc"
#else
YglTextureManager * YglTMInit(unsigned int w, unsigned int h) {

  GLuint error;
  YglTextureManager * tm;
  tm = (YglTextureManager *)malloc(sizeof(YglTextureManager));
  memset(tm, 0, sizeof(YglTextureManager));
  tm->width = w;
  tm->height = h;
  tm->current = 0;

  YglTMReset(tm);

  for (int i = 0; i < NUM_TEXTURE_BUFFER; i++) {

    glGenBuffers(1, &tm->pixelBufferID_in[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, tm->pixelBufferID_in[i]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, tm->width * tm->height * 4, NULL, GL_DYNAMIC_DRAW);
    YglUnbindPixelUnpack();

    glGetError();
    glGenTextures(1, &tm->textureID_in[i]);
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tm->width, tm->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to init YglTM->textureID %04X", error);
      abort();
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, tm->pixelBufferID_in[i]);
    tm->texture_in[i] = (unsigned int *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, tm->width * tm->height * 4, GL_MAP_WRITE_BIT );
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to init YglTM->texture %04X", error);
      abort();
    }
   
  }
  tm->texture = tm->texture_in[tm->current];
  YglUnbindPixelUnpack();
  YglGetColorRamPointer();

  return tm;
}

//////////////////////////////////////////////////////////////////////////////

void YglTMDeInit(YglTextureManager * tm) {

  for (int i = 0; i < NUM_TEXTURE_BUFFER; i++) {
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[i]);
    YglUnbindPixelUnpack();
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();

    glDeleteTextures(1, &tm->textureID_in[i]);
    tm->textureID_in[i] = 0;
    glDeleteBuffers(1, &tm->pixelBufferID_in[i]);
    tm->pixelBufferID_in[i] = 0;
  }

  free(tm);
}

//////////////////////////////////////////////////////////////////////////////

void YglTMReset(YglTextureManager * tm  ) {
  tm->currentX = 0;
  tm->currentY = 0;
  tm->yMax = 0;
}

#if 0
void YglTMReserve(YglTextureManager * tm, unsigned int w, unsigned int h){

  if (tm->width < w){
    YGLDEBUG("can't allocate texture: %dx%d\n", w, h);
    YglTMRealloc(tm, w, tm->height);
  }
  if ((tm->height - tm->currentY) < h) {
    YGLDEBUG("can't allocate texture: %dx%d\n", w, h);
    YglTMRealloc(tm, tm->width, tm->height + (h * 2));
    return;
  }
}
#endif

void YglTmPush(YglTextureManager * tm){
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tm->textureID_in[tm->current] );
  if (tm->texture != NULL ) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, tm->pixelBufferID_in[tm->current] );
    #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tm->width, tm->yMax, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    YglUnbindPixelUnpack();
    tm->texture = NULL;
  }
}

void YglTmPull(YglTextureManager * tm, u32 flg){
  if (tm->texture == NULL) {

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[tm->current]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, tm->pixelBufferID_in[tm->current]);

    if (flg) {
      tm->texture_in[tm->current] = (int*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, tm->width * tm->height * 4, GL_MAP_WRITE_BIT /*| GL_MAP_INVALIDATE_BUFFER_BIT*/ );
    }
    else {
      tm->texture_in[tm->current] = (int*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, tm->width * tm->height * 4, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    }
    if (tm->texture_in[tm->current] == NULL) {
      abort();
    }
    YglUnbindPixelUnpack();
/*    
    if (flg == 0) {
      if (tm->current == 0) {
        tm->current = 1;
      }
      else {
        tm->current = 0;
      }
    }
*/    
    tm->texture = tm->texture_in[tm->current];
  }
}


void YglTMRealloc(YglTextureManager * tm, unsigned int width, unsigned int height ){

  GLuint new_textureID[2];
  GLuint new_pixelBufferID[2];
  unsigned int * new_texture[2];
  GLuint error;

  Vdp2RgbTextureSync();

  if (tm->texture_in[tm->current] != NULL) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[tm->current] );
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, tm->pixelBufferID_in[tm->current]);
    #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
    tm->texture_in[tm->current] = NULL;
  }

  glGetError();

  for (int i = 0; i < NUM_TEXTURE_BUFFER; i++) {
    glGenTextures(1, &new_textureID[i]);
    glBindTexture(GL_TEXTURE_2D, new_textureID[i]);
    YglUnbindPixelUnpack();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR) {
      YGLDEBUG("Fail to init new_textureID %d, %04X(%d,%d)\n", new_textureID, error, width, height);
      abort();
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


    glGenBuffers(1, &new_pixelBufferID[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, new_pixelBufferID[i]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, width * height * 4, NULL, GL_DYNAMIC_DRAW);

    int dh = tm->height;
    if (dh > height) dh = height;

    glBindBuffer(GL_COPY_READ_BUFFER, tm->pixelBufferID_in[tm->current]);
    glBindBuffer(GL_COPY_WRITE_BUFFER, new_pixelBufferID[i]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, tm->width * dh * 4);
    if ((error = glGetError()) != GL_NO_ERROR) {
      YGLDEBUG("Fail to init new_texture %04X", error);
      abort();
    }
  }

  glBindBuffer(GL_COPY_READ_BUFFER, 0);
  glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

  for (int i = 0; i < NUM_TEXTURE_BUFFER; i++) {

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, new_pixelBufferID[i]);
    new_texture[i] = (unsigned int *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, width * height * 4, GL_MAP_WRITE_BIT );
    if ((error = glGetError()) != GL_NO_ERROR) {
      YGLDEBUG("Fail to init new_texture %04X", error);
      abort();
    }

    // Free textures
    glDeleteTextures(1, &tm->textureID_in[i]);
    glDeleteBuffers(1, &tm->pixelBufferID_in[i]);
    
    tm->texture_in[i] = new_texture[i];
    tm->textureID_in[i] = new_textureID[i];
    tm->pixelBufferID_in[i] = new_pixelBufferID[i];

  }

  // user new texture
  tm->width = width;
  tm->height = height;
  tm->texture = tm->texture_in[tm->current];
  //tm->textureID = new_textureID;
  //tm->pixelBufferID = new_pixelBufferID;

  return;

}

//////////////////////////////////////////////////////////////////////////////
#endif /* YABAUSE_VITAGL texture staging */
void YglTMAllocate(YglTextureManager * tm, YglTexture * output, unsigned int w, unsigned int h, unsigned int * x, unsigned int * y) {
  if( tm->width < w ){
    YGLDEBUG("can't allocate texture: %dx%d\n", w, h);
    YglTMRealloc( tm, w, tm->height);
    YglTMAllocate(tm, output, w, h, x, y);
    return;
  }
  if ((tm->height - tm->currentY) < h) {
    YGLDEBUG("can't allocate texture: %dx%d\n", w, h);
    YglTMRealloc( tm, tm->width, tm->height+(h*2));
    YglTMAllocate(tm, output, w, h, x, y);
    return;
  }

  if ((tm->width - tm->currentX) >= w) {
    *x = tm->currentX;
    *y = tm->currentY;
    output->w = tm->width - w;
    output->textdata = tm->texture + tm->currentY * tm->width + tm->currentX;
    tm->currentX += w;
#ifdef VITA_ATLAS_PRIVATE_TRIM
    YglAtlasExtentAdd(&tm->vita_cpu_extent,*x,*y,w,h);
#endif

    if ((tm->currentY + h) > tm->yMax){
      tm->yMax = tm->currentY + h;
    }
   } else {
     tm->currentX = 0;
     tm->currentY = tm->yMax;
     YglTMAllocate(tm, output, w, h, x, y);
   }
}

//////////////////////////////////////////////////////////////////////////////
int YglDumpFrameBuffer(const char * filename, int width, int height, char * buf ){

  FILE * fp = fopen(filename, "wb");
  int bsize = width*height * 3;
  char * pBitmap = malloc(bsize);
  int i, j;

  for (i = 0; i < height; i++){
    for (j = 0; j < width; j++){
      pBitmap[j * 3 + i*width * 3 + 0] = buf[j * 4 + i*width * 4 + 0];
      pBitmap[j * 3 + i*width * 3 + 1] = buf[j * 4 + i*width * 4 + 1];
      pBitmap[j * 3 + i*width * 3 + 2] = buf[j * 4 + i*width * 4 + 2];
    }
  }

  //-----------------------------------------
  //  File Header
  //------------------------------------------

  long offset = 14 + 40;
  char s[2];
  s[0] = 'B';
  s[1] = 'M';
  fwrite(s, sizeof(char), 2, fp);
  long filesize = bsize + offset;
  fwrite(&filesize, sizeof(long), 1, fp);
  short reserved = 0;
  fwrite(&reserved, sizeof(short), 1, fp);
  fwrite(&reserved, sizeof(short), 1, fp);
  fwrite(&offset, sizeof(long), 1, fp);


  //------------------------------------------
  // Bitmap Header
  //------------------------------------------
  long var_long;
  short var_short;

  var_long = 40;
  fwrite(&var_long, sizeof(long), 1, fp);
  var_long = width;

  fwrite(&var_long, sizeof(long), 1, fp);
  var_long = -height;

  fwrite(&var_long, sizeof(long), 1, fp);

  var_short = 1;
  fwrite(&var_short, sizeof(short), 1, fp);

  var_short = 24;
  fwrite(&var_short, sizeof(short), 1, fp);

  var_long = 0;
  fwrite(&var_long, sizeof(long), 1, fp);

  var_long = bsize;
  fwrite(&var_long, sizeof(long), 1, fp);


  var_long = 3780;
  fwrite(&var_long, sizeof(long), 1, fp);


  var_long = 3780;
  fwrite(&var_long, sizeof(long), 1, fp);
  var_long = 0;

  fwrite(&var_long, sizeof(long), 1, fp);
  var_long = 0;
  fwrite(&var_long, sizeof(long), 1, fp);

  //
  fwrite(pBitmap, sizeof(char), bsize, fp);

  fclose(fp);
  free(pBitmap);

  return 0;
}

void VIDOGLVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val ) {

  switch (type)
  {
  case 0:
    T1WriteByte(Vdp1FrameBuffer[_Ygl->drawframe], addr, val);
    break;
  case 1:
    T1WriteWord(Vdp1FrameBuffer[_Ygl->drawframe], addr, val);
    break;
  case 2:
    T1WriteLong(Vdp1FrameBuffer[_Ygl->drawframe], addr, val);
    break;
  default:
    break;
  }


  int tvmode = (Vdp1Regs->TVMR & 0x7);
  switch (tvmode) {
    case 0: // 16bit 512x256
    case 2: // 16bit 512x256
    case 4: // 16bit 512x256
    {
      u32 y = (addr >> 10) & 0xFF;
      u32 x = (addr & 0x3FF) >> 1;
      if (x >=_Ygl->rwidth || y >= _Ygl->rheight) {
        return;
      }
      u32 texaddr = _Ygl->rwidth*(_Ygl->rheight - y - 1) + x;

      switch (type)
      {
      case 0:
        LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
        break;
      case 1:
        if (val & 0x8000) {
          _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(0, 0, 0, 0, VDP1COLOR16TO24(val));
        }
        else {
          spritepixelinfo_struct spi = { 0 };
          Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, (u16 *)&val, &spi);
          _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, val);
        }
        break;
      case 2: {
        u16 color = (u16)((val >> 16) & 0xFFFF); 
        if (color & 0x8000) {
          _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(0, 0, 0, 0, VDP1COLOR16TO24(color));
        }
        else {
          spritepixelinfo_struct spi = { 0 };
          Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, &color, &spi);
          _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, color);
        }
        color = (u16)(val & 0xFFFF);
        if (color & 0x8000) {
          _Ygl->CpuWriteFrameBuffer[texaddr+1] = VDP1COLOR(0, 0, 0, 0, VDP1COLOR16TO24((color)));
        }
        else {
          spritepixelinfo_struct spi = { 0 };
          Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, &color, &spi);
          _Ygl->CpuWriteFrameBuffer[texaddr+1] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, color);
        }
        break;
      }
      default:
        break;
      }
      break;
    }
    case 1: { // 8bit 1024x256
      u32 y = (addr >> 10) & 0xFF;
      u32 x = (addr & 0x3FF) >> 1;
      if (x >= _Ygl->rwidth || y >= _Ygl->rheight) {
        return;
      }
      u32 texaddr = _Ygl->rwidth*(_Ygl->rheight - y - 1) + x;
      switch (type)
      {
      case 0:
        LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
        break;
      case 1:
        _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(1, 0, 0, 0, (val>>8) & 0xFF);
        _Ygl->CpuWriteFrameBuffer[texaddr + 1] = VDP1COLOR(1, 0, 0, 0, val&0xFF);
        break;
      case 2:
        LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
        break;
      }

      break;
    }
    case 3: { // 8bit 512x512
      u32 y = (addr >> 9) & 0x1FF;
      u32 x = addr & 0x1FF;
      if (x > _Ygl->rwidth || y >= _Ygl->rheight) {
        return;
      }
      u32 texaddr = _Ygl->rwidth*(_Ygl->rheight - y - 1) + x;
      switch (type)
      {
      case 0:
        LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
        break;
      case 1:
        _Ygl->CpuWriteFrameBuffer[texaddr] = VDP1COLOR(1, 0, 0, 0, (val>>8)&0xFF);
        _Ygl->CpuWriteFrameBuffer[texaddr + 1] = VDP1COLOR(1, 0, 0, 0, val&0xFF);
        break;
      case 2:
        LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
        break;
      }
      break;
    }
    defalut:
    break;
  }

  if (_Ygl->cpu_framebuffer_write[_Ygl->drawframe] == 0) {
    FRAMELOG("VIDOGLVdp1WriteFrameBuffer: CPU write framebuffer %d:1\n", _Ygl->drawframe);
  }
  _Ygl->cpu_framebuffer_write[_Ygl->drawframe]++;
}

#include "vdp1_attachments.h"
#ifdef VITA_VDP2_OPAQUE_RUNS
extern int YglDrawOpaqueRuns(YglProgram *,const float *);
extern void YglOpaqueDeInit(void);
#endif
#ifdef VITA_VDP2_INDEXED_QUADS
#include "quad_indices_vita.inc"
#endif
void YglDrawCpuFramebufferWrite(int target) {
  if (_Ygl->cpu_framebuffer_write[target] == 0) return;


  u32 drawFboId;
  spritepixelinfo_struct spi = { 0 };
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFboId);
  FRAMELOG("YglDrawCpuFramebufferWrite: write %d:%d\n", target, _Ygl->cpu_framebuffer_write[target]);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[target],0);
  YglVdp1AttachDepth(_Ygl->rboid_depth,_Ygl->rboid_stencil,0);
  _Ygl->cpu_framebuffer_write[0] = 0;
  _Ygl->cpu_framebuffer_write[1] = 0;

  if (_Ygl->smallfbotex == 0) {
    GLuint error;
    glGenTextures(1, &_Ygl->smallfbotex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _Ygl->smallfbotex);
    YglUnbindPixelUnpack();
    glGetError();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _Ygl->rwidth, _Ygl->rheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGetError();
    if ((error = glGetError()) != GL_NO_ERROR) {
      YGLDEBUG("Fail on YglDrawCpuFramebufferWrite at %d %04X %d %d", __LINE__, error, _Ygl->rwidth, _Ygl->rheight);
      abort();
    }
  }
  if (_Ygl->smallfbotex != 0) {
    glBindTexture(GL_TEXTURE_2D, _Ygl->smallfbotex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _Ygl->rwidth, _Ygl->rheight, GL_RGBA, GL_UNSIGNED_BYTE, _Ygl->CpuWriteFrameBuffer);
    glDisable(GL_SCISSOR_TEST);
    int params[4];
    glGetIntegerv(GL_VIEWPORT, params);
    glViewport(0, 0, _Ygl->width, _Ygl->height);
    YglWindowFramebuffer(_Ygl->smallfbotex, _Ygl->vdp1fbo, _Ygl->rwidth, _Ygl->rheight, _Ygl->rwidth, _Ygl->rheight);
    _Ygl->bWriteCpuFrameBuffer = 1;
    glViewport(params[0], params[1], params[2], params[3]);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, drawFboId);
}



void VIDOGLVdp1ReadFrameBuffer(u32 type, u32 addr, void * out) {
  u32 x = 0;
  u32 y = 0;
  int tvmode = (Vdp1Regs->TVMR & 0x7);
  switch( tvmode ) {
    case 0: // 16bit 512x256
    case 2: // 16bit 512x256
    case 4: // 16bit 512x256
      y = (addr >> 10)&0x1FF;
      x = (addr & 0x3FF) >> 1;
      break;
    case 1: // 8bit 1024x256
      y = (addr >> 10)&0x3FF;
      x = addr & 0x3FF;
      break;
    case 3: // 8bit 512x512
      y = (addr >> 9)&0x1FF;
      x = addr & 0x1FF;
      break;
    defalut: 
      y = 0;
      x = 0;
      break;
  }

  const int Line = y;
  const int Pix = x;
  if (_Ygl->cpu_framebuffer_write[_Ygl->drawframe] || (Pix >= Vdp1Regs->systemclipX2 || Line >= Vdp1Regs->systemclipY2)){
    switch (type)
    {
    case 0:
      *(u8*)out = T1ReadByte(Vdp1FrameBuffer[_Ygl->drawframe], addr);
      break;
    case 1:
      *(u16*)out = T1ReadWord(Vdp1FrameBuffer[_Ygl->drawframe], addr);
      break;
    case 2:
      *(u32*)out = T1ReadLong(Vdp1FrameBuffer[_Ygl->drawframe], addr);
      break;
    default:
      break;
    }
    return;
  }


  if (_Ygl->smallfbo == 0) {
      GLuint error;
      YabThreadLock(_Ygl->mutex);
      glGenTextures(1, &_Ygl->smallfbotex);
      YGLDEBUG("glGenTextures %d\n", _Ygl->smallfbotex);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, _Ygl->smallfbotex);
      YglUnbindPixelUnpack();
      #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
      glGetError();
      char * buf = malloc(_Ygl->rwidth * _Ygl->rheight * 4);
      memset(buf, 0, _Ygl->rwidth * _Ygl->rheight * 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _Ygl->rwidth, _Ygl->rheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
      free(buf);
      if ((error = glGetError()) != GL_NO_ERROR) {
        YGLDEBUG("Fail on VIDOGLVdp1ReadFrameBuffer at %d %04X %d %d", __LINE__, error, _Ygl->rwidth, _Ygl->rheight);
        abort();
      }
      YGLDEBUG("glTexImage2D %d\n", _Ygl->smallfbotex);
      glGenFramebuffers(1, &_Ygl->smallfbo);
      YGLDEBUG("glGenFramebuffers %d\n", _Ygl->smallfbo);
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->smallfbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _Ygl->smallfbotex, 0);
      int status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE) {
        YGLLOG("YglRenderVDP1: Framebuffer status = %08X\n", status);
        abort();
      }
      else {
        //YGLLOG("Framebuffer status OK = %08X\n", status );
      }

#ifndef YABAUSE_VITAGL
      glGenBuffers(1, &_Ygl->vdp1pixelBufferID);
      if ((error = glGetError()) != GL_NO_ERROR) {
        YGLDEBUG("Fail on VIDOGLVdp1ReadFrameBuffer at %d %04X", __LINE__, error);
        abort();
      }
      YGLDEBUG("glGenBuffers %d\n", _Ygl->vdp1pixelBufferID);
      if (_Ygl->vdp1pixelBufferID == 0) {
        YGLLOG("Fail to glGenBuffers %X", glGetError());
        abort();
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, _Ygl->vdp1pixelBufferID);
      glBufferData(GL_PIXEL_PACK_BUFFER, _Ygl->rwidth*_Ygl->rheight * 4, NULL, GL_STATIC_READ);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
      YabThreadUnLock(_Ygl->mutex);
  }


  while (_Ygl->vpd1_running){ YabThreadYield(); }

  YabThreadLock(_Ygl->mutex);
  if (_Ygl->pFrameBuffer == NULL){
    FrameProfileAdd("ReadFrameBuffer start");
    FRAMELOG("READ FRAME");
    if (_Ygl->sync != 0){
#ifdef YABAUSE_VITAGL
      /* Guest CPU observation is the completion boundary, not every draw. */
      glFinish();
#else
      glWaitSync(_Ygl->sync, 0, GL_TIMEOUT_IGNORED);
      glDeleteSync( _Ygl->sync );
#endif
      _Ygl->sync = 0;
    }


#if 0
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
    YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[_Ygl->drawframe],0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _Ygl->vdp1fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _Ygl->smallfbo);
    glBlitFramebuffer(0, 0, GlWidth, GlHeight, 0, 0, _Ygl->rwidth, _Ygl->rheight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
#else
    int params[4];
    glGetIntegerv(GL_VIEWPORT, params);
    glViewport(0, 0, _Ygl->rwidth, _Ygl->rheight);
    glScissor(0, 0, _Ygl->rwidth, _Ygl->rheight);
    glDisable(GL_SCISSOR_TEST);
    YglBlitFramebuffer(_Ygl->vdp1FrameBuff[_Ygl->drawframe], _Ygl->smallfbo, (float)_Ygl->rwidth / (float)_Ygl->width, (float)_Ygl->rheight / (float)_Ygl->height);
#endif
    YGLLOG("VIDOGLVdp1ReadFrameBuffer %d %08X\n", _Ygl->drawframe, addr);
    FrameProfileAdd("ReadFrameBuffer unlock");
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->smallfbo);
#ifdef YABAUSE_VITAGL
    _Ygl->pFrameBuffer = malloc((size_t)_Ygl->rwidth * _Ygl->rheight * 4);
    if (!_Ygl->pFrameBuffer) abort();
    glReadPixels(0, 0, _Ygl->rwidth, _Ygl->rheight, GL_RGBA,
                 GL_UNSIGNED_BYTE, _Ygl->pFrameBuffer);
    if (glGetError() != GL_NO_ERROR) {
      fprintf(stderr, "Vita VDP1 framebuffer readback failed\n");
      abort();
    }
#else
    glBindBuffer(GL_PIXEL_PACK_BUFFER, _Ygl->vdp1pixelBufferID);
    glReadPixels(0, 0, _Ygl->rwidth, _Ygl->rheight, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    _Ygl->pFrameBuffer = (unsigned int *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, _Ygl->rwidth * (_Ygl->rheight)* 4, GL_MAP_READ_BIT);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER,_Ygl->default_fbo);
    glViewport(params[0], params[1], params[2], params[3]);

    if (_Ygl->pFrameBuffer==NULL){
      switch (type) {
      case 1:
        *(u16*)out = 0x0000;
        break;
      case 2:
        *(u32*)out = 0x00000000;
        break;
      }
      YabThreadUnLock(_Ygl->mutex);
      return;
    }
    FrameProfileAdd("ReadFrameBuffer end");
  }

  int index;
  if( _Ygl->rwidth >= 640 ){
    index = (_Ygl->rheight-1-Line) *(_Ygl->rwidth * 4) + (Pix<<1) * 4;  
  }else{
    index = (_Ygl->rheight-1-Line) *(_Ygl->rwidth * 4) + Pix * 4;  
  }
 
  // 16bit mode
  if ((Vdp2Regs->SPCTL & 0xF) < 8) {
    // ToDo: index color mode
    switch (type) {
    case 1: {
      u8 r = *((u8*)(_Ygl->pFrameBuffer) + index);
      u16 g = *((u8*)(_Ygl->pFrameBuffer) + index + 1);
      u8 b = *((u8*)(_Ygl->pFrameBuffer) + index + 2);
      u16 a = *((u8*)(_Ygl->pFrameBuffer) + index + 3);
      if( (a&0x40) == 0 ){
        *(u16*)out = ((r >> 3) & 0x1f) | (((g >> 3) & 0x1f) << 5) | (((b >> 3) & 0x1F) << 10) | 0x8000;
      }else{
        u8 sptype = Vdp2Regs->SPCTL & 0x0F;
        switch(sptype){
        case 0:
          *(u16*)out = ((a<<(5+8))&0xE000) | (((a>>3)&0x03)<<11) | (((g<<8)|r)&0x7FF);
          break;
        case 1:
          *(u16*)out = ((a<<(5+8))&0xE000) | (((a>>3)&0x03)<<11) | (((g<<8)|r)&0x7FF);
          break;
        default:
          *(u16*)out = 0;
          LOG("VIDOGLVdp1ReadFrameBuffer sprite type %d is not supported",sptype);
          break;
        }
      }
    }
    break;
    case 2: {
      u32 r = *((u8*)(_Ygl->pFrameBuffer) + index);
      u32 g = *((u8*)(_Ygl->pFrameBuffer) + index + 1);
      u32 b = *((u8*)(_Ygl->pFrameBuffer) + index + 2);
      u32 r2 = *((u8*)(_Ygl->pFrameBuffer) + index + 4);
      u32 g2 = *((u8*)(_Ygl->pFrameBuffer) + index + 5);
      u32 b2 = *((u8*)(_Ygl->pFrameBuffer) + index + 6);
      /*  BBBBBGGGGGRRRRR */
      *(u32*)out = (((r2 >> 3) & 0x1f) | (((g2 >> 3) & 0x1f) << 5) | (((b2 >> 3) & 0x1F) << 10) | 0x8000) |
        ((((r >> 3) & 0x1f) | (((g >> 3) & 0x1f) << 5) | (((b >> 3) & 0x1F) << 10) | 0x8000) << 16);
    }
            break;
    }
  }
  // 8bitmode
  else {
      u16 r = *((u8*)(_Ygl->pFrameBuffer) + index);
      u16 r2 = *((u8*)(_Ygl->pFrameBuffer) + index + 4);
      *(u16*)out = (r<<8) | (r2<<0);
  }
  YabThreadUnLock(_Ygl->mutex);
}

//////////////////////////////////////////////////////////////////////////////
int YglGenFrameBuffer() {
  int status;
  GLuint error;

  if (rebuild_frame_buffer == 0){
    return 0;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  glFinish();
  glGetError();

  if (_Ygl->vdp1FrameBuff[0] == 0) {
    glGenTextures(2, _Ygl->vdp1FrameBuff);
  }
  glGetError();
  glBindTexture(GL_TEXTURE_2D, _Ygl->vdp1FrameBuff[0]);
  if ((error = glGetError()) != GL_NO_ERROR) {
    YGLDEBUG("Fail to YglGLInit at %d %04X %d %d", __LINE__, error, GlWidth, GlHeight);
    abort();
  }

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _Ygl->width, _Ygl->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  if ((error = glGetError()) != GL_NO_ERROR) {
    YGLDEBUG("Fail to YglGLInit at %d %04X %d %d", __LINE__, error, GlWidth, GlHeight);
    abort();
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindTexture(GL_TEXTURE_2D, _Ygl->vdp1FrameBuff[1]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _Ygl->width, _Ygl->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  if ((error = glGetError()) != GL_NO_ERROR) {
    YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
    abort();
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  #ifdef YABAUSE_VITAGL
    free(_Ygl->pFrameBuffer);
#endif
    _Ygl->pFrameBuffer = NULL;

   if(1) //strstr((const char*)glGetString(GL_EXTENSIONS),"packed_depth_stencil") != NULL )
  {
    if (_Ygl->rboid_depth != 0) glDeleteRenderbuffers(1, &_Ygl->rboid_depth);
    glGenRenderbuffers(1, &_Ygl->rboid_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->rboid_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, _Ygl->width, _Ygl->height);
    _Ygl->rboid_stencil = _Ygl->rboid_depth;
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
      abort();
    }
  }
  else{
    if (_Ygl->rboid_depth != 0) glDeleteRenderbuffers(1, &_Ygl->rboid_depth);
    glGenRenderbuffers(1, &_Ygl->rboid_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->rboid_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, _Ygl->width, _Ygl->height);

    if (_Ygl->rboid_stencil != 0) glDeleteRenderbuffers(1, &_Ygl->rboid_stencil);
    glGenRenderbuffers(1, &_Ygl->rboid_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->rboid_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, _Ygl->width, _Ygl->height);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
      abort();
    }
  }

  if (_Ygl->vdp1fbo != 0)
    glDeleteFramebuffers(1, &_Ygl->vdp1fbo);

  glGenFramebuffers(1, &_Ygl->vdp1fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[0],1);
  YglVdp1AttachDepth(_Ygl->rboid_depth,_Ygl->rboid_stencil,1);
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    YGLDEBUG("YglGLInit:Framebuffer status = %08X w=%d h=%d fbo=%d, tex=%d, depth=%d, stencil=%d\n", 
    status,_Ygl->width, _Ygl->height,
    _Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[0],
    _Ygl->rboid_depth,_Ygl->rboid_stencil);
    abort();
  }
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[1],0);
  YglVdp1AttachDepth(_Ygl->rboid_depth,_Ygl->rboid_stencil,0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    YGLDEBUG("YglGLInit:Framebuffer status = %08X\n", status);
    abort();
  }

  if (YglIsNeedFrameBuffer()==1){
    YglGenerateAABuffer();
  }

  YGLDEBUG("YglGLInit OK");
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
	_Ygl->targetfbo = 0;
  glBindTexture(GL_TEXTURE_2D, 0);
  rebuild_frame_buffer = 0;

  int base_texture_width = 512;
  switch (_Ygl->rbg_resolution_mode) {
  case RBG_RES_ORIGINAL:
    base_texture_width = 512;
    break;
  case RBG_RES_2x:
    base_texture_width = 1024;
    break;
  case RBG_RES_720P:
    base_texture_width = 1280;
    break;
  case RBG_RES_1080P:
    base_texture_width = 1920;
    break;
  case RBG_RES_FIT_TO_EMULATION:
    base_texture_width = GlWidth;
    break;
  default:
    break;
  }

  return 0;
}

//////////////////////////////////////////////////////////////////////////////
int YglIsNeedFrameBuffer() {
  if (_Ygl->aamode == AA_FXAA) {
    return 1;
  }
  if (_Ygl->aamode == AA_SCANLINE_FILTER && _Ygl->rheight <= 256 ) {
    return 1;
  }
  if (_Ygl->resolution_mode != RES_NATIVE) {
    return 1;
  }
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
int YglGLInit(int width, int height) {

  YGLDEBUG("YglGLInit(%d,%d)\n", width, height);
   rebuild_frame_buffer = 1;

   return 0;
}

//////////////////////////////////////////////////////////////////////////////
int YglGenerateAABuffer(){

  int status;
  GLuint error;

  YGLDEBUG("YglGenerateAABuffer: %d,%d", _Ygl->width, _Ygl->height);

  int width = _Ygl->width;
  int height = _Ygl->height;

  //--------------------------------------------------------------------------------
  // FXAA
  if (_Ygl->fxaa_fbotex != 0) {
    glDeleteTextures(1,&_Ygl->fxaa_fbotex);
  }
  glGenTextures(1, &_Ygl->fxaa_fbotex);
  glGetError();
  glBindTexture(GL_TEXTURE_2D, _Ygl->fxaa_fbotex);
  if ((error = glGetError()) != GL_NO_ERROR) {
    YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
    abort();
  }

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  if ((error = glGetError()) != GL_NO_ERROR) {
    YGLDEBUG("Fail to YglGLInit at %d %04X %d %d", __LINE__, error, width, height);
    abort();
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if ( 1) //strstr(glGetString(GL_EXTENSIONS), "packed_depth_stencil") != NULL)
  {
    if (_Ygl->fxaa_depth != 0) glDeleteRenderbuffers(1, &_Ygl->fxaa_depth);
    glGenRenderbuffers(1, &_Ygl->fxaa_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->fxaa_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    _Ygl->fxaa_stencil = _Ygl->fxaa_depth;
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
      abort();
    }
  }else{
    if (_Ygl->fxaa_depth != 0) glDeleteRenderbuffers(1, &_Ygl->fxaa_depth);
    glGenRenderbuffers(1, &_Ygl->fxaa_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->fxaa_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);

    if (_Ygl->fxaa_stencil != 0) glDeleteRenderbuffers(1, &_Ygl->fxaa_stencil);
    glGenRenderbuffers(1, &_Ygl->fxaa_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, _Ygl->fxaa_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLDEBUG("Fail to YglGLInit at %d %04X", __LINE__, error);
      abort();
    }
  }

  if (_Ygl->fxaa_fbo != 0){
    glDeleteFramebuffers(1, &_Ygl->fxaa_fbo);
  }

  glGenFramebuffers(1, &_Ygl->fxaa_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->fxaa_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _Ygl->fxaa_fbotex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _Ygl->fxaa_depth);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _Ygl->fxaa_stencil);
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    YGLDEBUG("YglGLInit:Framebuffer status = %08X\n", status);
    abort();
  }
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
int YglScreenInit(int r, int g, int b, int d) {
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
void YuiSetVideoAttribute(int type, int val){
  return;
}

//////////////////////////////////////////////////////////////////////////////
int YglInit(int width, int height, unsigned int depth) {
#ifdef VITA_DIAG_ABLATE
  YglAblateLoad();
#endif
  unsigned int i,j;
  void * dataPointer=NULL;
  YGLLOG("YglInit(%d,%d,%d);",width,height,depth );

  if ((_Ygl = (Ygl *)malloc(sizeof(Ygl))) == NULL) {
    return -1;
  }

  memset(_Ygl,0,sizeof(Ygl));

  _Ygl->depth = depth;
  _Ygl->rwidth = 320;
  _Ygl->rheight = 240;
  _Ygl->density = 1;

  _Ygl->CpuWriteFrameBuffer = (u32*)malloc(_Ygl->rwidth * _Ygl->rheight * 4);
  if (!_Ygl->CpuWriteFrameBuffer) return -1;
  memset(_Ygl->CpuWriteFrameBuffer, 0xFF, _Ygl->rwidth * _Ygl->rheight * 4);

  if ((_Ygl->levels = (YglLevel *)malloc(sizeof(YglLevel) * (depth + 1))) == NULL){
    return -1;
  }

  memset(_Ygl->levels,0,sizeof(YglLevel) * (depth+1) );
  for(i = 0;i < (depth+1) ;i++) {
    _Ygl->levels[i].prgcurrent = 0;
    _Ygl->levels[i].uclipcurrent = 0;
    _Ygl->levels[i].prgcount = 1;
    _Ygl->levels[i].prg = (YglProgram*)malloc(sizeof(YglProgram)*_Ygl->levels[i].prgcount);
    if (_Ygl->levels[i].prg == NULL){ 
      return -1; 
    }
    memset(_Ygl->levels[i].prg, 0, sizeof(YglProgram)*_Ygl->levels[i].prgcount);
    for(j = 0;j < _Ygl->levels[i].prgcount; j++) {
      _Ygl->levels[i].prg[j].prg=0;
      _Ygl->levels[i].prg[j].currentQuad = 0;
      _Ygl->levels[i].prg[j].maxQuad = 12 * 2000;
      if ((_Ygl->levels[i].prg[j].quads = (float *)malloc(_Ygl->levels[i].prg[j].maxQuad * sizeof(float))) == NULL){ return -1; }
      if ((_Ygl->levels[i].prg[j].textcoords = (float *)malloc(_Ygl->levels[i].prg[j].maxQuad * sizeof(float) * 2)) == NULL){ return -1; }
      if ((_Ygl->levels[i].prg[j].vertexAttribute = (float *)malloc(_Ygl->levels[i].prg[j].maxQuad * sizeof(float) * 2)) == NULL){ return -1; }
    }
  }

  if( _Ygl->mutex == NULL){
    _Ygl->mutex = YabThreadCreateMutex();
  }

  if (_Ygl->crammutex == NULL) {
    _Ygl->crammutex = YabThreadCreateMutex();
  }



#if defined(_USEGLEW_)
  glewInit();
#endif

#if defined(__ANDROID__) && !defined(__LIBRETRO__)
  glPatchParameteri = (PFNGLPATCHPARAMETERIPROC)eglGetProcAddress("glPatchParameteri");
  //glMemoryBarrier = (PFNGLPATCHPARAMETERIPROC)eglGetProcAddress("glMemoryBarrier");
#endif

  glGetError();

#ifdef __LIBRETRO__
  _Ygl->default_fbo = YuiGetFB();
#else
  _Ygl->default_fbo = 0;
#endif
  _Ygl->drawframe = 0;
  _Ygl->readframe = 1;

#if !defined(__LIBRETRO__)
  // This line is causing a black screen on the libretro port
  glGetIntegerv(GL_FRAMEBUFFER_BINDING,&_Ygl->default_fbo);
  printf("GL_FRAMEBUFFER_BINDING = %d",_Ygl->default_fbo );
#endif

  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  YglLoadIdentity(&_Ygl->mtxModelView);
  YglOrtho(&_Ygl->mtxModelView, 0.0f, 320.0f, 224.0f, 0.0f, 10.0f, 0.0f);

  YglLoadIdentity(&_Ygl->mtxTexture);
  YglOrtho(&_Ygl->mtxTexture, -width, width, -height, height, 1.0f, 0.0f);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glDisable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
//  glClearDepthf(0.0f);

  glCullFace(GL_FRONT_AND_BACK);
  glDisable(GL_CULL_FACE);
#ifndef YABAUSE_VITAGL
  glDisable(GL_DITHER);
#endif

  glGetError();

#ifndef YABAUSE_VITAGL
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#endif
  /* Vita uploads/readbacks here are tightly packed RGBA8. Pinned vitaGL only
   * exposes UNPACK_ROW_LENGTH, not the desktop byte-alignment selectors. */

  YglTM = YglTMInit(512, 512);

  _Ygl->smallfbo = 0;
  _Ygl->smallfbotex = 0;
  _Ygl->tmpfbo = 0;
  _Ygl->tmpfbotex = 0;

  YglGLInit(width, height);

  if (YglProgramInit() != 0) {
    YGLDEBUG("Fail to YglProgramInit\n");
    abort();
  }
  
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo );
  glBindTexture(GL_TEXTURE_2D, 0);
  _Ygl->st = 0;
  _Ygl->msglength = 0;
  _Ygl->aamode = AA_NONE;
  //_Ygl->aamode = AA_FXAA;
  //_Ygl->aamode = AA_SCANLINE_FILTER;

  return 0;
}

//////////////////////////////////////////////////////////////////////////////
void YglDeInit(void) {
   unsigned int i,j;
#ifdef VITA_VDP2_OPAQUE_RUNS
   YglOpaqueDeInit();
#endif

   YglTMDeInit(YglTM);
//   YglTMDeInit(YglTM_vdp1);

   if (_Ygl)
   {
      if(_Ygl->mutex) YabThreadFreeMutex(_Ygl->mutex );
      
      if (_Ygl->levels)
      {
         for (i = 0; i < (_Ygl->depth+1); i++)
         {
         for (j = 0; j < _Ygl->levels[i].prgcount; j++)
         {
            if (_Ygl->levels[i].prg[j].quads)
            free(_Ygl->levels[i].prg[j].quads);
            if (_Ygl->levels[i].prg[j].textcoords)
            free(_Ygl->levels[i].prg[j].textcoords);
            if (_Ygl->levels[i].prg[j].vertexAttribute)
            free(_Ygl->levels[i].prg[j].vertexAttribute);
         }
         free(_Ygl->levels[i].prg);
         }
         free(_Ygl->levels);
      }

#ifdef YABAUSE_VITAGL
      free(_Ygl->lincolor_buf);
      free(_Ygl->backcolor_buf);
      free(_Ygl->cram_tex_buf);
      free(_Ygl->pFrameBuffer);
      glDeleteFramebuffers(1, &_Ygl->vita_feedback_fbo);
      glDeleteTextures(1, &_Ygl->vita_feedback_tex);
      glDeleteTextures(1, &_Ygl->lincolor_tex);
      glDeleteTextures(1, &_Ygl->back_tex);
      glDeleteTextures(1, &_Ygl->cram_tex);
#ifdef VITA_VDP2_INDEXED_QUADS
      glDeleteBuffers(1, &vita_quad_index_buffer);
      vita_quad_index_buffer = 0;
#endif
      for (i = 0; i < enBGMAX; ++i) {
        free(_Ygl->bg[i].lincolor_buf);
        glDeleteTextures(1, &_Ygl->bg[i].lincolor_tex);
      }
#endif
      free(_Ygl);
      _Ygl = NULL;
   }

}


//////////////////////////////////////////////////////////////////////////////

#include "geometry_buffer.inc"

YglProgram * YglGetProgram( YglSprite * input, int prg, unsigned needed )
{
   YglLevel   *level;
   YglProgram *program;
   float checkval;

   if (input->priority > 8) {
      VDP1LOG("sprite with priority %d\n", input->priority);
      return NULL;
   }

   level = &_Ygl->levels[input->priority];

   level->blendmode |= (input->blendmode&0x03);
   if( input->uclipmode != level->uclipcurrent ||
     (input->uclipmode !=0 && 
    (level->ux1 != Vdp1Regs->userclipX1 || level->uy1 != Vdp1Regs->userclipY1 ||
    level->ux2 != Vdp1Regs->userclipX2 || level->uy2 != Vdp1Regs->userclipY2) )
     )
   {
      if( input->uclipmode == 0x02 || input->uclipmode == 0x03 )
      {
         if (YglProgramChange(level,PG_VFP1_STARTUSERCLIP) != 0) abort();
         program = &level->prg[level->prgcurrent];
         program->uClipMode = input->uclipmode;
         program->ux1=Vdp1Regs->userclipX1;
         program->uy1=Vdp1Regs->userclipY1;
         program->ux2=Vdp1Regs->userclipX2;
         program->uy2=Vdp1Regs->userclipY2;
         level->ux1=Vdp1Regs->userclipX1;
         level->uy1=Vdp1Regs->userclipY1;
         level->ux2=Vdp1Regs->userclipX2;
         level->uy2=Vdp1Regs->userclipY2;
      }else{
         if (YglProgramChange(level,PG_VFP1_ENDUSERCLIP) != 0) abort();
         program = &level->prg[level->prgcurrent];
         program->uClipMode = input->uclipmode;
      }
      level->uclipcurrent = input->uclipmode;

   }

   checkval = (float)(input->cor) / 255.0f;
   if (checkval != level->prg[level->prgcurrent].color_offset_val[0])
   {
     if (YglProgramChange(level, prg) != 0) abort();
     level->prg[level->prgcurrent].id = input->id;
     level->prg[level->prgcurrent].blendmode = input->blendmode;

   } else if( level->prg[level->prgcurrent].prgid != prg ) {
      if (YglProgramChange(level,prg) != 0) abort();
    level->prg[level->prgcurrent].id = input->id;
    level->prg[level->prgcurrent].blendmode = input->blendmode;
   }
   else if (level->prg[level->prgcurrent].blendmode != input->blendmode){
     if (YglProgramChange(level, prg) != 0) abort();
     level->prg[level->prgcurrent].id = input->id;
     level->prg[level->prgcurrent].blendmode = input->blendmode;
   }
   else if (input->id != level->prg[level->prgcurrent].id ){
     if (YglProgramChange(level, prg) != 0) abort();
     level->prg[level->prgcurrent].id = input->id;
     level->prg[level->prgcurrent].blendmode = input->blendmode;
   }
// for polygon debug
  //else if (prg == PG_VFP1_GOURAUDSAHDING ){
  //   YglProgramChange(level, prg);
  //}
#ifdef YABAUSE_VITAGL
   /* Preserve command boundaries for reads of previously drawn pixels. */
   if ((prg == PG_VFP1_HALFTRANS || prg == PG_VFP1_SHADOW ||
        prg == PG_VFP1_GOURAUDSAHDING_HALFTRANS) &&
       level->prg[level->prgcurrent].currentQuad != 0) {
     if (YglProgramChange(level, prg) != 0) abort();
     level->prg[level->prgcurrent].id = input->id;
     level->prg[level->prgcurrent].blendmode = input->blendmode;
   }
#endif
   program = &level->prg[level->prgcurrent];

   if (YglReserveGeometry(program, needed) != 0) {
     fprintf(stderr, "Cannot reserve draw geometry: used=%d needed=%u capacity=%d\n",
             program->currentQuad, needed, program->maxQuad);
     abort();
   }
   program->interuput_texture = 0;
   return program;
}



//////////////////////////////////////////////////////////////////////////////

int YglCheckTriangle( const float * point ){
  if ((point[2 * 0 + 0] == point[2 * 1 + 0]) && (point[2 * 0 + 1] == point[2 * 1 + 1])) {
    return 1;
  }
  else if ((point[2 * 1 + 0] == point[2 * 2 + 0]) && (point[2 * 1 + 1] == point[2 * 2 + 1]))  {
    return 1;
  }
  else if ((point[2 * 2 + 0] == point[2 * 3 + 0]) && (point[2 * 2 + 1] == point[2 * 3 + 1]))  {
    return 1;
  }
  else if ((point[2 * 3 + 0] == point[2 * 0 + 0]) && (point[2 * 3 + 1] == point[2 * 0 + 1])) {
    return 1;
  }
  return 0;
}

static int YglQuadGrowShading_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg);
static int YglTriangleGrowShading_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg);
static int YglQuadGrowShading_tesselation_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg);

void YglCacheQuadGrowShading(YglSprite * input, float * colors, YglCache * cache){

  if (_Ygl->polygonmode == GPU_TESSERATION) {
    YglTesserationProgramInit();
    YglQuadGrowShading_tesselation_in(input, NULL, colors, cache, 0);
  }
  else if (_Ygl->polygonmode == CPU_TESSERATION) {
    YglTriangleGrowShading_in(input, NULL, colors, cache, 0);
  }
  else if (_Ygl->polygonmode == PERSPECTIVE_CORRECTION) {
    if (YglCheckTriangle(input->vertices)){
      VT_STACK_BEGIN(VT_VDP1_RASTER);
      YglTriangleGrowShading_in(input, NULL, colors, cache, 0);
      VT_STACK_END();
    }
    else{
      YglQuadGrowShading_in(input, NULL, colors, cache, 0);
    }
  }

}

int YglQuadGrowShading(YglSprite * input, YglTexture * output, float * colors, YglCache * c){

  if (_Ygl->polygonmode == GPU_TESSERATION) {
    YglTesserationProgramInit();
    return YglQuadGrowShading_tesselation_in(input, output, colors, c, 1);
  }
  else if (_Ygl->polygonmode == CPU_TESSERATION) {
    return YglTriangleGrowShading_in(input, output, colors, c, 1);
  }
  else if (_Ygl->polygonmode == PERSPECTIVE_CORRECTION) {
    if (YglCheckTriangle(input->vertices)){
      VT_STACK_BEGIN(VT_VDP1_RASTER);
      const int r = YglTriangleGrowShading_in(input, output, colors, c, 1);
      VT_STACK_END();
      return r;
    }
    return YglQuadGrowShading_in(input, output, colors, c, 1);
  }
  return 0;
}


int YglTriangleGrowShading(YglSprite * input, YglTexture * output, float * colors, YglCache * c) {
  return YglTriangleGrowShading_in(input, output, colors, c, 1);
}

void YglCacheTriangleGrowShading(YglSprite * input, float * colors, YglCache * cache) {
  YglTriangleGrowShading_in(input, NULL, colors, cache, 0);
}

int YglTriangleGrowShading_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg ) {
  unsigned int x, y;
  YglProgram *program;
  int prg = PG_VFP1_GOURAUDSAHDING;
  float * pos;
  int u, v;

  // Select Program
  if ((input->blendmode & 0x03) == VDP2_CC_ADD)
  {
    prg = PG_VDP2_ADDBLEND;
  }
  else if (input->blendmode == VDP1_COLOR_CL_GROW_HALF_TRANSPARENT)
  {
    prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS;
  }
  else if (input->blendmode == VDP1_COLOR_CL_HALF_LUMINANCE)
  {
    prg = PG_VFP1_HALF_LUMINANCE;
  }
  else if (input->blendmode == VDP1_COLOR_CL_MESH)
  {
    prg = PG_VFP1_MESH;
  }
  else if (input->blendmode == VDP1_COLOR_CL_SHADOW){
    prg = PG_VFP1_SHADOW;
  }
  else if (input->blendmode == VDP1_COLOR_SPD){
    prg = PG_VFP1_GOURAUDSAHDING_SPD;
  }

  if (input->linescreen == 1){
    prg = PG_LINECOLOR_INSERT;
    if (((Vdp2Regs->CCCTL >> 9) & 0x01)){
      prg = PG_LINECOLOR_INSERT_DESTALPHA;
    }
  }
  else if (input->linescreen == 2){ // per line operation by HBLANK
    prg = PG_VDP2_PER_LINE_ALPHA;
  }

  program = YglGetProgram(input, prg, YGL_MAX_NEED_BUFFER);
  if (program == NULL || program->quads == NULL) return -1;

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;


  pos = program->quads + program->currentQuad;
  float * colv = (program->vertexAttribute + (program->currentQuad * 2));
  texturecoordinate_struct texv[6];
  texturecoordinate_struct * tpos = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));

  if (output != NULL){
    YglTMAllocate(_Ygl->texture_manager, output, input->w, input->h, &x, &y);
  }
  else{
    x = c->x;
    y = c->y;
  }

  texv[0].r = texv[1].r = texv[2].r = texv[3].r = texv[4].r = texv[5].r = 0; // these can stay at 0
  texv[0].q = texv[1].q = texv[2].q = texv[3].q = texv[4].q = texv[5].q = 1.0f; // these can stay at 0

  if (input->flip & 0x1) {
    texv[0].s = texv[3].s = texv[5].s = (float)((x + input->w) - ATLAS_BIAS);
    texv[1].s = texv[2].s = texv[4].s = (float)((x)+ATLAS_BIAS);
  }
  else {
    texv[0].s = texv[3].s = texv[5].s = (float)((x)+ATLAS_BIAS);
    texv[1].s = texv[2].s = texv[4].s = (float)((x + input->w) - ATLAS_BIAS);
  }
  if (input->flip & 0x2) {
    texv[0].t = texv[1].t = texv[3].t = (float)((y + input->h) - ATLAS_BIAS);
    texv[2].t = texv[4].t = texv[5].t = (float)((y)+ATLAS_BIAS);
  }
  else {
    texv[0].t = texv[1].t = texv[3].t = (float)((y)+ATLAS_BIAS);
    texv[2].t = texv[4].t = texv[5].t = (float)((y + input->h) - ATLAS_BIAS);
  }
  
  if (c != NULL && cash_flg == 1)
  {
    switch (input->flip) {
    case 0:
      c->x = texv[0].s; //  *(program->textcoords + ((program->currentQuad + 12 - 12) * 2));
      c->y = texv[0].t; // *(program->textcoords + ((program->currentQuad + 12 - 12) * 2) + 1);
      break;
    case 1:
      c->x = texv[1].s; // *(program->textcoords + ((program->currentQuad + 12 - 10) * 2));
      c->y = texv[0].t; // (program->textcoords + ((program->currentQuad + 12 - 10) * 2) + 1);
      break;
    case 2:
      c->x = texv[0].s; //*(program->textcoords + ((program->currentQuad + 12 - 2) * 2));
      c->y = texv[2].t; // *(program->textcoords + ((program->currentQuad + 12 - 2) * 2) + 1);
      break;
    case 3:
      c->x = texv[1].s; //  *(program->textcoords + ((program->currentQuad + 12 - 4) * 2));
      c->y = texv[2].t; //*(program->textcoords + ((program->currentQuad + 12 - 4) * 2) + 1);
      break;
    }
  }

  int tess_count = YGL_TESS_COUNT;
  float s_step = (float)(texv[2].s-texv[0].s)/(float)tess_count;
  float t_step = (float)(texv[2].t-texv[0].t)/(float)tess_count;

  float vec_ad_x = input->vertices[6] - input->vertices[0];
  float vec_ad_y = input->vertices[7] - input->vertices[1];
  float vec_ad_xs = vec_ad_x / tess_count;
  float vec_ad_ys = vec_ad_y / tess_count;

  float vec_bc_x = input->vertices[4] - input->vertices[2];
  float vec_bc_y = input->vertices[5] - input->vertices[3];
  float vec_bc_xs = vec_bc_x / tess_count;
  float vec_bc_ys = vec_bc_y / tess_count;

  for (v = 0; v < tess_count ; v++){

    // Top Line for current row
    float ax = input->vertices[0] + vec_ad_xs * v;
    float ay = input->vertices[1] + vec_ad_ys * v;
    float bx = input->vertices[2] + vec_bc_xs * v;
    float by = input->vertices[3] + vec_bc_ys * v;
    float ab_step_x = (bx - ax) / tess_count;
    float ab_step_y = (by - ay) / tess_count;

    // botton Line for current row
    float cx = input->vertices[2] + vec_bc_xs * (v + 1);
    float cy = input->vertices[3] + vec_bc_ys * (v + 1);
    float dx = input->vertices[0] + vec_ad_xs * (v + 1);
    float dy = input->vertices[1] + vec_ad_ys * (v + 1);

    float dc_step_x = (cx - dx) / tess_count;
    float dc_step_y = (cy - dy) / tess_count;

    for (u = 0; u < tess_count ; u++){

      float * cpos = &pos[12*(u + tess_count*v) ];
      texturecoordinate_struct * ctpos = &tpos[6 * (u + tess_count*v)];
      float * vtxa = &colv[24 * (u + tess_count*v)];

      /*
        A+--+B
         |  |
        D+--+C
      */
      float dax = ax + ab_step_x * u;
      float day = ay + ab_step_y * u;
      float dbx = dax + ab_step_x;
      float dby = day + ab_step_y;
      float ddx = dx + dc_step_x * u;
      float ddy = dy + dc_step_y * u;
      float dcx = ddx + dc_step_x;
      float dcy = ddy + dc_step_y;

      cpos[0] = dax;
      cpos[1] = day;
      cpos[2] = dbx;
      cpos[3] = dby;
      cpos[4] = dcx;
      cpos[5] = dcy;

      cpos[6] = dax;
      cpos[7] = day;
      cpos[8] = dcx;
      cpos[9] = dcy;
      cpos[10] = ddx;
      cpos[11] = ddy;

      ctpos[0].s = texv[0].s + s_step * u;
      ctpos[0].t = texv[0].t + t_step * v;
      ctpos[1].s = ctpos[0].s + s_step;
      ctpos[1].t = ctpos[0].t;
      ctpos[2].s = ctpos[0].s + s_step;
      ctpos[2].t = ctpos[0].t + t_step;

      ctpos[3].s = ctpos[0].s;
      ctpos[3].t = ctpos[0].t;
      ctpos[4].s = ctpos[2].s;
      ctpos[4].t = ctpos[2].t;
      ctpos[5].s = ctpos[0].s;
      ctpos[5].t = ctpos[0].t + t_step;
      ctpos[0].r = ctpos[1].r = ctpos[2].r = ctpos[3].r = ctpos[4].r = ctpos[5].r = 0; // these can stay at 0
      ctpos[0].q = ctpos[1].q = ctpos[2].q = ctpos[3].q = ctpos[4].q = ctpos[5].q = 1.0f; // these can stay at 0

      // ToDo: color interpolation
      if (colors == NULL) {
        memset(vtxa, 0, sizeof(float) * 24);
      }
      else {

        int uindex = u;
        int vindex = v;
        vtxa[0] = (colors[0] * (tess_count - uindex) + colors[4] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[12] * (tess_count - u) + colors[8] * uindex) / (float)tess_count * vindex;
        vtxa[1] = (colors[1] * (tess_count - uindex) + colors[5] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[13] * (tess_count - u) + colors[9] * uindex) / (float)tess_count * vindex;
        vtxa[2] = (colors[2] * (tess_count - uindex) + colors[6] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[14] * (tess_count - u) + colors[10] * uindex) / (float)tess_count * vindex;
        vtxa[3] = (colors[3] * (tess_count - uindex) + colors[7] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[15] * (tess_count - u) + colors[11] * uindex) / (float)tess_count * vindex;
        vtxa[0] /= (float)tess_count;
        vtxa[1] /= (float)tess_count;
        vtxa[2] /= (float)tess_count;
        vtxa[3] /= (float)tess_count;

        uindex = u + 1;
        vindex = v;
        vtxa[4] = (colors[0] * (tess_count - uindex) + colors[4] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[12] * (tess_count - u) + colors[8] * uindex) / (float)tess_count * vindex;
        vtxa[5] = (colors[1] * (tess_count - uindex) + colors[5] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[13] * (tess_count - u) + colors[9] * uindex) / (float)tess_count * vindex;
        vtxa[6] = (colors[2] * (tess_count - uindex) + colors[6] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[14] * (tess_count - u) + colors[10] * uindex) / (float)tess_count * vindex;
        vtxa[7] = (colors[3] * (tess_count - uindex) + colors[7] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[15] * (tess_count - u) + colors[11] * uindex) / (float)tess_count * vindex;
        vtxa[4] /= (float)tess_count;
        vtxa[5] /= (float)tess_count;
        vtxa[6] /= (float)tess_count;
        vtxa[7] /= (float)tess_count;

        uindex = u + 1;
        vindex = v + 1;
        vtxa[8] = (colors[0] * (tess_count - uindex) + colors[4] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[12] * (tess_count - u) + colors[8] * uindex) / (float)tess_count * vindex;
        vtxa[9] = (colors[1] * (tess_count - uindex) + colors[5] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[13] * (tess_count - u) + colors[9] * uindex) / (float)tess_count * vindex;
        vtxa[10] = (colors[2] * (tess_count - uindex) + colors[6] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[14] * (tess_count - u) + colors[10] * uindex) / (float)tess_count * vindex;
        vtxa[11] = (colors[3] * (tess_count - uindex) + colors[7] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[15] * (tess_count - u) + colors[11] * uindex) / (float)tess_count * vindex;
        vtxa[8] /= (float)tess_count;
        vtxa[9] /= (float)tess_count;
        vtxa[10] /= (float)tess_count;
        vtxa[11] /= (float)tess_count;

        vtxa[12] = vtxa[0];
        vtxa[13] = vtxa[1];
        vtxa[14] = vtxa[2];
        vtxa[15] = vtxa[3];

        vtxa[16] = vtxa[8];
        vtxa[17] = vtxa[9];
        vtxa[18] = vtxa[10];
        vtxa[19] = vtxa[11];

        uindex = u;
        vindex = v + 1;
        vtxa[20] = (colors[0] * (tess_count - uindex) + colors[4] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[12] * (tess_count - u) + colors[8] * uindex) / (float)tess_count * vindex;
        vtxa[21] = (colors[1] * (tess_count - uindex) + colors[5] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[13] * (tess_count - u) + colors[9] * uindex) / (float)tess_count * vindex;
        vtxa[22] = (colors[2] * (tess_count - uindex) + colors[6] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[14] * (tess_count - u) + colors[10] * uindex) / (float)tess_count * vindex;
        vtxa[23] = (colors[3] * (tess_count - uindex) + colors[7] * uindex) / (float)tess_count * (tess_count - vindex) + (colors[15] * (tess_count - u) + colors[11] * uindex) / (float)tess_count * vindex;
        vtxa[20] /= (float)tess_count;
        vtxa[21] /= (float)tess_count;
        vtxa[22] /= (float)tess_count;
        vtxa[23] /= (float)tess_count;
      }

    }
  }
  program->currentQuad = program->currentQuad + (12*tess_count*tess_count);
  return 0;
}

int YglQuadGrowShading_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg) {
   unsigned int x, y;
   YglProgram *program;
   texturecoordinate_struct *tmp;
   float * vtxa;
   float q[4];
   int prg = PG_VFP1_GOURAUDSAHDING;
   float * pos;


   if ((input->blendmode & 0x03) == VDP2_CC_ADD)
   {
      prg = PG_VDP2_ADDBLEND;
   }
   else if (input->blendmode == VDP1_COLOR_CL_GROW_HALF_TRANSPARENT)
   {
      prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS;
   }
   else if (input->blendmode == VDP1_COLOR_CL_HALF_LUMINANCE) {
      prg = PG_VFP1_HALF_LUMINANCE;
   }
   else if (input->blendmode == VDP1_COLOR_CL_MESH)
   {
     prg = PG_VFP1_MESH;
   }
   else if (input->blendmode == VDP1_COLOR_CL_SHADOW){
     prg = PG_VFP1_SHADOW;
   }
   else if (input->blendmode == VDP1_COLOR_SPD){
     prg = PG_VFP1_GOURAUDSAHDING_SPD;
   }

   if (input->linescreen == 1){
     prg = PG_LINECOLOR_INSERT;
     if (((Vdp2Regs->CCCTL >> 9) & 0x01)){
       prg = PG_LINECOLOR_INSERT_DESTALPHA;
     }

   }
   else if (input->linescreen == 2){ // per line operation by HBLANK
     prg = PG_VDP2_PER_LINE_ALPHA;
   }



   program = YglGetProgram(input,prg, 12);
   if( program == NULL ) return -1;
   //YGLLOG( "program->quads = %X,%X,%d/%d\n",program->quads,program->vertexBuffer,program->currentQuad,program->maxQuad );
   if( program->quads == NULL ) {
       int a=0;
   }

   program->color_offset_val[0] = (float)(input->cor)/255.0f;
   program->color_offset_val[1] = (float)(input->cog)/255.0f;
   program->color_offset_val[2] = (float)(input->cob)/255.0f;
   program->color_offset_val[3] = 0;

   if (output != NULL){
     YglTMAllocate(_Ygl->texture_manager, output, input->w, input->h, &x, &y);
   }
   else{
     x = c->x;
     y = c->y;
   }

   // Vertex
   pos = program->quads + program->currentQuad;

   pos[0] = input->vertices[0];
   pos[1] = input->vertices[1];
   pos[2] = input->vertices[2];
   pos[3] = input->vertices[3];
   pos[4] = input->vertices[4];
   pos[5] = input->vertices[5];
   pos[6] = input->vertices[0];
   pos[7] = input->vertices[1];
   pos[8] = input->vertices[4];
   pos[9] = input->vertices[5];
   pos[10] = input->vertices[6];
   pos[11] = input->vertices[7];


   // Color
   vtxa = (program->vertexAttribute + (program->currentQuad * 2));
   if( colors == NULL ) {
      memset(vtxa,0,sizeof(float)*24);
   } else {
     vtxa[0] = colors[0];
     vtxa[1] = colors[1];
     vtxa[2] = colors[2];
     vtxa[3] = colors[3];

     vtxa[4] = colors[4];
     vtxa[5] = colors[5];
     vtxa[6] = colors[6];
     vtxa[7] = colors[7];

     vtxa[8] = colors[8];
     vtxa[9] = colors[9];
     vtxa[10] = colors[10];
     vtxa[11] = colors[11];

     vtxa[12] = colors[0];
     vtxa[13] = colors[1];
     vtxa[14] = colors[2];
     vtxa[15] = colors[3];

     vtxa[16] = colors[8];
     vtxa[17] = colors[9];
     vtxa[18] = colors[10];
     vtxa[19] = colors[11];

     vtxa[20] = colors[12];
     vtxa[21] = colors[13];
     vtxa[22] = colors[14];
     vtxa[23] = colors[15];
   }

   // texture
   tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));

   program->currentQuad += 12;

   tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0; // these can stay at 0
   if (input->flip & 0x1) {
     tmp[0].s = tmp[3].s = tmp[5].s = (float)((x + input->w) - ATLAS_BIAS) ;
     tmp[1].s = tmp[2].s = tmp[4].s = (float)((x)+ATLAS_BIAS) ;
   } else {
     tmp[0].s = tmp[3].s = tmp[5].s = (float)((x)+ATLAS_BIAS) ;
     tmp[1].s = tmp[2].s = tmp[4].s = (float)((x + input->w) - ATLAS_BIAS);
   }
   if (input->flip & 0x2) {
     tmp[0].t = tmp[1].t = tmp[3].t = (float)((y + input->h) - ATLAS_BIAS);
     tmp[2].t = tmp[4].t = tmp[5].t = (float)((y)+ATLAS_BIAS);
   } else {
     tmp[0].t = tmp[1].t = tmp[3].t = (float)((y)+ATLAS_BIAS);
     tmp[2].t = tmp[4].t = tmp[5].t = (float)((y + input->h) - ATLAS_BIAS);
   }

   if (c != NULL && cash_flg == 1)
   {
      switch(input->flip) {
        case 0:
          c->x = *(program->textcoords + ((program->currentQuad - 12) * 2));   // upper left coordinates(0)
          c->y = *(program->textcoords + ((program->currentQuad - 12) * 2)+1); // upper left coordinates(0)
          break;
        case 1:
          c->x = *(program->textcoords + ((program->currentQuad - 10) * 2));   // upper left coordinates(0)
          c->y = *(program->textcoords + ((program->currentQuad - 10) * 2)+1); // upper left coordinates(0)
          break;
       case 2:
          c->x = *(program->textcoords + ((program->currentQuad - 2) * 2));   // upper left coordinates(0)
          c->y = *(program->textcoords + ((program->currentQuad - 2) * 2)+1); // upper left coordinates(0)
          break;
       case 3:
          c->x = *(program->textcoords + ((program->currentQuad - 4) * 2));   // upper left coordinates(0)
          c->y = *(program->textcoords + ((program->currentQuad - 4) * 2)+1); // upper left coordinates(0)
          break;
      }
   }

   if( input->dst == 1 )
   {
      YglCalcTextureQ(input->vertices,q);

      tmp[0].s *= q[0];
      tmp[0].t *= q[0];
      tmp[1].s *= q[1];
      tmp[1].t *= q[1];
      tmp[2].s *= q[2];
      tmp[2].t *= q[2];
      tmp[3].s *= q[0];
      tmp[3].t *= q[0];
      tmp[4].s *= q[2];
      tmp[4].t *= q[2];
      tmp[5].s *= q[3];
      tmp[5].t *= q[3];

      tmp[0].q = q[0];
      tmp[1].q = q[1];
      tmp[2].q = q[2];
      tmp[3].q = q[0];
      tmp[4].q = q[2];
      tmp[5].q = q[3];
   }else{
      tmp[0].q = 1.0f;
      tmp[1].q = 1.0f;
      tmp[2].q = 1.0f;
      tmp[3].q = 1.0f;
      tmp[4].q = 1.0f;
      tmp[5].q = 1.0f;
   }

   return 0;
}


int YglQuadGrowShading_tesselation_in(YglSprite * input, YglTexture * output, float * colors, YglCache * c, int cash_flg) {
  unsigned int x, y;
  YglProgram *program;
  texturecoordinate_struct *tmp;
  float * vtxa;
  int prg = PG_VFP1_GOURAUDSAHDING_TESS;
  float * pos;

  if (input->blendmode == VDP1_COLOR_CL_GROW_HALF_TRANSPARENT)
  {
    prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS_TESS;
  }
  else if (input->blendmode == VDP1_COLOR_CL_MESH)
  {
    prg = PG_VFP1_MESH_TESS;
  }
  else if (input->blendmode == VDP1_COLOR_CL_SHADOW){
    prg = PG_VFP1_SHADOW_TESS;
  }
  else if (input->blendmode == VDP1_COLOR_SPD){
    prg = PG_VFP1_GOURAUDSAHDING_SPD_TESS;
  }

  program = YglGetProgram(input, prg, 8);
  if (program == NULL) return -1;
  //YGLLOG( "program->quads = %X,%X,%d/%d\n",program->quads,program->vertexBuffer,program->currentQuad,program->maxQuad );
  if (program->quads == NULL) {
    int a = 0;
  }

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0.0;

  if (output != NULL){
    YglTMAllocate(_Ygl->texture_manager, output, input->w, input->h, &x, &y);
  }
  else{
    x = c->x;
    y = c->y;
  }

  // Vertex
  pos = program->quads + program->currentQuad;

  pos[0] = input->vertices[0];
  pos[1] = input->vertices[1];
  pos[2] = input->vertices[2];
  pos[3] = input->vertices[3];
  pos[4] = input->vertices[4];
  pos[5] = input->vertices[5];
  pos[6] = input->vertices[6];
  pos[7] = input->vertices[7];


  // Color
  vtxa = (program->vertexAttribute + (program->currentQuad * 2));
  if (colors == NULL) {
    memset(vtxa, 0, sizeof(float) * 24);
  }
  else {
    vtxa[0] = colors[0];
    vtxa[1] = colors[1];
    vtxa[2] = colors[2];
    vtxa[3] = colors[3];

    vtxa[4] = colors[4];
    vtxa[5] = colors[5];
    vtxa[6] = colors[6];
    vtxa[7] = colors[7];

    vtxa[8] = colors[8];
    vtxa[9] = colors[9];
    vtxa[10] = colors[10];
    vtxa[11] = colors[11];

    vtxa[12] = colors[12];
    vtxa[13] = colors[13];
    vtxa[14] = colors[14];
    vtxa[15] = colors[15];
  }

  // texture
  tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));

  program->currentQuad += 8;

  tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = 0.0f; // these can stay at 0.0
  tmp[0].q = tmp[1].q = tmp[2].q = tmp[3].q = 1.0f; // these can stay at 1.0

  if ( input->flip & 0x1) {
    tmp[0].s = tmp[3].s = (float)((x + input->w) - ATLAS_BIAS);
    tmp[1].s = tmp[2].s = (float)((x)+ATLAS_BIAS);
  }
  else {
    tmp[0].s = tmp[3].s = (float)((x)+ATLAS_BIAS);
    tmp[1].s = tmp[2].s = (float)((x + input->w) - ATLAS_BIAS);
  }
  if( input->flip & 0x2) {
    tmp[0].t = tmp[1].t = (float)((y + input->h) - ATLAS_BIAS);
    tmp[2].t = tmp[3].t = (float)((y)+ATLAS_BIAS);
  }
  else {
    tmp[0].t = tmp[1].t = (float)((y)+ATLAS_BIAS);
    tmp[2].t = tmp[3].t = (float)((y + input->h) - ATLAS_BIAS);
  }

  if (c != NULL && cash_flg == 1)
  {
    switch (input->flip) {
    case 0:
      c->x = tmp[0].s;
      c->y = tmp[0].t;
      break;
    case 1:
      c->x = tmp[1].s;
      c->y = tmp[0].t;
      break;
    case 2:
      c->x = tmp[0].s;
      c->y = tmp[2].t;
      break;
    case 3:
      c->x = tmp[1].s;
      c->y = tmp[2].t;
      break;
    }
  }


  return 0;
}

static void YglQuadOffset_in(vdp2draw_struct * input, YglTexture * output, YglCache * c, int cx, int cy, float sx, float sy, int cash_flg);

void YglQuadOffset(vdp2draw_struct * input, YglTexture * output, YglCache * c, int cx, int cy, float sx, float sy) {
  YglQuadOffset_in(input, output, c, cx, cy, sx, sy, 1);
}

void YglCachedQuadOffset(vdp2draw_struct * input, YglCache * cache, int cx, int cy, float sx, float sy) {
  YglQuadOffset_in(input, NULL, cache, cx, cy, sx, sy, 0);
}

#ifdef VITA_SKIP_TRANSPARENT_CELLS
YglProgram *ygl_last_offset_program; /* batch and program id of the last offset quad */
int ygl_last_offset_prg;
#endif
void YglQuadOffset_in(vdp2draw_struct * input, YglTexture * output, YglCache * c, int cx, int cy, float sx, float sy, int cash_flg) {
  unsigned int x, y;
  YglProgram *program;
  texturecoordinate_struct *tmp;
  int prg = PG_NORMAL;
  float * pos;
  //float * vtxa;

  int vHeight;
#ifdef VITA_SKIP_TRANSPARENT_CELLS
  ygl_last_offset_program = NULL;
#endif

  if (input->colornumber >= 3) {
    prg = PG_NORMAL;
    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC;
    }
    if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR;
    }
    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA;
      }
    }
    else if (input->linescreen == 2) { // per line operation by HBLANK
      prg = PG_VDP2_PER_LINE_ALPHA;
    }
  }
  else {

    if( (input->blendmode&0x03) == VDP2_CC_ADD)
      prg = PG_VDP2_ADDCOLOR_CRAM;
    else {
      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY;
      }
      else {
        prg = PG_VDP2_NORMAL_CRAM;
      }
    }

    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC_CRAM;
    }
    if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR_CRAM;
    }
    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT_CRAM;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
      }
    }
    else if (input->linescreen == 2) { // per line operation by HBLANK
      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET; // Assault Leynos 2
      }
      else {
        prg = PG_VDP2_PER_LINE_ALPHA_CRAM;
      }
    }
  }
  
  program = YglGetProgram((YglSprite*)input, prg, 12);
  if (program == NULL) return;

  program->colornumber = input->colornumber;
  program->bwin0 = input->bEnWin0;
  program->logwin0 = input->WindowArea0;
  program->bwin1 = input->bEnWin1;
  program->logwin1 = input->WindowArea1;
  program->winmode = input->LogicWin;
  program->lineTexture = input->lineTexture;
  program->specialcolormode = input->specialcolormode;

  program->mosaic[0] = input->mosaicxmask;
  program->mosaic[1] = input->mosaicymask;

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;
  //info->cor

  vHeight = input->vertices[5] - input->vertices[1];

  pos = program->quads + program->currentQuad;
  pos[0] = (input->vertices[0] - cx) * sx;
  pos[1] = input->vertices[1] * sy;
  pos[2] = (input->vertices[2] - cx) * sx;
  pos[3] = input->vertices[3] * sy;
  pos[4] = (input->vertices[4] - cx) * sx;
  pos[5] = input->vertices[5] * sy;
  pos[6] = (input->vertices[0] - cx) * sx;
  pos[7] = (input->vertices[1]) * sy;
  pos[8] = (input->vertices[4] - cx)*sx;
  pos[9] = input->vertices[5] * sy;
  pos[10] = (input->vertices[6] - cx) * sx;
  pos[11] = input->vertices[7] * sy;

  // vtxa = (program->vertexAttribute + (program->currentQuad * 2));
  // memset(vtxa,0,sizeof(float)*24);

  tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));

  program->currentQuad += 12;
#ifdef VITA_SKIP_TRANSPARENT_CELLS
  ygl_last_offset_program = program;
  ygl_last_offset_prg = prg;
#endif
  if (output != NULL){
    YglTMAllocate(_Ygl->texture_manager, output, input->cellw, input->cellh, &x, &y);
  }
  else{
    x = c->x;
    y = c->y;
  }

  tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0; // these can stay at 0

  /*
  0 +---+ 1
  |   |
  +---+ 2
  3 +---+
  |   |
  5 +---+ 4
  */

  if (input->flipfunction & 0x1) {
    tmp[0].s = tmp[3].s = tmp[5].s = (float)(x + input->cellw) - ATLAS_BIAS;
    tmp[1].s = tmp[2].s = tmp[4].s = (float)(x)+ATLAS_BIAS;
  }
  else {
    tmp[0].s = tmp[3].s = tmp[5].s = (float)(x)+ATLAS_BIAS;
    tmp[1].s = tmp[2].s = tmp[4].s = (float)(x + input->cellw) - ATLAS_BIAS;
  }
  if (input->flipfunction & 0x2) {
    tmp[0].t = tmp[1].t = tmp[3].t = (float)(y + input->cellh - cy) - ATLAS_BIAS;
    tmp[2].t = tmp[4].t = tmp[5].t = (float)(y + input->cellh - (cy + vHeight)) + ATLAS_BIAS;
  }
  else {
    tmp[0].t = tmp[1].t = tmp[3].t = (float)(y + cy) + ATLAS_BIAS;
    tmp[2].t = tmp[4].t = tmp[5].t = (float)(y + (cy + vHeight)) - ATLAS_BIAS;
  }

  if (c != NULL && cash_flg == 1)
  {
    c->x = x;
    c->y = y;
  }

  tmp[0].q = 1.0f;
  tmp[1].q = 1.0f;
  tmp[2].q = 1.0f;
  tmp[3].q = 1.0f;
  tmp[4].q = 1.0f;
  tmp[5].q = 1.0f;
}


static int YglQuad_in(vdp2draw_struct * input, YglTexture * output, YglCache * c, int cash_flg);

float * YglQuad(vdp2draw_struct * input, YglTexture * output, YglCache * c){
  YglQuad_in(input, output, c, 1);
  return 0;
}

void YglCachedQuad(vdp2draw_struct * input, YglCache * cache){
  YglQuad_in(input, NULL, cache, 0);
}

int YglQuad_in(vdp2draw_struct * input, YglTexture * output, YglCache * c, int cash_flg) {
  unsigned int x, y;
  YglProgram *program;
  texturecoordinate_struct *tmp;
  int prg = PG_NORMAL;
  float * pos;
  //float * vtxa;

  if (input->colornumber >= 3) {
      prg = PG_NORMAL;
      if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
        prg = PG_VDP2_MOSAIC;
      }
      if ((input->blendmode & VDP2_CC_BLUR) != 0) {
        prg = PG_VDP2_BLUR;
      }
      if (input->linescreen == 1) {
        prg = PG_LINECOLOR_INSERT;
        if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
          prg = PG_LINECOLOR_INSERT_DESTALPHA;
        }
      }
      else if (input->linescreen == 2) { // per line operation by HBLANK
        prg = PG_VDP2_PER_LINE_ALPHA;
      }
  } else {
    if ((input->blendmode & 0x03) == VDP2_CC_ADD)
      prg = PG_VDP2_ADDCOLOR_CRAM;
    else {
      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY;
      }
      else {
        prg = PG_VDP2_NORMAL_CRAM;
      }
    }

      if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
        prg = PG_VDP2_MOSAIC_CRAM;
      }
      if (((input->blendmode & VDP2_CC_BLUR) != 0)) {
        prg = PG_VDP2_BLUR_CRAM;
      }
      if (input->linescreen == 1) {
        prg = PG_LINECOLOR_INSERT_CRAM;
        if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
          prg = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
        }
      }
      else if (input->linescreen == 2) { // per line operation by HBLANK
        if (input->specialprimode == 2) {
          prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET; // Assault Leynos 2
        }
        else {
          prg = PG_VDP2_PER_LINE_ALPHA_CRAM;
        }

    }
  }

  program = YglGetProgram((YglSprite*)input, prg, 12);
  if (program == NULL) return -1;

  program->colornumber = input->colornumber;
  program->bwin0 = input->bEnWin0;
  program->logwin0 = input->WindowArea0;
  program->bwin1 = input->bEnWin1;
  program->logwin1 = input->WindowArea1;
  program->winmode = input->LogicWin;
  program->lineTexture = input->lineTexture;
  program->blendmode = input->blendmode;
  program->specialcolormode = input->specialcolormode;

  program->mosaic[0] = input->mosaicxmask;
  program->mosaic[1] = input->mosaicymask;

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;
  //info->cor

  pos = program->quads + program->currentQuad;
  pos[0] = input->vertices[0];
  pos[1] = input->vertices[1];
  pos[2] = input->vertices[2];
  pos[3] = input->vertices[3];
  pos[4] = input->vertices[4];
  pos[5] = input->vertices[5];
  pos[6] = input->vertices[0];
  pos[7] = input->vertices[1];
  pos[8] = input->vertices[4];
  pos[9] = input->vertices[5];
  pos[10] = input->vertices[6];
  pos[11] = input->vertices[7];

  // vtxa = (program->vertexAttribute + (program->currentQuad * 2));
  // memset(vtxa,0,sizeof(float)*24);

  tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));

  program->currentQuad += 12;

  if (output != NULL){
    YglTMAllocate(_Ygl->texture_manager, output, input->cellw, input->cellh, &x, &y);
  }
  else{
    x = c->x;
    y = c->y;
  }



  tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0; // these can stay at 0

  /*
  0 +---+ 1
  |   |
  +---+ 2
  3 +---+
  |   |
  5 +---+ 4
  */

  if (input->flipfunction & 0x1) {
    tmp[0].s = tmp[3].s = tmp[5].s = (float)(x + input->cellw) - ATLAS_BIAS;
    tmp[1].s = tmp[2].s = tmp[4].s = (float)(x)+ATLAS_BIAS;
  }
  else {
    tmp[0].s = tmp[3].s = tmp[5].s = (float)(x)+ATLAS_BIAS;
    tmp[1].s = tmp[2].s = tmp[4].s = (float)(x + input->cellw) - ATLAS_BIAS;
  }
  if (input->flipfunction & 0x2) {
    tmp[0].t = tmp[1].t = tmp[3].t = (float)(y + input->cellh) - ATLAS_BIAS;
    tmp[2].t = tmp[4].t = tmp[5].t = (float)(y)+ATLAS_BIAS;
  }
  else {
    tmp[0].t = tmp[1].t = tmp[3].t = (float)(y)+ATLAS_BIAS;
    tmp[2].t = tmp[4].t = tmp[5].t = (float)(y + input->cellh) - ATLAS_BIAS;
  }

  if (c != NULL && cash_flg == 1)
  {
    switch (input->flipfunction) {
    case 0:
      c->x = *(program->textcoords + ((program->currentQuad - 12) * 2));   // upper left coordinates(0)
      c->y = *(program->textcoords + ((program->currentQuad - 12) * 2) + 1); // upper left coordinates(0)
      break;
    case 1:
      c->x = *(program->textcoords + ((program->currentQuad - 10) * 2));   // upper left coordinates(0)
      c->y = *(program->textcoords + ((program->currentQuad - 10) * 2) + 1); // upper left coordinates(0)
      break;
    case 2:
      c->x = *(program->textcoords + ((program->currentQuad - 2) * 2));   // upper left coordinates(0)
      c->y = *(program->textcoords + ((program->currentQuad - 2) * 2) + 1); // upper left coordinates(0)
      break;
    case 3:
      c->x = *(program->textcoords + ((program->currentQuad - 4) * 2));   // upper left coordinates(0)
      c->y = *(program->textcoords + ((program->currentQuad - 4) * 2) + 1); // upper left coordinates(0)
      break;
    }
  }

  tmp[0].q = 1.0f;
  tmp[1].q = 1.0f;
  tmp[2].q = 1.0f;
  tmp[3].q = 1.0f;
  tmp[4].q = 1.0f;
  tmp[5].q = 1.0f;

  return 0;
}


int YglQuadRbg0(vdp2draw_struct * input, YglTexture * output, YglCache * c, YglCache * line, int rbg_type) {
  unsigned int x, y;
  YglProgram *program;
  texturecoordinate_struct *tmp;
  int prg = PG_NORMAL;
  float * pos;

  if(input->colornumber >= 3 ) {
    prg = PG_NORMAL;
    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC;
    }
    if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR;
    }
    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA;
      }
    }
    else if (input->linescreen == 2) { // per line operation by HBLANK
      prg = PG_VDP2_PER_LINE_ALPHA;
    }
  }
  else {

    if (line->x != -1 && VDP2_CC_NONE != input->blendmode) {
      prg = PG_VDP2_RBG_CRAM_LINE;
    }
    else if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC_CRAM;
    }
    else if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR_CRAM;
    }
    else if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT_CRAM;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
      }
    }
    else if (input->linescreen == 2) { // per line operation by HBLANK

      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET; // Assault Leynos 2
      }
      else {
        prg = PG_VDP2_PER_LINE_ALPHA_CRAM;
      }
    }
    else {
      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY;
      }else {
        prg = PG_VDP2_NORMAL_CRAM;
      }
    }
/*
    if (line->x != -1 && VDP2_CC_NONE != input->blendmode ) {
      prg = PG_VDP2_RBG_CRAM_LINE;
    }
    else {
      prg = PG_VDP2_NORMAL_CRAM;
    }
*/
  }

#ifdef VITA_ROTATION_TARGET
  /* Plain RBG programs only: they sample s_texture alone and their setup and
   * window redraw bind YglVitaRotationSource. Line/mosaic/blur/per-line keep
   * the atlas path. A unique batch id keeps this quad in its own batch
   * (YglGetProgram splits on id and resets interuput_texture for appends). */
  int rotation_source = 0;
  if (!_Ygl->rbg_use_compute_shader &&
      (prg == PG_NORMAL || prg == PG_VDP2_NORMAL_CRAM || prg == PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY))
    rotation_source = YglVitaRotationTarget(c->x, c->y, input->cellw, input->cellh);
  int saved_id = input->id;
  if (rotation_source) input->id = -0x524f5400 - rotation_source;
  program = YglGetProgram((YglSprite*)input, prg, 12);
  input->id = saved_id;
#else
  program = YglGetProgram((YglSprite*)input, prg, 12);
#endif
  if (program == NULL) return -1;
  
  program->colornumber = input->colornumber;
  program->blendmode = input->blendmode;
  program->bwin0 = input->bEnWin0;
  program->logwin0 = input->WindowArea0;
  program->bwin1 = input->bEnWin1;
  program->logwin1 = input->WindowArea1;
  program->winmode = input->LogicWin;
  program->lineTexture = input->lineTexture;
  program->specialcolormode = input->specialcolormode;

  program->mosaic[0] = input->mosaicxmask;
  program->mosaic[1] = input->mosaicymask;

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;

 
  //info->cor
  pos = program->quads + program->currentQuad;
  pos[0] = input->vertices[0];
  pos[1] = input->vertices[1];
  pos[2] = input->vertices[2];
  pos[3] = input->vertices[3];
  pos[4] = input->vertices[4];
  pos[5] = input->vertices[5];
  pos[6] = input->vertices[0];
  pos[7] = input->vertices[1];
  pos[8] = input->vertices[4];
  pos[9] = input->vertices[5];
  pos[10] = input->vertices[6];
  pos[11] = input->vertices[7];

   //vtxa = (program->vertexAttribute + (program->currentQuad * 2));
   //memset(vtxa,0,sizeof(float)*24);

  int line_height = 0;

  if (_Ygl->rbg_use_compute_shader) {
	  
	  if(rbg_type == 0 )
		program->interuput_texture = 1;
	  else
		program->interuput_texture = 2;

	  tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));
	  program->currentQuad += 12;
	  tmp[0].s = tmp[3].s = tmp[5].s = 0;
	  tmp[1].s = tmp[2].s = tmp[4].s = (float)(input->cellw);
	  tmp[0].t = tmp[1].t = tmp[3].t = 0;
	  tmp[2].t = tmp[4].t = tmp[5].t = (float)(input->cellh);
	  //tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0;
	  //tmp[0].q = tmp[1].q = tmp[2].q = tmp[3].q = tmp[4].q = tmp[5].q = 0;
    line_height = input->drawh;

  }
  else {

    line_height = input->drawh;

	  program->interuput_texture = 0;

	  tmp = (texturecoordinate_struct *)(program->textcoords + (program->currentQuad * 2));
	  program->currentQuad += 12;
	  x = c->x;
	  y = c->y;
#ifdef VITA_ROTATION_TARGET
	  if (rotation_source) {
	    /* Same texel offsets as the atlas quad, relative to the target. */
	    x = y = 0;
	    program->interuput_texture = rotation_source;
	  }
#endif

	  /*
	  0 +---+ 1
		  |   |
		  +---+ 2
	  3 +---+
		  |   |
	  5 +---+ 4
				*/

	  tmp[0].s = tmp[3].s = tmp[5].s = (float)(x)+ATLAS_BIAS;
	  tmp[1].s = tmp[2].s = tmp[4].s = (float)(x + input->cellw) - ATLAS_BIAS;
	  tmp[0].t = tmp[1].t = tmp[3].t = (float)(y)+ATLAS_BIAS;
	  tmp[2].t = tmp[4].t = tmp[5].t = (float)(y + input->cellh) - ATLAS_BIAS;
  }

  if (prg == PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET) {
    tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0;
    tmp[0].q = tmp[1].q = tmp[3].q = 0;
    tmp[2].q = tmp[4].q = tmp[5].q = line_height;
  }
  else {

    if (line == NULL) {
      tmp[0].r = tmp[1].r = tmp[2].r = tmp[3].r = tmp[4].r = tmp[5].r = 0;
      tmp[0].q = tmp[1].q = tmp[2].q = tmp[3].q = tmp[4].q = tmp[5].q = 0;
    }
    else {
      tmp[0].r = (float)(line->x) + ATLAS_BIAS;
      tmp[0].q = (float)(line->y) + ATLAS_BIAS;

      tmp[1].r = (float)(line->x) + ATLAS_BIAS;
      tmp[1].q = (float)(line->y + 1) - ATLAS_BIAS;

      tmp[2].r = (float)(line->x + input->cellh) - ATLAS_BIAS;
      tmp[2].q = (float)(line->y + 1) - ATLAS_BIAS;

      tmp[3].r = (float)(line->x) + ATLAS_BIAS;
      tmp[3].q = (float)(line->y) + ATLAS_BIAS;

      tmp[4].r = (float)(line->x + input->cellh) - ATLAS_BIAS;
      tmp[4].q = (float)(line->y + 1) - ATLAS_BIAS;

      tmp[5].r = (float)(line->x + input->cellh) - ATLAS_BIAS;
      tmp[5].q = (float)(line->y) + ATLAS_BIAS;
    }
  }
  
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
void YglEraseWriteVDP1(void) {

  u16 color;
  int priority;
  u32 alpha = 0;
  if (_Ygl->vdp1FrameBuff[0] == 0) return;

  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[_Ygl->readframe],0);
  YglVdp1AttachDepth(_Ygl->rboid_depth,_Ygl->rboid_stencil,0);

  color = Vdp1Regs->EWDR;
  priority = 0;

  if ((color & 0x8000) && (Vdp2Regs->SPCTL & 0x20)) {

    u8 rgb_alpha = 0xF8;
    int tp = 0;
    u8 spmode = Vdp2Regs->SPCTL & 0x0f;
    if (spmode & 0x8){
      if (!(color & 0xFF)) {
        rgb_alpha = 0;
      }
    }
    // vdp2/hon/p08_12.htm#no8_15
    else if (Vdp2Regs->SPCTL & 0x10) { // Enable Sprite Window
      if (spmode >= 0x2 && spmode <= 0x7) {
        rgb_alpha = 0;
      }
    }
    else {
      //u8 *cclist = (u8 *)&Vdp2Regs->CCRSA;
      //cclist[0] &= 0x1F;
      //u8 rgb_alpha = 0xF8 - (((cclist[0] & 0x1F) << 3) & 0xF8);
      alpha = VDP1COLOR(0, 0, 0, 0, 0);
      alpha >>= 24;
    }
    //alpha = rgb_alpha;
    //priority = Vdp2Regs->PRISA & 0x7;
  }
  else{
    int shadow, normalshadow, colorcalc = 0;
    Vdp1ProcessSpritePixel(Vdp2Regs->SPCTL & 0xF, &color, &shadow, &normalshadow, &priority, &colorcalc);
#if 0
    priority = ((u8 *)&Vdp2Regs->PRISA)[priority] & 0x7;
    if (color == 0) {
      alpha = 0;
      priority = 0;
    }
    else{
      alpha = 0xF8;
    }
#endif
    alpha = VDP1COLOR(1, colorcalc, priority, 0, 0);
    alpha >>= 24;
  }
  //alpha |= priority;

  glClearColor((color & 0x1F) / 31.0f, ((color >> 5) & 0x1F) / 31.0f, ((color >> 10) & 0x1F) / 31.0f, alpha / 255.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  FRAMELOG("YglEraseWriteVDP1xx: clear %d\n", _Ygl->readframe);

  if( _Ygl->bWriteCpuFrameBuffer ){
    memset(_Ygl->CpuWriteFrameBuffer,0xFF, _Ygl->rwidth * _Ygl->rheight * 4);
    _Ygl->bWriteCpuFrameBuffer = 0;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  
}

u32 Vdp2ColorRamGetColor(u32 colorindex, int alpha);

//////////////////////////////////////////////////////////////////////////////
void YglFrameChangeVDP1(){
  u32 current_drawframe = 0;
  current_drawframe = _Ygl->drawframe;
  _Ygl->drawframe = _Ygl->readframe;
  _Ygl->readframe = current_drawframe;
  FRAMELOG("YglFrameChangeVDP1: swap drawframe =%d readframe = %d\n", _Ygl->drawframe, _Ygl->readframe);
}


//////////////////////////////////////////////////////////////////////////////
#ifdef VITA_STACK_PROFILE
unsigned v1calls_;
#endif
void YglRenderVDP1(void) {
  VT_SCOPE(VT_GPU_SUBMIT);
#ifdef VITA_STACK_PROFILE
  ++v1calls_;
#endif
  YglLevel * level;
  GLuint cprg=0;
  int j;
  int status;
  FrameProfileAdd("YglRenderVDP1 start");
  YabThreadLock(_Ygl->mutex);
  _Ygl->vdp1_hasMesh = 0;
#ifdef VITA_STACK_PROFILE
  float rb_[4]; memcpy(rb_, _Ygl->vdp1_region[_Ygl->drawframe], sizeof rb_);
  int rbt_ = _Ygl->vdp1_region_tracked[_Ygl->drawframe];
#endif

  YglMatrix m;

  YglLoadIdentity(&m);
  if (Vdp1Regs->TVMR & 0x02) {
    YglOrtho(&m, 0.0f, (float)Vdp1Regs->systemclipX2, (float)Vdp1Regs->systemclipY2, 0.0f, 10.0f, 0.0f);
  }
  else {
    YglOrtho(&m, 0.0f, (float)_Ygl->rwidth, (float)_Ygl->rheight, 0.0f, 10.0f, 0.0f);
  }

  FRAMELOG("YglRenderVDP1: drawframe =%d:%d", _Ygl->drawframe, _Ygl->cpu_framebuffer_write[_Ygl->drawframe]);

  if (_Ygl->pFrameBuffer != NULL) {
    #ifdef YABAUSE_VITAGL
    free(_Ygl->pFrameBuffer);
#endif
    _Ygl->pFrameBuffer = NULL;
    glBindTexture(GL_TEXTURE_2D, _Ygl->smallfbotex);
#ifndef YABAUSE_VITAGL
    glBindBuffer(GL_PIXEL_PACK_BUFFER, _Ygl->vdp1pixelBufferID);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
  }
  YabThreadUnLock(_Ygl->mutex);
  YGLLOG("YglRenderVDP1 %d, PTMR = %d\n", _Ygl->drawframe, Vdp1Regs->PTMR);

  level = &(_Ygl->levels[_Ygl->depth]);
    if( level == NULL ) {
        return;
    }
  cprg = -1;

  YglGenFrameBuffer();
  if(level->prgcurrent != 0)
    YglDrawCpuFramebufferWrite(_Ygl->drawframe);
      
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  YglVdp1AttachColor(_Ygl->vdp1fbo,_Ygl->vdp1FrameBuff[_Ygl->drawframe],0);
  YglVdp1AttachDepth(_Ygl->rboid_depth,_Ygl->rboid_stencil,0);
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if( status != GL_FRAMEBUFFER_COMPLETE ) {
    YGLLOG("YglRenderVDP1: Framebuffer status = %08X\n", status );
    YabThreadUnLock( _Ygl->mutex );
    return;
  }else{
    //YGLLOG("Framebuffer status OK = %08X\n", status );
  }

  glDisable(GL_STENCIL_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glCullFace(GL_FRONT_AND_BACK);
  glDisable(GL_CULL_FACE);

  glViewport(0,0,_Ygl->width,_Ygl->height);
  glScissor(0, 0, _Ygl->width, _Ygl->height);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, YglTM->textureID_in[YglTM->current] );

  for( j=0;j<(level->prgcurrent+1); j++ ) {
    if( level->prg[j].prgid != cprg ) {
      cprg = level->prg[j].prgid;
      glUseProgram(level->prg[j].prg);
    }
    
    if(level->prg[j].setupUniform) {
      level->prg[j].setupUniform((void*)&level->prg[j]);
    }
    if( level->prg[j].currentQuad != 0 ) {
      glUniformMatrix4fv(level->prg[j].mtxModelView, 1, GL_FALSE, (GLfloat*)&m.m[0][0]);
      glVertexAttribPointer(level->prg[j].vertexp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)level->prg[j].quads);
      glVertexAttribPointer(level->prg[j].texcoordp,4,GL_FLOAT,GL_FALSE,0,(GLvoid *)level->prg[j].textcoords );
      if( level->prg[j].vaid != 0 ) {
        glVertexAttribPointer(level->prg[j].vaid,4, GL_FLOAT, GL_FALSE, 0, level->prg[j].vertexAttribute);
      }

      if ( level->prg[j].prgid >= PG_VFP1_GOURAUDSAHDING  && level->prg[j].prgid <= PG_VFP1_MESH ) {
        _Ygl->vdp1_hasMesh = 1;
      }

      if ( level->prg[j].prgid >= PG_VFP1_GOURAUDSAHDING_TESS ) {
#ifdef YABAUSE_VITAGL
        fprintf(stderr, "GPU tessellation selected on Vita\n");
        abort();
#else
#if defined(__XU4__)
        glPatchParameteriOES(GL_PATCH_VERTICES, 4);
#else        
        if (glPatchParameteri) glPatchParameteri(GL_PATCH_VERTICES, 4);
#endif        
        glDrawArrays(GL_PATCHES, 0, level->prg[j].currentQuad / 2);
#endif
      }else{
#ifdef VITA_STACK_PROFILE
        static uint64_t ga_us; static unsigned ga_hit, ga_all, ga_frames;
        uint64_t td0 = sceKernelGetProcessTimeWide();
#endif
#ifdef VITA_STACK_PROFILE
        uint64_t tr0_ = sceKernelGetProcessTimeWide();
#endif
#ifdef VITA_STACK_PROFILE
        uint64_t tr1_ = sceKernelGetProcessTimeWide();
#endif
#ifdef VITA_STACK_PROFILE
        { static unsigned fr_, nd_, nv_[64], np_[64]; static const void *lastl_; static unsigned lastc_;
          extern unsigned v1calls_;
          if (lastc_ != v1calls_ && nd_) { lastc_ = v1calls_; if ((++fr_ & 15) == 0) { char b_[512]; int o_ = 0;
              for (int q_ = 0; q_ < 64; ++q_) if (np_[q_]) o_ += snprintf(b_ + o_, sizeof b_ - o_, " %d:%u/%u", q_, np_[q_], nv_[q_]);
              YuiMsg("v1prg draws=%u wh=%dx%d%s", nd_, _Ygl->width, _Ygl->height, b_); }
            nd_ = 0; memset(nv_, 0, sizeof nv_); memset(np_, 0, sizeof np_); }
          lastc_ = v1calls_; ++nd_; unsigned id_ = (unsigned)level->prg[j].prgid & 63; ++np_[id_]; nv_[id_] += level->prg[j].currentQuad / 2; (void)lastl_; }
#endif
        if (!ABL(8)) glDrawArrays(GL_TRIANGLES, 0, level->prg[j].currentQuad / 2);
#ifdef VITA_STACK_PROFILE
        { uint64_t tr2_ = sceKernelGetProcessTimeWide(); static unsigned bn_;
          if (level->prg[j].currentQuad / 2 > 10000 && (++bn_ & 7) == 0)
            YuiMsg("bigdraw verts=%u arena=%d region_us=%llu draw_us=%llu", level->prg[j].currentQuad / 2, arena,
                   (unsigned long long)(tr1_ - tr0_), (unsigned long long)(tr2_ - tr1_)); }
        ga_us += sceKernelGetProcessTimeWide() - td0; ++ga_all;
        if (ga_all == 512) { (void)ga_frames;
          YuiMsg("diag_v1 draws=%u arena=%u draw_us=%llu", ga_all, ga_hit, (unsigned long long)ga_us);
          ga_us = 0; ga_hit = ga_all = 0;
        }
#endif
      }
      level->prg[j].currentQuad = 0;
      _Ygl->cpu_framebuffer_write[ _Ygl->drawframe] = 0;
    }

    if( level->prg[j].cleanupUniform ){
      level->prg[j].cleanupUniform((void*)&level->prg[j]);
    }
  }
  
#ifdef VITA_STACK_PROFILE
  { static unsigned n_; const float *r_ = _Ygl->vdp1_region[_Ygl->drawframe];
    if ((++n_ & 31) < 4) YuiMsg("v1region n=%u df=%d tvmr=%x before=%d:%.0f,%.0f-%.0f,%.0f after=%d:%.0f,%.0f-%.0f,%.0f", n_, _Ygl->drawframe, Vdp1Regs->TVMR,
      rbt_, rb_[0], rb_[1], rb_[2], rb_[3], _Ygl->vdp1_region_tracked[_Ygl->drawframe], r_[0], r_[1], r_[2], r_[3]); }
#endif
  level->prgcurrent = 0;

#ifdef YABAUSE_VITAGL
  /* Same-context GPU consumers retain order; only CPU observation waits. */
  _Ygl->sync = 1;
#else
  if(_Ygl->sync != 0) {
    glDeleteSync(_Ygl->sync);
    _Ygl->sync = 0;
  }

  _Ygl->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
#endif


  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
#if !defined(YABAUSE_VITAGL) || !defined(VITA_VDP1_DEFER_FLUSH)
  glFlush();  
#endif
  /* On vitaGL glFlush starts the currently bound display scene. The next
   * actual draw/clear/target change can end VDP1 and start its real consumer
   * instead. CPU framebuffer observation still calls glFinish explicitly;
   * feedback copies/readbacks and presentation keep their own boundaries. */
  VITA_GPU_STAGE(2);
  FrameProfileAdd("YglRenderVDP1 end");
}

//////////////////////////////////////////////////////////////////////////////
void YglDmyRenderVDP1(void) {
}

//////////////////////////////////////////////////////////////////////////////
void YglNeedToUpdateWindow()
{
  _Ygl->bUpdateWindow = 1;
}

void YglSetVdp2Window()
{
    int bwin0,bwin1,bspwin;
   //if( _Ygl->bUpdateWindow && (_Ygl->win0_vertexcnt != 0 || _Ygl->win1_vertexcnt != 0 ) )

    bwin0 = (Vdp2Regs->WCTLC >> 9) &0x01;
    bwin1 = (Vdp2Regs->WCTLC >> 11) &0x01;
    bspwin = ((Vdp2Regs->WCTLC >> 13) & 0x01); // ((Vdp2Regs->SPCTL >> 4) & 0x03) == 0x01;
   if( (_Ygl->win0_vertexcnt != 0 || _Ygl->win1_vertexcnt != 0 || bspwin) )
   {

     Ygl_uniformWindow(&_Ygl->windowpg);
     glUniformMatrix4fv( _Ygl->windowpg.mtxModelView, 1, GL_FALSE, (GLfloat*) &_Ygl->mtxModelView.m[0][0] );

      //
     glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
     glDepthMask(GL_FALSE);
     glDisable(GL_DEPTH_TEST);

     //glClearStencil(0);
     //glClear(GL_STENCIL_BUFFER_BIT);
     glEnable(GL_STENCIL_TEST);

     glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);

      if( _Ygl->win0_vertexcnt != 0 )
      {
           glStencilMask(0x01);
           glStencilFunc(GL_ALWAYS,0x01,0x01);
           glVertexAttribPointer(_Ygl->windowpg.vertexp,2,YGL_WINDOW_COORD_FORMAT, GL_FALSE,0,(GLvoid *)_Ygl->win0v );
           glDrawArrays(GL_TRIANGLE_STRIP,0,_Ygl->win0_vertexcnt);
      }

      if( _Ygl->win1_vertexcnt != 0 )
      {
          glStencilMask(0x02);
          glStencilFunc(GL_ALWAYS,0x02,0x02);
          glVertexAttribPointer(_Ygl->windowpg.vertexp,2,YGL_WINDOW_COORD_FORMAT, GL_FALSE,0,(GLvoid *)_Ygl->win1v );
          glDrawArrays(GL_TRIANGLE_STRIP,0,_Ygl->win1_vertexcnt);
      }

      // 8. sprite window
      if (bspwin) {
        glStencilMask(0x04);
        glStencilFunc(GL_ALWAYS, 0x04, 0x04);
        YglRenderFrameBufferShadow();
      }

      glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
      glDepthMask(GL_TRUE);
      glEnable(GL_DEPTH_TEST);
      glDisable(GL_STENCIL_TEST);
      glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
      glStencilFunc(GL_ALWAYS,0,0xFF);
      glStencilMask(0xFFFFFFFF);

      _Ygl->bUpdateWindow = 0;
   }
   return;
}

extern Vdp2 * fixVdp2Regs;
void YglUpdateVdp2Reg() {
  int i,line;
  u8 *cclist  = (u8 *)&fixVdp2Regs->CCRSA;
  u8 *prilist = (u8 *)&fixVdp2Regs->PRISA;

  for (i = 0; i < 8; i++) {
    _Ygl->fbu_.u_alpha[i*4] = (float)(0xFF - (((cclist[i] & 0x1F) << 3) & 0xF8)) / 255.0f;
    _Ygl->fbu_.u_pri[i*4] = ((float)(prilist[i] & 0x7) / 10.0f) + 0.05f;
  }
  _Ygl->fbu_.u_cctll = ((float)((fixVdp2Regs->SPCTL >> 8) & 0x07) / 10.0f) + 0.05f;

  if (*Vdp2External.perline_alpha_draw & 0x40) {
    u32 * linebuf;
    int line_shift = 0;
    if (_Ygl->rheight > 256) {
      line_shift = 1;
    }
    else {
      line_shift = 0;
    }

    linebuf = YglGetPerlineBuf(&_Ygl->bg[SPRITE], _Ygl->rheight, 1 + 8 + 8);
    for (line = 0; line < _Ygl->rheight; line++) {
      linebuf[line] = 0xFF000000;
      Vdp2 * lVdp2Regs = &Vdp2Lines[line >> line_shift];

      u8 *cclist = (u8 *)&lVdp2Regs->CCRSA;
      u8 *prilist = (u8 *)&lVdp2Regs->PRISA;
      for (i = 0; i < 8; i++) {
        linebuf[line + _Ygl->rheight * (1 + i)] = (prilist[i] & 0x7) << 24;
        linebuf[line + _Ygl->rheight * (1 + 8 + i)] = (0xFF - (((cclist[i] & 0x1F) << 3) & 0xF8)) << 24;
      }

      if (lVdp2Regs->CLOFEN & 0x40) {

        // color offset enable
        if (lVdp2Regs->CLOFSL & 0x40)
        {
          // color offset B
          vdp1cor = lVdp2Regs->COBR & 0xFF;
          if (lVdp2Regs->COBR & 0x100)
            vdp1cor |= 0xFFFFFF00;

          vdp1cog = lVdp2Regs->COBG & 0xFF;
          if (lVdp2Regs->COBG & 0x100)
            vdp1cog |= 0xFFFFFF00;

          vdp1cob = lVdp2Regs->COBB & 0xFF;
          if (lVdp2Regs->COBB & 0x100)
            vdp1cob |= 0xFFFFFF00;
        }
        else
        {
          // color offset A
          vdp1cor = lVdp2Regs->COAR & 0xFF;
          if (lVdp2Regs->COAR & 0x100)
            vdp1cor |= 0xFFFFFF00;

          vdp1cog = lVdp2Regs->COAG & 0xFF;
          if (lVdp2Regs->COAG & 0x100)
            vdp1cog |= 0xFFFFFF00;

          vdp1cob = lVdp2Regs->COAB & 0xFF;
          if (lVdp2Regs->COAB & 0x100)
            vdp1cob |= 0xFFFFFF00;
        }


        linebuf[line] |= ((int)(128.0f + (vdp1cor / 2.0)) & 0xFF) << 0;
        linebuf[line] |= ((int)(128.0f + (vdp1cog / 2.0)) & 0xFF) << 8;
        linebuf[line] |= ((int)(128.0f + (vdp1cob / 2.0)) & 0xFF) << 16;
      }
      else {
        linebuf[line] |= 0x00808080;
      }
    }
    YglSetPerlineBuf(&_Ygl->bg[SPRITE], linebuf, _Ygl->rheight, 1 + 8 + 8);
    _Ygl->vdp1_lineTexture = _Ygl->bg[SPRITE].lincolor_tex;
  }
  else {
    _Ygl->vdp1_lineTexture = 0;
    if (fixVdp2Regs->CLOFEN & 0x40)
    {
      // color offset enable
      if (fixVdp2Regs->CLOFSL & 0x40)
      {
        // color offset B
        vdp1cor = fixVdp2Regs->COBR & 0xFF;
        if (fixVdp2Regs->COBR & 0x100)
          vdp1cor |= 0xFFFFFF00;

        vdp1cog = fixVdp2Regs->COBG & 0xFF;
        if (fixVdp2Regs->COBG & 0x100)
          vdp1cog |= 0xFFFFFF00;

        vdp1cob = fixVdp2Regs->COBB & 0xFF;
        if (fixVdp2Regs->COBB & 0x100)
          vdp1cob |= 0xFFFFFF00;
      }
      else
      {
        // color offset A
        vdp1cor = fixVdp2Regs->COAR & 0xFF;
        if (fixVdp2Regs->COAR & 0x100)
          vdp1cor |= 0xFFFFFF00;

        vdp1cog = fixVdp2Regs->COAG & 0xFF;
        if (fixVdp2Regs->COAG & 0x100)
          vdp1cog |= 0xFFFFFF00;

        vdp1cob = fixVdp2Regs->COAB & 0xFF;
        if (fixVdp2Regs->COAB & 0x100)
          vdp1cob |= 0xFFFFFF00;
      }
    }
    else // color offset disable
      vdp1cor = vdp1cog = vdp1cob = 0;
  }


  _Ygl->fbu_.u_coloroffset[0] = vdp1cor / 255.0f;
  _Ygl->fbu_.u_coloroffset[1] = vdp1cog / 255.0f;
  _Ygl->fbu_.u_coloroffset[2] = vdp1cob / 255.0f;
  _Ygl->fbu_.u_coloroffset[3] = 0.0f;

  // For Line Color insersion
  _Ygl->fbu_.u_emu_height = (float)_Ygl->rheight / (float)_Ygl->height;
  _Ygl->fbu_.u_vheight = (float)_Ygl->height;
  _Ygl->fbu_.u_color_ram_offset = (fixVdp2Regs->CRAOFB & 0x70) << 4;
  if (_Ygl->resolution_mode == RES_NATIVE) {
    _Ygl->fbu_.u_viewport_offset = (float)_Ygl->originy;
  }
  else {
    _Ygl->fbu_.u_viewport_offset = 0.0f;
  }

  // Check if transparent sprite window
  // hard/vdp2/hon/p08_12.htm#SPWINEN_
  if ( (fixVdp2Regs->SPCTL & 0x10) && // Sprite Window is enabled
       ((fixVdp2Regs->SPCTL & 0xF)  >=2 && (fixVdp2Regs->SPCTL & 0xF) < 8)) // inside sprite type
  {
    _Ygl->fbu_.u_sprite_window = 1;  
  }else{
    _Ygl->fbu_.u_sprite_window = 0;  
  }
  

#ifndef YABAUSE_VITAGL
  if (_Ygl->framebuffer_uniform_id_ == 0) {
    glGenBuffers(1, &_Ygl->framebuffer_uniform_id_);
  }
  glBindBuffer(GL_UNIFORM_BUFFER, _Ygl->framebuffer_uniform_id_);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformFrameBuffer), &_Ygl->fbu_, GL_STATIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
#endif

}

#ifdef VITA_FB_PRIORITY_CULL
#include "framebuffer_priority.h"
#endif
void YglRenderFrameBuffer(int from, int to) {
  if (ABL(2)) return;

  YglWindowCoord   vertices[12];
  GLfloat texcord[12];
  float offsetcol[4];
  int bwin0, bwin1, logwin0, logwin1, bwinsp, logwinsp, winmode;
  int is_addcolor = 0;
  int cwidth = 0;
  int cheight = 0;

  if(_Ygl->cpu_framebuffer_write[_Ygl->readframe]!=0) FRAMELOG("YglRenderFrameBuffer: CPU write to readframe FB %d:%d %d to %d\n", _Ygl->readframe, _Ygl->cpu_framebuffer_write[_Ygl->readframe] , from, to, _Ygl->readframe);

  YglGenFrameBuffer();
  YglDrawCpuFramebufferWrite(_Ygl->readframe);

  
  // Out of range, do nothing
  if (_Ygl->vdp1_maxpri < from) return;
  if (_Ygl->vdp1_minpri > to) return;

#ifdef VITA_FB_PRIORITY_CULL
  if (_Ygl->vdp1_lineTexture == 0) {
    static unsigned checked, skipped;
    int visible=YglFramebufferPriorityVisible(_Ygl->fbu_.u_pri,
                                              (float)from/10.0f,(float)to/10.0f);
    ++checked;
    skipped+=!visible;
    if ((checked&255)==0)
      YuiMsg("framebuffer_priority_cull checked=%u skipped=%u",checked,skipped);
    if (!visible) return;
  }
#endif

  if (_Ygl->vdp1_lineTexture != 0){ // hbalnk-in function
    Ygl_uniformVDP2DrawFramebuffer_perline(&_Ygl->renderfb, (float)(from) / 10.0f, (float)(to) / 10.0f, _Ygl->vdp1_lineTexture);
  }else{
    Ygl_uniformVDP2DrawFramebuffer(&_Ygl->renderfb, (float)(from) / 10.0f, (float)(to) / 10.0f, offsetcol, 1 );
  }

  glBindTexture(GL_TEXTURE_2D, _Ygl->vdp1FrameBuff[_Ygl->readframe]);
  YglUnbindPixelUnpack();
  
  //

  YglMatrix result;
  if (Vdp1Regs->TVMR & 0x02){
    YglMatrix rotate;
    YglLoadIdentity(&rotate);
    rotate.m[0][0] = paraA.deltaX;
    rotate.m[0][1] = paraA.deltaY;
    rotate.m[1][0] = paraA.deltaXst;
    rotate.m[1][1] = paraA.deltaYst;
    YglTranslatef(&rotate, -paraA.Xst, -paraA.Yst, 0.0f);
    if( (Vdp2Regs->TVMD&0xC0) == 0x80 ){
      YglScalef(&rotate, 1.0f, 0.5f, 1.0f);
    }
    YglMatrixMultiply(&result, &_Ygl->mtxModelView, &rotate);
    cwidth = Vdp1Regs->systemclipX2;
    cheight = Vdp1Regs->systemclipY2;
  }
  else{
    memcpy(&result, &_Ygl->mtxModelView, sizeof(result));
    cwidth = _Ygl->rwidth;
    cheight = _Ygl->rheight;
  }
#define FB_DRAW() glDrawArrays(GL_TRIANGLES, 0, 6)
#define FB_RESTORE() ((void)0)



   // render
   vertices[0] = 0 - 0.5;
   vertices[1] = 0 - 0.5;
   vertices[2] = cwidth + 1 - 0.5;
   vertices[3] = 0 - 0.5;
   vertices[4] = cwidth + 1 - 0.5;
   vertices[5] = cheight + 1 - 0.5;

   vertices[6] = 0 - 0.5;
   vertices[7] = 0 - 0.5;
   vertices[8] = cwidth + 1 - 0.5;
   vertices[9] = cheight + 1 - 0.5;
   vertices[10] = 0 - 0.5;
   vertices[11] = cheight + 1 - 0.5;

   texcord[0] = 0.0f;
   texcord[1] = 1.0f;
   texcord[2] = 1.0f;
   texcord[3] = 1.0f;
   texcord[4] = 1.0f;
   texcord[5] = 0.0f;

   texcord[6] = 0.0f;
   texcord[7] = 1.0f;
   texcord[8] = 1.0f;
   texcord[9] = 0.0f;
   texcord[10] = 0.0f;
   texcord[11] = 0.0f;

   // Window Mode
   bwin0 = (Vdp2Regs->WCTLC >> 9) &0x01;
   logwin0 = (Vdp2Regs->WCTLC >> 8) & 0x01;
   bwin1 = ((Vdp2Regs->WCTLC >> 11) &0x01) << 1;
   logwin1 = ((Vdp2Regs->WCTLC >> 10) & 0x01) << 1;
   bwinsp = ((Vdp2Regs->WCTLC >> 13) & 0x01) << 2;
   logwinsp = (((Vdp2Regs->WCTLC >> 12) & 0x01)?0:1) << 2; // Invarse?
      
   winmode = (Vdp2Regs->WCTLC >> 15) & 0x01;

   int bwin_cc0 = (Vdp2Regs->WCTLD >> 9) & 0x01;
   int logwin_cc0 = (Vdp2Regs->WCTLD >> 8) & 0x01;
   int bwin_cc1 = (Vdp2Regs->WCTLD >> 11) & 0x01;
   int logwin_cc1 = (Vdp2Regs->WCTLD >> 10) & 0x01;
   int winmode_cc = (Vdp2Regs->WCTLD >> 15) & 0x01;
   

   if (bwin_cc0 || bwin_cc1){

     glEnable(GL_STENCIL_TEST);
     glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

     if (bwin_cc0 && !bwin_cc1)
     {
       if (logwin_cc0)
       {
         glStencilFunc(GL_EQUAL, 0x01, 0x01);
       }
       else{
         glStencilFunc(GL_NOTEQUAL, 0x01, 0x01);
       }
     }
     else if (!bwin_cc0 && bwin_cc1) {

       if (logwin_cc1)
       {
         glStencilFunc(GL_EQUAL, 0x02, 0x02);
       }
       else{
         glStencilFunc(GL_NOTEQUAL, 0x02, 0x02);
       }
     }
     else if (bwin_cc0 && bwin_cc1) {
       // and
       if (winmode_cc == 0x0)
       {
         if (logwin_cc0 == 1 && logwin_cc1 == 1){ // show inside
           glStencilFunc(GL_EQUAL, 0x03, 0x03);
         }
         else if (logwin_cc0 == 0 && logwin_cc1 == 0) {
           glStencilFunc(GL_GREATER, 0x01, 0x03);
         }
         else{
           glStencilFunc(GL_ALWAYS, 0x00, 0x00);
         }

         // OR
       }
       else
       {
         // OR
         if (logwin_cc0 == 1 && logwin_cc1 == 1){ // show inside
           glStencilFunc(GL_LEQUAL, 0x01, 0x03);
         }
         else if (logwin_cc0 == 0 && logwin_cc1 == 0) {
           glStencilFunc(GL_NOTEQUAL, 0x03, 0x03);
         }
         else{
           glStencilFunc(GL_ALWAYS, 0x00, 0x00);
         }
       }
     }

     glUniformMatrix4fv(_Ygl->renderfb.mtxModelView, 1, GL_FALSE, (GLfloat*)result.m);
     glVertexAttribPointer(_Ygl->renderfb.vertexp, 2, YGL_WINDOW_COORD_FORMAT, GL_FALSE, 0, (GLvoid *)vertices);
     glVertexAttribPointer(_Ygl->renderfb.texcoordp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)texcord);
     glDrawArrays(GL_TRIANGLES, 0, 6);

     glDepthFunc(GL_GREATER);
     glDisable(GL_BLEND);


     glDisable(GL_STENCIL_TEST);
     glStencilFunc(GL_ALWAYS, 0, 0xFF);
     if (bwin0 || bwin1 || bwinsp)
     {
       glEnable(GL_STENCIL_TEST);
       glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

       int winmask = (bwin0 | bwin1 | bwinsp);
       int winflag = 0;
       if (winmode == 0) { // and
         if (bwin0)  winflag = logwin0;
         if (bwin1)  winflag |= logwin1;
         if (bwinsp) winflag |= logwinsp;
         glStencilFunc(GL_EQUAL, winflag, winmask);
       }
       else { // or
         winflag = winmask;
         if (bwin0)  winflag &= ~logwin0;
         if (bwin1)  winflag &= ~logwin1;
         if (bwinsp) winflag &= ~logwinsp;
         glStencilFunc(GL_NOTEQUAL, winflag, winmask);
       }
     }

     Ygl_uniformVDP2DrawFramebuffer(&_Ygl->renderfb, (float)(from) / 10.0f, (float)(to) / 10.0f, offsetcol, 0 );
     glDrawArrays(GL_TRIANGLES, 0, 6);

     glDepthFunc(GL_GEQUAL);
     glEnable(GL_BLEND);
     if (bwin0 || bwin1 || bwinsp)
     {
       glDisable(GL_STENCIL_TEST);
       glStencilFunc(GL_ALWAYS, 0, 0xFF);
     }
     return;
   }


   if (bwin0 || bwin1 || bwinsp)
   {
      glEnable(GL_STENCIL_TEST);
      glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);

      int winmask = (bwin0 | bwin1 | bwinsp);
      int winflag = 0;
      if (winmode == 0) { // and
        if (bwin0)  winflag = logwin0;
        if (bwin1)  winflag |= logwin1;
        if (bwinsp) winflag |= logwinsp;
        glStencilFunc(GL_EQUAL, winflag, winmask);
      }
      else { // or
        winflag = winmask;
        if (bwin0)  winflag &= ~logwin0;
        if (bwin1)  winflag &= ~logwin1;
        if (bwinsp) winflag &= ~logwinsp;
        glStencilFunc(GL_NOTEQUAL, winflag, winmask);
      }
   }

   glUniformMatrix4fv(_Ygl->renderfb.mtxModelView, 1, GL_FALSE, (GLfloat*)result.m);
   glVertexAttribPointer(_Ygl->renderfb.vertexp,2,YGL_WINDOW_COORD_FORMAT, GL_FALSE,0,(GLvoid *)vertices );
   glVertexAttribPointer(_Ygl->renderfb.texcoordp,2,GL_FLOAT,GL_FALSE,0,(GLvoid *)texcord );
   glDrawArrays(GL_TRIANGLES, 0, 6);

#if 0
   if (is_addcolor == 1){
     Ygl_uniformVDP2DrawFramebuffer_addcolor_shadow(&_Ygl->renderfb, (float)(from) / 10.0f, (float)(to) / 10.0f, offsetcol);
     glUniformMatrix4fv(_Ygl->renderfb.mtxModelView, 1, GL_FALSE, (GLfloat*)result.m);
     glVertexAttribPointer(_Ygl->renderfb.vertexp, 2, YGL_WINDOW_COORD_FORMAT, GL_FALSE, 0, (GLvoid *)vertices);
     glVertexAttribPointer(_Ygl->renderfb.texcoordp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)texcord);
     glDrawArrays(GL_TRIANGLES, 0, 6);
   }
#endif

   if( bwin0 || bwin1 || bwinsp)
   {
      glDisable(GL_STENCIL_TEST);
      glStencilFunc(GL_ALWAYS,0,0xFF);
   }
   glEnable(GL_BLEND);
}


void YglRenderFrameBufferShadow() {

  YglWindowCoord   vertices[12];
  GLfloat texcord[12];
  float offsetcol[4];
  int bwin0, bwin1, logwin0, logwin1, winmode;
  int is_addcolor = 0;

  YglGenFrameBuffer();

  Ygl_uniformVDP2DrawFrameBufferShadow(&_Ygl->renderfb);

  glBindTexture(GL_TEXTURE_2D, _Ygl->vdp1FrameBuff[_Ygl->readframe]);
  YglUnbindPixelUnpack();

  YglMatrix result;
  if (Vdp1Regs->TVMR & 0x02) {
    YglMatrix rotate;
    YglLoadIdentity(&rotate);
    rotate.m[0][0] = paraA.deltaX;
    rotate.m[0][1] = paraA.deltaY;
    rotate.m[1][0] = paraA.deltaXst;
    rotate.m[1][1] = paraA.deltaYst;
    YglTranslatef(&rotate, -paraA.Xst, -paraA.Yst, 0.0f);
    YglMatrixMultiply(&result, &_Ygl->mtxModelView, &rotate);
  }
  else {
    memcpy(&result, &_Ygl->mtxModelView, sizeof(result));
  }

  // render
  vertices[0] = 0 - 0.5;
  vertices[1] = 0 - 0.5;
  vertices[2] = _Ygl->rwidth + 1 - 0.5;
  vertices[3] = 0 - 0.5;
  vertices[4] = _Ygl->rwidth + 1 - 0.5;
  vertices[5] = _Ygl->rheight + 1 - 0.5;

  vertices[6] = 0 - 0.5;
  vertices[7] = 0 - 0.5;
  vertices[8] = _Ygl->rwidth + 1 - 0.5;
  vertices[9] = _Ygl->rheight + 1 - 0.5;
  vertices[10] = 0 - 0.5;
  vertices[11] = _Ygl->rheight + 1 - 0.5;

  texcord[0] = 0.0f;
  texcord[1] = 1.0f;
  texcord[2] = 1.0f;
  texcord[3] = 1.0f;
  texcord[4] = 1.0f;
  texcord[5] = 0.0f;

  texcord[6] = 0.0f;
  texcord[7] = 1.0f;
  texcord[8] = 1.0f;
  texcord[9] = 0.0f;
  texcord[10] = 0.0f;
  texcord[11] = 0.0f;
  glUniformMatrix4fv(_Ygl->renderfb.mtxModelView, 1, GL_FALSE, (GLfloat*)result.m);
  glVertexAttribPointer(_Ygl->renderfb.vertexp, 2, YGL_WINDOW_COORD_FORMAT, GL_FALSE, 0, (GLvoid *)vertices);
  glVertexAttribPointer(_Ygl->renderfb.texcoordp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)texcord);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}


void YglSetClearColor(float r, float g, float b){
  _Ygl->clear_r = r;
  _Ygl->clear_g = g;
  _Ygl->clear_b = b;
}

#ifdef VITA_DIAG_ABLATE
unsigned vt_frame_seq;
#endif
void YglRender(void) {
#ifdef VITA_DIAG_ABLATE
  ++vt_frame_seq;
#endif
  VT_SCOPE(VT_GPU_SUBMIT);
   YglLevel * level;
   GLuint cprg=0;
   int from = 0;
   int to   = 0;
   YglMatrix mtx;
   YglMatrix dmtx;
   unsigned int i,j;
   int ccwindow = 0;

   YGLLOG("YglRender\n");

   FrameProfileAdd("YglRender start");
   if ((Vdp2Regs->TVMD & 0x8000) == 0){
     glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
     glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
     goto render_finish;
   }

   if (YglIsNeedFrameBuffer() == 1) {
     if (_Ygl->fxaa_fbotex == 0){
       YglGenerateAABuffer();
     }
     glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->fxaa_fbo);
     _Ygl->targetfbo = _Ygl->fxaa_fbo;

   } else {
     glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
     _Ygl->targetfbo = _Ygl->default_fbo;
   }

   glClearDepthf(0.0f);
   glDepthMask(GL_TRUE);
   glEnable(GL_DEPTH_TEST);
   glDisable(GL_SCISSOR_TEST);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

   glViewport(_Ygl->originx, _Ygl->originy, _Ygl->width, _Ygl->height);
   glEnable(GL_SCISSOR_TEST);
   if (_Ygl->resolution_mode != RES_NATIVE) {
     glViewport(0, 0, _Ygl->width, _Ygl->height);
     glScissor(0, 0, _Ygl->width, _Ygl->height);
   }
   else {
     glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
     glScissor(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
   }

   if (_Ygl->aamode == AA_FXAA) {
     glViewport(0, 0, _Ygl->width, _Ygl->height);
     glScissor(0, 0, _Ygl->width, _Ygl->height);
   }

   if (_Ygl->aamode == AA_SCANLINE_FILTER && _Ygl->rheight <= 256) {
     glViewport(0, 0, _Ygl->width, _Ygl->height);
     glScissor(0, 0, _Ygl->width, _Ygl->height);
   }

   if ((fixVdp2Regs->BKTAU & 0x8000) != 0) {
     YglDrawBackScreen(GlWidth, GlHeight);
   }
   else {

     if (_Ygl->clear_r != 0.0 || _Ygl->clear_g != 0.0 || _Ygl->clear_b != 0.0) {
       glClearColor(_Ygl->clear_r, _Ygl->clear_g, _Ygl->clear_b, 1.0f);
       glClear(GL_COLOR_BUFFER_BIT);
     }
   }
   
   if (_Ygl->texture_manager == NULL) goto render_finish;
   YglUpdateVdp2Reg();

   glBindTexture(GL_TEXTURE_2D, YglTM->textureID_in[YglTM->current]);
   YglUnbindPixelUnpack();

  // Color Calcurate Window  
   ccwindow = ((Vdp2Regs->WCTLD >> 9) & 0x01);
   ccwindow |= ((Vdp2Regs->WCTLD >> 11) & 0x01);

   YglSetVdp2Window();

   FRAMELOG("YglRenderFrameBuffer: fb %d", _Ygl->readframe);

   // This is workaround for Azel disc 2
   // Only top and second prioriy pixel is calculated
#if 0 // There are many regressions...
   int lowpri = -1;
   int hitcnt = 0;
   if ( (fixVdp2Regs->CCCTL & 0x500) == 0x100 ) {
     for (i = _Ygl->depth; i > 0 ; i--)
     {
       level = _Ygl->levels + i;
       if (level->prgcurrent != 0) {
         for (j = (level->prgcurrent + 1); j > 0 ; j--) {
           if (level->prg[j].blendmode & VDP2_CC_ADD) {
             lowpri = i;
             hitcnt++;
           }
         }
       }
     }
     if (hitcnt < 3) {
       lowpri = -1;
     }
   }
#endif


  // 12.14 CCRTMD                               // TODO: MSB perpxel transparent is not uported yet
  if (((Vdp2Regs->CCCTL >> 9) & 0x01) == 0x01 /*&& ((Vdp2Regs->SPCTL >> 12) & 0x3 != 0x03)*/ ){
    YglRenderDestinationAlpha();
  }
  else
  {
    glEnable(GL_BLEND);
    int blendfunc_src = GL_SRC_ALPHA;
    int blendfunc_dst = GL_ONE_MINUS_SRC_ALPHA;

    YglLoadIdentity(&mtx);
    cprg = -1;
    YglTranslatef(&mtx, 0.0f, 0.0f, -1.0f);
    for (i = 0; i < _Ygl->depth; i++)
    {
      level = _Ygl->levels + i;
      if (level->blendmode != 0x00)
      {
        to = i;

        glEnable(GL_BLEND);
        glBlendFunc(blendfunc_src, blendfunc_dst);

        if (Vdp1External.disptoggle & 0x01) { VITA_GPU_STAGE(3); YglRenderFrameBuffer(from, to); VITA_GPU_STAGE(6); }
        from = to;

        // clean up
        cprg = -1;
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, YglTM->textureID_in[YglTM->current]);
                YglUnbindPixelUnpack();

      }

      glDisable(GL_STENCIL_TEST);
      for (j = 0; j < (level->prgcurrent + 1); j++)
      {
        YglMatrixMultiply(&dmtx, &mtx, &_Ygl->mtxModelView);

        if (level->prg[j].prgid != cprg)
        {
          cprg = level->prg[j].prgid;
          glUseProgram(level->prg[j].prg);
        }
        if (level->prg[j].setupUniform)
        {
          level->prg[j].setupUniform((void*)&level->prg[j]);
        }

        if (level->prg[j].currentQuad != 0)
        {
#ifdef YABAUSE_VITAGL
          if (j < 2 && YuiCapturePending()) {
            YglProgram *p = &level->prg[j];
            GLint texture = 0, active = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            /* GPU rotation output is intentionally absent from CPU staging.
             * Do not read an in-flight render target through a CPU pointer. */
            const int gpu_rotation = YglVitaRotationWritten();
            const u32 *atlas_gpu = gpu_rotation ? NULL : vglGetTexDataPointer(GL_TEXTURE_2D);
            unsigned atlas_errors = 0, cram_errors = 0, stale_palette = 0;
            if (atlas_gpu && texture == YglTM->textureID_in[YglTM->current])
              for (unsigned n = 0; n < YglTM->width * YglTM->yMax; ++n)
                atlas_errors += atlas_gpu[n] != YglTM->texture_in[YglTM->current][n];
            glActiveTexture(GL_TEXTURE1);
            GLint cram_binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &cram_binding);
            const u32 *cram_gpu = vglGetTexDataPointer(GL_TEXTURE_2D);
            if (cram_gpu && _Ygl->cram_tex_buf && cram_binding == _Ygl->cram_tex)
              for (unsigned n = 0; n < 2048; ++n)
                cram_errors += cram_gpu[n] != _Ygl->cram_tex_buf[n];
            if (Vdp2Internal.ColorMode <= 1)
              for (unsigned n = 0; n < 2048; ++n) {
                u32 raw = T2ReadWord(Vdp2ColorRam, n * 2);
                u32 expected = SAT2YAB1((u32)((raw & 0x8000) ? 255 : 0), raw);
                stale_palette += expected != _Ygl->cram_tex_buf[n];
              }
            glActiveTexture(active);
            YuiMsg("capture_palette stale_entries=%u", stale_palette);
            YuiMsg("capture_rotation_gpu atlas_written=%d cpu_atlas_comparison_skipped=%d",gpu_rotation,gpu_rotation);
            YuiMsg("capture_texture atlas_mismatch=%u cram_mismatch=%u coloroffset=%g,%g,%g,%g palette0=%08x palette1=%08x palette255=%08x mode=%d",
              atlas_errors, cram_errors, p->color_offset_val[0], p->color_offset_val[1],
              p->color_offset_val[2], p->color_offset_val[3], _Ygl->cram_tex_buf[0],
              _Ygl->cram_tex_buf[1], _Ygl->cram_tex_buf[255], Vdp2Internal.ColorMode);
            YuiMsg("capture_layer priority=%u batch=%u program=%d count=%d atlas=%ux%u rows=%u texture=%d expected_texture=%u pos=%g,%g uv=%g,%g matrix_diag=%g,%g,%g,%g translation=%g,%g,%g attrs=%d,%d mvp=%d",
              i, j, p->prgid, p->currentQuad, YglTM->width, YglTM->height,
              YglTM->yMax, texture, YglTM->textureID_in[YglTM->current],
              p->quads[0], p->quads[1], p->textcoords[0], p->textcoords[1],
              dmtx.m[0][0], dmtx.m[1][1], dmtx.m[2][2], dmtx.m[3][3],
              dmtx.m[0][3], dmtx.m[1][3], dmtx.m[2][3],
              p->vertexp, p->texcoordp, p->mtxModelView);
          }
#endif
          if (level->prg[j].prgid == PG_LINECOLOR_INSERT ||
              level->prg[j].prgid == PG_LINECOLOR_INSERT_CRAM || 
             (level->prg[j].blendmode & VDP2_CC_BLUR) ){
            glDisable(GL_BLEND);
          }else{
            if ((level->prg[j].blendmode & 0x03) == VDP2_CC_NONE){
              glDisable(GL_BLEND);
            }
            else if ((level->prg[j].blendmode & 0x03) == VDP2_CC_RATE){
                glEnable(GL_BLEND);
                glBlendFunc(blendfunc_src, blendfunc_dst);
            }
            else if ( (level->prg[j].blendmode&0x03) == VDP2_CC_ADD){

#if 1 // There are many regressions...
              glEnable(GL_BLEND);
              glBlendFunc(GL_ONE, GL_SRC_ALPHA);
#else
              // This is workaround for Azel disc 2
              if ((fixVdp2Regs->CCCTL & 0x500) == 0x100) {
                if (lowpri == i) {
                  glDisable(GL_BLEND);
                }
                else {
                  glEnable(GL_BLEND);

                  if (level->prg[j].specialcolormode == 0) {
                    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
                  }
                  else {
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                  }
                }
              }
              else {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_SRC_ALPHA);
              }
#endif
            }
          }

          if ((level->prg[j].bwin0 != 0 || level->prg[j].bwin1 != 0) || (level->prg[j].blendmode != VDP2_CC_NONE && ccwindow) ){
            YglSetupWindow(&level->prg[j]);
          }

          glUniformMatrix4fv(level->prg[j].mtxModelView, 1, GL_FALSE, (GLfloat*)&dmtx.m[0][0]);
          glVertexAttribPointer(level->prg[j].vertexp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)level->prg[j].quads);
          glVertexAttribPointer(level->prg[j].texcoordp, 4, GL_FLOAT, GL_FALSE, 0, (GLvoid *)level->prg[j].textcoords);
          if (level->prg[j].vaid != 0) { glVertexAttribPointer(level->prg[j].vaid, 4, GL_FLOAT, GL_FALSE, 0, level->prg[j].vertexAttribute); }
#ifdef VITA_VDP2_OPAQUE_RUNS
          if(ABL(4) || YglDrawOpaqueRuns(&level->prg[j],&dmtx.m[0][0])) { }
          else
#endif
#ifdef VITA_VDP2_INDEXED_QUADS
          if (level->prg[j].prgid == PG_NORMAL || level->prg[j].prgid == PG_VDP2_NORMAL_CRAM)
            YglDrawIndexedVdp2Quads(&level->prg[j]);
          else
#endif
          if (!ABL(4)) glDrawArrays(GL_TRIANGLES, 0, level->prg[j].currentQuad / 2);
#ifdef VITA_DIAG_ABLATE
          { static unsigned f_; extern unsigned vt_frame_seq; static unsigned last_;
            if (last_ != vt_frame_seq) { last_ = vt_frame_seq; ++f_; }
            if ((f_ & 63) == 1)
              YuiMsg("v2draw frame=%u level=%d batch=%d prg=%d verts=%d blend=%d win=%d,%d", f_, i, j,
                     level->prg[j].prgid, level->prg[j].currentQuad / 2, level->prg[j].blendmode,
                     level->prg[j].bwin0, level->prg[j].bwin1); }
#endif

          if (level->prg[j].bwin0 != 0 || level->prg[j].bwin1 != 0 || (level->prg[j].blendmode != VDP2_CC_NONE && ccwindow) ){
            level->prg[j].matrix = (GLfloat*)dmtx.m;
            YglCleanUpWindow(&level->prg[j]);
          }

          level->prg[j].currentQuad = 0;
        }

        if (level->prg[j].cleanupUniform)
        {
          level->prg[j].matrix = (GLfloat*)dmtx.m;
          level->prg[j].cleanupUniform((void*)&level->prg[j]);
        }

      }
      level->prgcurrent = 0;
      YglTranslatef(&mtx, 0.0f, 0.0f, 0.1f);
    }
    glEnable(GL_BLEND);
    glBlendFunc(blendfunc_src, blendfunc_dst);
    if (Vdp1External.disptoggle & 0x01) { VITA_GPU_STAGE(3); YglRenderFrameBuffer(from, 8); VITA_GPU_STAGE(6); }
  }

   if ((fixVdp2Regs->SDCTL & 0xFF) != 0 || _Ygl->msb_shadow_count_[_Ygl->readframe] != 0 ) {
     VITA_GPU_STAGE(3); YglRenderFrameBufferShadow(); VITA_GPU_STAGE(7);
   }

  
  VITA_GPU_STAGE(3);
  if (_Ygl->aamode == AA_FXAA){
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    glScissor(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    _Ygl->targetfbo = 0;
    YglBlitFXAA(_Ygl->fxaa_fbotex, GlWidth, GlHeight);
  }
  else if (_Ygl->aamode == AA_SCANLINE_FILTER && _Ygl->rheight <= 256 ){
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);

    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    glScissor(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    YglBlitScanlineFilter(_Ygl->fxaa_fbotex, GlHeight, _Ygl->rheight);
  }
  else if (_Ygl->resolution_mode != RES_NATIVE ) {
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);

    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    glScissor(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
    if (!ABL(1)) YglBlitFramebuffer(_Ygl->fxaa_fbotex, _Ygl->default_fbo, GlWidth, GlHeight);
  }
  else{
    
  }
render_finish:
  glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
  glUseProgram(0);
#ifndef YABAUSE_VITAGL
  glGetError();
#endif
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  YglUnbindPixelUnpack();
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  OSDDisplayMessages(NULL,0,0);
#ifdef VITA_SKIP_TRANSPARENT_CELLS
  {
    extern unsigned vita_transparent_cells_skipped, vita_transparent_cells_decoded;
    static unsigned frames;
    if ((++frames & 127) == 0) {
      YuiMsg("transparent_cells frames=128 skipped=%u decoded=%u",
             vita_transparent_cells_skipped, vita_transparent_cells_decoded);
      vita_transparent_cells_skipped = vita_transparent_cells_decoded = 0;
#ifdef VITA_DIAG_CELL_ALPHA
      extern unsigned vita_cell_alpha_counts[4];
      YuiMsg("cell_alpha frames=128 zero=%u nonzero=%u mixed=%u nonzero_unblended=%u diagnostic_timing=1",
          vita_cell_alpha_counts[0],vita_cell_alpha_counts[1],vita_cell_alpha_counts[2],vita_cell_alpha_counts[3]);
      memset(vita_cell_alpha_counts,0,sizeof(unsigned)*4);
#endif
    }
  }
#endif
  VITA_GPU_STAGE(4);
  YuiSwapBuffers();
  FrameProfileAdd("YglRender end");
  return;
}


int YglSetupWindow(YglProgram * prg){

  int bwin_cc0 = (Vdp2Regs->WCTLD >> 9) & 0x01;
  int logwin_cc0 = (Vdp2Regs->WCTLD >> 8) & 0x01;
  int bwin_cc1 = (Vdp2Regs->WCTLD >> 11) & 0x01;
  int logwin_cc1 = (Vdp2Regs->WCTLD >> 10) & 0x01;
  int winmode_cc = (Vdp2Regs->WCTLD >> 15) & 0x01;

  /*
    ToDo: 
     When both Color Calculation window and Transparent Window is enabled,
       Only 'AND' condition pixel need to be drawn in this function.
  */

  glEnable(GL_STENCIL_TEST);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

  // Stencil Value
  // no window = 0
  // win0      = 1
  // win1      = 2
  // SP        = 4
  // both		   = 3

  if (prg->bwin0 == 0 && prg->bwin1 == 0) {
    // Color Clcuaraion Window
    if (bwin_cc0 && !bwin_cc1)
    {
      // Win0
      if (logwin_cc0)
      {
        glStencilFunc(GL_EQUAL, 0x01, 0x01);
      }
      else {
        glStencilFunc(GL_NOTEQUAL, 0x01, 0x01);
      }
      return 0;
    }
    else if (!bwin_cc0 && bwin_cc1)
    {
      if (logwin_cc1)
      {
        glStencilFunc(GL_EQUAL, 0x02, 0x02);
      }
      else {
        glStencilFunc(GL_NOTEQUAL, 0x02, 0x02);
      }
      return 0;
    }
    else if (bwin_cc0 && bwin_cc1) {
      // and
      if (winmode_cc == 0x0)
      {
        if (logwin_cc0 == 1 && logwin_cc1 == 1) {
          glStencilFunc(GL_EQUAL, 0x03, 0x03);
        }
        else if (logwin_cc0 == 0 && logwin_cc1 == 0) {
          glStencilFunc(GL_GREATER, 0x01, 0x03);
        }
        else {
          glStencilFunc(GL_ALWAYS, 0, 0xFF);
        }
      }
      // OR
      else if (winmode_cc == 0x01)
      {
        if (logwin_cc0 == 1 && logwin_cc1 == 1) {
          glStencilFunc(GL_LEQUAL, 0x01, 0x03);
        }
        else if (logwin_cc0 == 0 && logwin_cc1 == 0) {
          glStencilFunc(GL_NOTEQUAL, 0x03, 0x03);
        }
        else {
          glStencilFunc(GL_ALWAYS, 0, 0xFF);
        }
      }
      return 0;
    }
  }

  // Transparent Window
  if (prg->bwin0 || prg->bwin1 || prg->bwinsp)
  {
    u8 bwin1 = prg->bwin1 << 1;
    u8 logwin1 = prg->logwin1 << 1;
    u8 bwinsp = prg->bwinsp << 2;
    u8 logwinsp = prg->logwinsp << 2;

    int winmask = (prg->bwin0 | bwin1 | bwinsp);
    int winflag = 0;
    if (prg->winmode == 0) { // and
      if (prg->bwin0)  winflag = prg->logwin0;
      if (prg->bwin1)  winflag |= logwin1;
      if (prg->bwinsp) winflag |= logwinsp;
      glStencilFunc(GL_EQUAL, winflag, winmask);
    }
    else { // or
      winflag = winmask;
      if (prg->bwin0)  winflag &= ~prg->logwin0;
      if (prg->bwin1)  winflag &= ~logwin1;
      if (prg->bwinsp) winflag &= ~logwinsp;
      glStencilFunc(GL_NOTEQUAL, winflag, winmask);
    }
  }
  return 0;
}

int YglCleanUpWindow(YglProgram * prg){

  int bwin_cc0 = (Vdp2Regs->WCTLD >> 9) & 0x01;
  int logwin_cc0 = (Vdp2Regs->WCTLD >> 8) & 0x01;
  int bwin_cc1 = (Vdp2Regs->WCTLD >> 11) & 0x01;
  int logwin_cc1 = (Vdp2Regs->WCTLD >> 10) & 0x01;
  int winmode_cc = (Vdp2Regs->WCTLD >> 15) & 0x01;

  if (prg->bwin0 == 0 && prg->bwin1 == 0) {
    if (bwin_cc0 || bwin_cc1) {
      // Disable Color clacuration then draw outside of window
      glDisable(GL_STENCIL_TEST);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_GEQUAL);
      glDisable(GL_BLEND);
      Ygl_setNormalshader(prg);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)prg->quads);
      glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, (GLvoid *)prg->textcoords);
      glDrawArrays(GL_TRIANGLES, 0, prg->currentQuad / 2);
      glDepthFunc(GL_GEQUAL);
      Ygl_cleanupNormal(prg);
      glUseProgram(prg->prg);
    }
  }

  glEnable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 0, 0xFF);

  return 0;
}

void YglRenderDestinationAlpha(void) {
  YglLevel * level;
  GLuint cprg = 0;
  int from = 0;
  int to = 0;
  YglMatrix mtx;
  YglMatrix dmtx;
  unsigned int i, j;
  int highpri = 8;
  int ccwindow;

  glEnable(GL_BLEND);

  YglLoadIdentity(&mtx);

  cprg = -1;

  int blendfunc_src = GL_DST_ALPHA;
  int blendfunc_dst = GL_ONE_MINUS_DST_ALPHA;

  // Color Calcurate Window  
  ccwindow = ((Vdp2Regs->WCTLD >> 9) & 0x01);
  ccwindow |= ((Vdp2Regs->WCTLD >> 11) & 0x01);

  // Find out top prooriy
  // ToDo: this operation need to be per pixel!
  for (i = 0; i < _Ygl->depth; i++)
  {
    level = _Ygl->levels + i;
    if (level->prgcurrent != 0 ){

      for (j = 0; j < (level->prgcurrent + 1); j++){
        if (level->prg[j].blendmode == 1){
          highpri = i;
        }
      }
      
    }
  }

  YglTranslatef(&mtx, 0.0f, 0.0f, -1.0f);
  for (i = 0; i < _Ygl->depth; i++)
  {
    level = _Ygl->levels + i;
    if (level->blendmode != 0)
    {
      to = i;

      if (highpri == i ){
        glEnable(GL_BLEND);
        glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
      }else{
        glDisable(GL_BLEND);
      }
      if (Vdp1External.disptoggle & 0x01) YglRenderFrameBuffer(from, to);
      from = to;

      // clean up
      cprg = -1;
      glUseProgram(0);
      glBindTexture(GL_TEXTURE_2D, YglTM->textureID_in[YglTM->current]);
            YglUnbindPixelUnpack();
    }
    glDisable(GL_STENCIL_TEST);
    for (j = 0; j<(level->prgcurrent + 1); j++)
    {
      if (level->prg[j].prgid != cprg)
      {
        cprg = level->prg[j].prgid;
        glUseProgram(level->prg[j].prg);
      }

      if (level->prg[j].setupUniform)
      {
        level->prg[j].setupUniform((void*)&level->prg[j]);
      }

      YglMatrixMultiply(&dmtx, &mtx, &_Ygl->mtxModelView);

      if (level->prg[j].currentQuad != 0)
      {
        if (level->prg[j].prgid == PG_LINECOLOR_INSERT || 
            level->prg[j].prgid == PG_LINECOLOR_INSERT_CRAM ||
            level->prg[j].prgid == PG_LINECOLOR_INSERT_DESTALPHA ||
            level->prg[j].prgid == PG_LINECOLOR_INSERT_DESTALPHA_CRAM ||
            level->prg[j].prgid == PG_VDP2_PER_LINE_ALPHA ) {
              glDisable(GL_BLEND);
        }
        else{
          if (level->prg[j].blendmode == 0){
            glDisable(GL_BLEND);
          }
          else if (level->prg[j].blendmode == 1){
            glEnable(GL_BLEND);
            glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
          }
          else if (level->prg[j].blendmode == 2){
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
          }
        }

        // workaround for "KURO NO DANSYOU #657"
        if (i != highpri && highpri - 1 != i ){
          glDisable(GL_BLEND);
        }

        if ((level->prg[j].bwin0 != 0 || level->prg[j].bwin1 != 0) || (level->prg[j].blendmode != VDP2_CC_NONE && ccwindow)){
          YglSetupWindow(&level->prg[j]);
        }

        glUniformMatrix4fv(level->prg[j].mtxModelView, 1, GL_FALSE, (GLfloat*)&dmtx.m[0][0]);
        glVertexAttribPointer(level->prg[j].vertexp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)level->prg[j].quads);
        glVertexAttribPointer(level->prg[j].texcoordp, 4, GL_FLOAT, GL_FALSE, 0, (GLvoid *)level->prg[j].textcoords);
        if (level->prg[j].vaid != 0) { glVertexAttribPointer(level->prg[j].vaid, 4, GL_FLOAT, GL_FALSE, 0, level->prg[j].vertexAttribute); }
        glDrawArrays(GL_TRIANGLES, 0, level->prg[j].currentQuad / 2);
        
        if (level->prg[j].bwin0 != 0 || level->prg[j].bwin1 != 0 || (level->prg[j].blendmode != VDP2_CC_NONE && ccwindow)){
          level->prg[j].matrix = (GLfloat*)dmtx.m;
          YglCleanUpWindow(&level->prg[j]);
        }

        level->prg[j].currentQuad = 0;
      }

      if (level->prg[j].cleanupUniform)
      {
        level->prg[j].matrix = (GLfloat*)dmtx.m;
        level->prg[j].cleanupUniform((void*)&level->prg[j]);
      }

    }
    level->prgcurrent = 0;

    YglTranslatef(&mtx, 0.0f, 0.0f, 0.1f);

  }

  for (i = from; i < _Ygl->vdp1_maxpri+1 ; i++){
    if (((Vdp2Regs->CCCTL >> 6) & 0x01) == 0x01){
      switch ((Vdp2Regs->SPCTL >> 12) & 0x3){
      case 0:
        if (i <= ((Vdp2Regs->SPCTL >> 8) & 0x07)){
          glEnable(GL_BLEND);
          glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
        }
        else{
          glDisable(GL_BLEND);
        }
        break;
      case 1:
        if (i == ((Vdp2Regs->SPCTL >> 8) & 0x07)){
          glEnable(GL_BLEND);
          glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
        }
        else{
          glDisable(GL_BLEND);
        }
        break;
      case 2:
        if (i >= ((Vdp2Regs->SPCTL >> 8) & 0x07)){
          glEnable(GL_BLEND);
          glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
        }
        else{
          glDisable(GL_BLEND);
        }
        break;
      case 3:
        // ToDO: MSB color cacuration
        glEnable(GL_BLEND);
        glBlendFuncSeparate(blendfunc_src, blendfunc_dst, GL_ONE, GL_ZERO);
        break;
      }
    }
    if (Vdp1External.disptoggle & 0x01) YglRenderFrameBuffer(i, i+1);
  }

  return;
}


//////////////////////////////////////////////////////////////////////////////

void YglReset(void) {
   YglLevel * level;
   unsigned int i,j;


   for(i = 0;i < (_Ygl->depth+1) ;i++) {
     level = _Ygl->levels + i;
     level->blendmode  = 0;
     level->prgcurrent = 0;
     level->uclipcurrent = 0;
     level->ux1 = 0;
     level->uy1 = 0;
     level->ux2 = 0;
     level->uy2 = 0;
     for( j=0; j< level->prgcount; j++ )
     {
         _Ygl->levels[i].prg[j].currentQuad = 0;
     }
   }
   _Ygl->msglength = 0;
}

//////////////////////////////////////////////////////////////////////////////

void YglShowTexture(void) {
   _Ygl->st = !_Ygl->st;
}

#ifdef VITA_PALETTE_UPLOAD_REUSE
#include "palette_upload.h"
static YglPaletteUpload vita_palette_upload;
static unsigned vita_palette_updates,vita_palette_reuses,vita_palette_upload_bytes;
#endif
u32 * YglGetColorRamPointer() {
  int error;
  if (_Ygl->cram_tex == 0) {
#ifdef VITA_PALETTE_UPLOAD_REUSE
    vita_palette_upload.valid=0; /* Includes recycled texture IDs/context rebuilds. */
#endif
    glGetError();
    glGenTextures(1, &_Ygl->cram_tex);
#if 0
    glGenBuffers(1, &_Ygl->cram_tex_pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->cram_tex_pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 2048 * 4, NULL, GL_DYNAMIC_DRAW);
#ifndef YABAUSE_VITAGL
    YglUnbindPixelUnpack();
#endif
#endif
#ifndef YABAUSE_VITAGL
    YglUnbindPixelUnpack();
#endif
    glBindTexture(GL_TEXTURE_2D, _Ygl->cram_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2048, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLLOG("Fail to init cram_tex %04X", error);
      return NULL;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    _Ygl->colupd_min_addr = 0xFFFFFFFF ;
    _Ygl->colupd_max_addr = 0x00000000;



  }

  if (_Ygl->cram_tex_buf == NULL) {
#if 0
    glBindTexture(GL_TEXTURE_2D, _Ygl->cram_tex);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->cram_tex_pbo);
    _Ygl->cram_tex_buf = (u32 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, 2048 * 4, GL_MAP_WRITE_BIT /*| GL_MAP_INVALIDATE_BUFFER_BIT*/);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLLOG("Fail to init YglTM->lincolor_buf %04X", error);
      return NULL;
    }
#ifndef YABAUSE_VITAGL
    YglUnbindPixelUnpack();
#endif
#endif
    _Ygl->cram_tex_buf = calloc(2048, sizeof(u32));
    if (!_Ygl->cram_tex_buf) abort();
    glBindTexture(GL_TEXTURE_2D, _Ygl->cram_tex);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2048, 1,GL_RGBA, GL_UNSIGNED_BYTE,_Ygl->cram_tex_buf);
#ifdef VITA_PALETTE_UPLOAD_REUSE
    YglPaletteUploadRemember(&vita_palette_upload,_Ygl->cram_tex_buf,0,2048);
#endif
  }

  return _Ygl->cram_tex_buf;
}

void YglOnUpdateColorRamWord(u32 addr) {

  if (_Ygl == NULL) return;

  YabThreadLock(_Ygl->crammutex);
  Vdp2ColorRamUpdated = 1;

  if (_Ygl->colupd_min_addr > addr)
    _Ygl->colupd_min_addr = addr;
  if (_Ygl->colupd_max_addr < addr)
    _Ygl->colupd_max_addr = addr;

  u32 * buf = _Ygl->cram_tex_buf;
  if (buf == NULL) {
    YabThreadUnLock(_Ygl->crammutex);
    return;
  }
  
  switch (Vdp2Internal.ColorMode)
  {
  case 0:
  case 1:
  {
    u16 tmp;
    u8 alpha = 0;
    tmp = T2ReadWord(Vdp2ColorRam, addr);
    if (tmp & 0x8000) alpha = 0xFF;
    buf[(addr >> 1) & 0x7FF] = SAT2YAB1(alpha, tmp);
    break;
  }
  case 2:
  {
    u32 tmp1 = T2ReadWord(Vdp2ColorRam, (addr&0xFFC));
    u32 tmp2 = T2ReadWord(Vdp2ColorRam, (addr&0xFFC)+2);
    u8 alpha = 0;
    if (tmp1 & 0x8000) alpha = 0xFF;
    buf[(addr >> 2) & 0x7FF] = SAT2YAB2(alpha, tmp1, tmp2);
    break;
  }
  default: 
    break;
  }
  YabThreadUnLock(_Ygl->crammutex);
}


void YglUpdateColorRam() {
  YabThreadLock(_Ygl->crammutex);
  if (Vdp2ColorRamUpdated) {
    Vdp2ColorRamUpdated = 0;
    if (_Ygl->colupd_min_addr > _Ygl->colupd_max_addr) {
      YabThreadUnLock(_Ygl->crammutex);
      return; // !? not initilized?
    }

    u32 * buf = YglGetColorRamPointer();
    int index_shft = 1;
    if (Vdp2Internal.ColorMode == 2) {
      index_shft = 2;
    }
    glBindTexture(GL_TEXTURE_2D, _Ygl->cram_tex);
#ifndef YABAUSE_VITAGL
    YglUnbindPixelUnpack();
#endif
    _Ygl->colupd_min_addr &= 0xFFF;
    _Ygl->colupd_max_addr &= 0xFFF;
    const u32 start_addr = (_Ygl->colupd_min_addr >> index_shft);
    const u32 size = ((_Ygl->colupd_max_addr - _Ygl->colupd_min_addr) >> index_shft) + 1;
#ifdef VITA_PALETTE_UPLOAD_REUSE
    ++vita_palette_updates;
    if(YglPaletteUploadEqual(&vita_palette_upload,buf,start_addr,size)) {
      ++vita_palette_reuses;
    } else {
#endif
#if 0
    glTexSubImage2D(GL_TEXTURE_2D,
      0,
      0, 0,
      2048, 1,
      GL_RGBA, GL_UNSIGNED_BYTE,
      buf);
#else
    glTexSubImage2D(GL_TEXTURE_2D, 
      0, 
      start_addr, 0,
      size, 1,
      GL_RGBA, GL_UNSIGNED_BYTE, 
      &buf[start_addr] );
#endif
#ifdef VITA_PALETTE_UPLOAD_REUSE
      YglPaletteUploadRemember(&vita_palette_upload,buf,start_addr,size);
      vita_palette_upload_bytes+=size*4;
    }
#endif
    _Ygl->colupd_min_addr = 0xFFFFFFFF;
    _Ygl->colupd_max_addr = 0x00000000;
  }
  YabThreadUnLock(_Ygl->crammutex);
#ifdef VITA_PALETTE_UPLOAD_REUSE
  static unsigned calls;
  if(++calls%128==0) {
    YuiMsg("palette_upload frames=128 requests=%u identical=%u uploaded_bytes=%u",vita_palette_updates,vita_palette_reuses,vita_palette_upload_bytes);
    vita_palette_updates=vita_palette_reuses=vita_palette_upload_bytes=0;
  }
#endif
  return;

}



#ifdef YABAUSE_VITAGL
#include "line_vita.inc"
#else
u32 * YglGetLineColorPointer(){
  int error;
  if (_Ygl->lincolor_tex == 0){
    glGetError();
    glGenTextures(1, &_Ygl->lincolor_tex);

    glGenBuffers(1, &_Ygl->linecolor_pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->linecolor_pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 512 * 4, NULL, GL_DYNAMIC_DRAW);
    YglUnbindPixelUnpack();

    glBindTexture(GL_TEXTURE_2D, _Ygl->lincolor_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLLOG("Fail to init lincolor_tex %04X", error);
      return NULL;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  glBindTexture(GL_TEXTURE_2D, _Ygl->lincolor_tex);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->linecolor_pbo);
  _Ygl->lincolor_buf = (u32 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, 512 * 4, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT  );
  if ((error = glGetError()) != GL_NO_ERROR)
  {
    YGLLOG("Fail to init YglTM->lincolor_buf %04X", error);
    return NULL;
  }
  YglUnbindPixelUnpack();

  return _Ygl->lincolor_buf;
}

void YglSetLineColor(u32 * pbuf, int size){

  glBindTexture(GL_TEXTURE_2D, _Ygl->lincolor_tex);
  //if (_Ygl->lincolor_buf == pbuf) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->linecolor_pbo);
    #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    YglUnbindPixelUnpack();
    _Ygl->lincolor_buf = NULL;
  //}
  glBindTexture(GL_TEXTURE_2D, 0 );
  return;
}

//////////////////////////////////////////////////////////////////////////////
u32* YglGetBackColorPointer() {
  int status;
  GLuint error;

  YGLDEBUG("YglGetBackColorPointer: %d,%d", _Ygl->width, _Ygl->height);


  if (_Ygl->back_tex == 0) {
    glGetError();
    glGenTextures(1, &_Ygl->back_tex);

    glGenBuffers(1, &_Ygl->back_pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->back_pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 512 * 4, NULL, GL_DYNAMIC_DRAW);
    YglUnbindPixelUnpack();

    glBindTexture(GL_TEXTURE_2D, _Ygl->back_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLLOG("Fail to init back_tex %04X", error);
      return NULL;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, _Ygl->back_tex);
#if 0
    if( _Ygl->backcolor_buf == NULL ){
        _Ygl->backcolor_buf = malloc(512 * 4);
    }
#else
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->back_pbo);
  if( _Ygl->backcolor_buf != NULL ){
    #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
  }
  _Ygl->backcolor_buf = (u32 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, 512 * 4, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT );
  if ((error = glGetError()) != GL_NO_ERROR)
  {
    YGLLOG("Fail to init YglTM->backcolor_buf %04X", error);
    return NULL;
  }
  YglUnbindPixelUnpack();
#endif

  return _Ygl->backcolor_buf;
}

void YglSetBackColor(int size) {

  glBindTexture(GL_TEXTURE_2D, _Ygl->back_tex);
#if 0
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, _Ygl->backcolor_buf);
#else
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _Ygl->back_pbo);
  #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  YglUnbindPixelUnpack();
  _Ygl->backcolor_buf = NULL;
#endif
  glBindTexture(GL_TEXTURE_2D, 0);
  return;
}

#endif /* CPU-owned Vita line/back tables */
void YglRebuildGramebuffer(){
  switch (_Ygl->resolution_mode) {
  case RES_NATIVE:
    _Ygl->width = GlWidth;
    _Ygl->height = GlHeight;
    rebuild_frame_buffer = 1;
    break;
  case RES_4x:
    _Ygl->width = _Ygl->rwidth * 4;
    _Ygl->height = _Ygl->rheight * 4;
    rebuild_frame_buffer = 1;
    break;
  case RES_2x:
    _Ygl->width = _Ygl->rwidth * 2;
    _Ygl->height = _Ygl->rheight * 2;
    rebuild_frame_buffer = 1;
    break;
  case RES_ORIGINAL:
    _Ygl->width = _Ygl->rwidth;
    _Ygl->height = _Ygl->rheight;
    rebuild_frame_buffer = 1;
    break;
  case RES_720P:
    _Ygl->width = 1280;
    _Ygl->height = 720;
    rebuild_frame_buffer = 1;
    break;
  case RES_1080P:
    _Ygl->width = 1920;
    _Ygl->height = 1080;
    rebuild_frame_buffer = 1;
    break;
  }
}
//////////////////////////////////////////////////////////////////////////////

void YglChangeResolution(int w, int h) {
  YglLoadIdentity(&_Ygl->mtxModelView);
  YglOrtho(&_Ygl->mtxModelView, 0.0f, (float)w, (float)h, 0.0f, 10.0f, 0.0f);
  if( _Ygl->rwidth != w || _Ygl->rheight != h ) {
    _Ygl->rwidth = w;
    _Ygl->rheight = h;
    if (_Ygl->CpuWriteFrameBuffer != NULL) {
      free(_Ygl->CpuWriteFrameBuffer);
    }
    _Ygl->CpuWriteFrameBuffer = (u32*)malloc(_Ygl->rwidth * _Ygl->rheight * 4);
    memset(_Ygl->CpuWriteFrameBuffer, 0xFF, _Ygl->rwidth * _Ygl->rheight * 4);

       YGLDEBUG("YglChangeResolution %d,%d\n",w,h);
       if (_Ygl->smallfbo != 0) {
         glDeleteFramebuffers(1, &_Ygl->smallfbo);
         _Ygl->smallfbo = 0;
         glDeleteTextures(1, &_Ygl->smallfbotex);
         _Ygl->smallfbotex = 0;
         glDeleteBuffers(1, &_Ygl->vdp1pixelBufferID);
         _Ygl->vdp1pixelBufferID = 0;
         #ifdef YABAUSE_VITAGL
    free(_Ygl->pFrameBuffer);
#endif
    _Ygl->pFrameBuffer = NULL;
       }

     if (_Ygl->tmpfbo != 0){
       glDeleteFramebuffers(1, &_Ygl->tmpfbo);
       _Ygl->tmpfbo = 0;
       glDeleteTextures(1, &_Ygl->tmpfbotex);
       _Ygl->tmpfbotex = 0;
     }

    switch (_Ygl->resolution_mode) {
    case RES_NATIVE:
      _Ygl->width = GlWidth;
      _Ygl->height = GlHeight;
      rebuild_frame_buffer = 1;
      break;
    case RES_4x:
      _Ygl->width = w * 4;
      _Ygl->height = h * 4;
      rebuild_frame_buffer = 1;
      break;
    case RES_2x:
      _Ygl->width = w * 2;
      _Ygl->height = h * 2;
      rebuild_frame_buffer = 1;
      break;
    case RES_ORIGINAL:
      _Ygl->width = w;
      _Ygl->height = h;
      rebuild_frame_buffer = 1;
      break;
     case RES_720P:
      _Ygl->width = 1280;
      _Ygl->height = 720;
      rebuild_frame_buffer = 1;
      break;
      case RES_1080P:
      _Ygl->width = 1920;
      _Ygl->height = 1080;
      rebuild_frame_buffer = 1;
      break;
    }
  }
  if (_Ygl->rotate_screen && _Ygl->resolution_mode == RES_NATIVE) {
    YglRotatef(&_Ygl->mtxModelView, 90.0, 0.0, 0.0, 1.0f);
  }
}

void YglSetDensity(int d) {
  _Ygl->density = d;
}

//////////////////////////////////////////////////////////////////////////////

void YglOnScreenDebugMessage(char *string, ...) {
   va_list arglist;

   va_start(arglist, string);
   vsprintf(_Ygl->message, string, arglist);
   va_end(arglist);
   _Ygl->msglength = (int)strlen(_Ygl->message);
}

void VIDOGLSync(){
  //YglTmPull(YglTM_vdp1);
  YglTmPull(YglTM, 0);
  _Ygl->texture_manager = NULL;
  if (_Ygl->rbg_use_compute_shader) RBGGenerator_onFinish();
  //glFinish();
  //if (_Ygl->frame_sync != 0) {
  //  glDeleteSync(_Ygl->frame_sync);
  //  _Ygl->frame_sync = 0;
  //}
  //glFinish();
  //_Ygl->frame_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

///////////////////////////////////////////////////////////////////////////////
// Per line operation
#ifndef YABAUSE_VITAGL
u32 * YglGetPerlineBuf(YglPerLineInfo * perline, int linecount, int depth ){
  int error;
  glGetError();
  if (perline->lincolor_tex == 0){
    glGetError();
    glGenTextures(1, &perline->lincolor_tex);

    glGenBuffers(1, &perline->linecolor_pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, perline->linecolor_pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 512 * 4 * depth, NULL, GL_DYNAMIC_DRAW);
    YglUnbindPixelUnpack();

    glBindTexture(GL_TEXTURE_2D, perline->lincolor_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if ((error = glGetError()) != GL_NO_ERROR)
    {
      YGLLOG("Fail to init lincolor_tex %04X", error);
      return NULL;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  }

  glBindTexture(GL_TEXTURE_2D, perline->lincolor_tex);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, perline->linecolor_pbo);
  perline->lincolor_buf = (u32 *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, linecount * 4 * depth, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT  );
  if ((error = glGetError()) != GL_NO_ERROR)
  {
    YGLLOG("Fail to init YglTM->lincolor_buf %04X", error);
    return NULL;
  }
  YglUnbindPixelUnpack();

  return perline->lincolor_buf;
}

void YglSetPerlineBuf(YglPerLineInfo * perline, u32 * pbuf, int linecount, int depth){

  glBindTexture(GL_TEXTURE_2D, perline->lincolor_tex);
  //if (_Ygl->lincolor_buf == pbuf) {
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, perline->linecolor_pbo);
  #ifndef YABAUSE_VITAGL
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
#endif
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, linecount, depth, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  YglUnbindPixelUnpack();
  perline->lincolor_buf = NULL;
  //}
  glBindTexture(GL_TEXTURE_2D, 0);
  return;
}
#endif /* Desktop per-line PBOs */
