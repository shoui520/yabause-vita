/* SPDX-License-Identifier: GPL-2.0-or-later
 * Execute the production CPU atlas with a copying GL test double. This checks
 * host ownership/row preservation, NOT GPU retirement or shader correctness.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* This GL ownership fixture does not link the Vita telemetry runtime. */
#define VT_SCOPE(phase) ((void)0)
typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLint;
typedef uint32_t u32;
enum { GL_TEXTURE0, GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE,
       GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
       GL_TEXTURE_WRAP_T, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_NO_ERROR,
       GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER, GL_FRAMEBUFFER,
       GL_READ_FRAMEBUFFER_BINDING, GL_DRAW_FRAMEBUFFER_BINDING,
       GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_COMPLETE, GL_COLOR_BUFFER_BIT };
#define NUM_TEXTURE_BUFFER 1
#include "../src/video/opengl/atlas_extent.h"
#include "../src/video/opengl/atlas_snapshot.h"
#include "../src/video/opengl/atlas_snapshot_test.h"
typedef struct {
  YglAtlasExtent vita_cpu_extent;
  YglAtlasSnapshot vita_snapshot;
  unsigned currentX, currentY, yMax, width, height;
  unsigned vita_gpu_height;
  unsigned *texture, *texture_in[2];
  GLuint textureID_in[2], pixelBufferID_in[2];
  int current;
} YglTextureManager;
static struct { unsigned w, h; unsigned *pixels; } gpu[32];
static unsigned next_id, bound, uploads, syncs;
#ifdef VITA_ATLAS_REPLACE
static int rotation_written;
static unsigned full_uploads;
static int YglVitaRotationWritten(void) { return rotation_written; }
#endif
#ifdef VITA_ATLAS_TRIM
static int rotation_pending_atlas;
static int YglVitaRotationUsesAtlas(void) { return rotation_pending_atlas; }
#endif
#ifdef VITA_ATLAS_PRIVATE_TRIM
static int private_target;
static int YglVitaRotationPrivateRect(unsigned x,unsigned y,unsigned w,unsigned h) {
  return private_target && x==0 && y==8 && w==16 && h==4;
}
#endif
static void glActiveTexture(GLenum unit) { assert(unit == GL_TEXTURE0); }
static void glGenTextures(int count, GLuint *id) {
  assert(count == 1 && next_id < 31); *id = ++next_id;
}
static void glBindTexture(GLenum target, GLuint id) {
  assert(target == GL_TEXTURE_2D && id <= next_id); bound = id;
}
static void glTexImage2D(GLenum target, int level, GLenum internal,
    unsigned w, unsigned h, int border, GLenum format, GLenum type, void *data) {
  assert(target == GL_TEXTURE_2D && !level && !border);
  assert(internal == GL_RGBA && format == GL_RGBA && type == GL_UNSIGNED_BYTE);
  free(gpu[bound].pixels);
  gpu[bound].w = w; gpu[bound].h = h;
  gpu[bound].pixels = calloc((size_t)w * h, 4);
  assert(gpu[bound].pixels);
  if (data) {
    memcpy(gpu[bound].pixels, data, (size_t)w*h*4);
    ++uploads;
#ifdef VITA_ATLAS_REPLACE
    ++full_uploads;
#endif
  }
}
static void glTexParameteri(GLenum target, GLenum name, GLenum value) {
  assert(target == GL_TEXTURE_2D);
  assert(value == ((name == GL_TEXTURE_MIN_FILTER || name == GL_TEXTURE_MAG_FILTER)
                  ? GL_NEAREST : GL_CLAMP_TO_EDGE));
}
static GLenum glGetError(void) { return GL_NO_ERROR; }
static void glDeleteTextures(int count, const GLuint *id) {
  assert(count == 1); free(gpu[*id].pixels); gpu[*id].pixels = NULL;
}
static void glTexSubImage2D(GLenum target, int level, int x, int y,
    unsigned w, unsigned h, GLenum format, GLenum type, const void *data) {
  assert(target == GL_TEXTURE_2D && !level && !x && !y);
  assert(format == GL_RGBA && type == GL_UNSIGNED_BYTE);
  assert(w <= gpu[bound].w && h <= gpu[bound].h);
  for (unsigned row = 0; row < h; ++row)
    memcpy(gpu[bound].pixels + row * gpu[bound].w,
           (const unsigned *)data + row * w, (size_t)w * 4);
  ++uploads;
}
static void Vdp2RgbTextureSync(void) { ++syncs; }
static u32 *YglGetColorRamPointer(void) { return NULL; }
#include "../src/video/opengl/atlas_vita.inc"
typedef struct {
  GLuint lincolor_tex, linecolor_pbo;
  u32 *lincolor_buf;
  int vita_depth;
} YglPerLineInfo;
static struct {
  u32 *lincolor_buf, *backcolor_buf;
  GLuint lincolor_tex, back_tex;
  GLuint vdp1fbo, vita_feedback_fbo, vita_feedback_tex;
  int width, height, vita_feedback_width, vita_feedback_height;
} line_state, *_Ygl = &line_state;
#include "../src/video/opengl/line_vita.inc"
static GLuint fb_texture[16], next_fb, read_fb, draw_fb;
static void glGetIntegerv(GLenum name, GLint *value) {
  assert(name == GL_READ_FRAMEBUFFER_BINDING || name == GL_DRAW_FRAMEBUFFER_BINDING);
  *value = name == GL_READ_FRAMEBUFFER_BINDING ? read_fb : draw_fb;
}
static void glGenFramebuffers(int count, GLuint *fb) {
  assert(count == 1 && next_fb < 15); *fb = ++next_fb;
}
static void glBindFramebuffer(GLenum target, GLuint fb) {
  assert(fb <= next_fb);
  if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) read_fb = fb;
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) draw_fb = fb;
}
static void glFramebufferTexture2D(GLenum target, GLenum attachment,
                                 GLenum kind, GLuint tex, int level) {
  assert(target == GL_FRAMEBUFFER && attachment == GL_COLOR_ATTACHMENT0);
  assert(kind == GL_TEXTURE_2D && !level); fb_texture[draw_fb] = tex;
}
static GLenum glCheckFramebufferStatus(GLenum target) {
  assert(target == GL_FRAMEBUFFER && fb_texture[draw_fb]);
  return GL_FRAMEBUFFER_COMPLETE;
}
static void glDeleteFramebuffers(int count, const GLuint *fb) {
  assert(count == 1); fb_texture[*fb] = 0;
}
static void glBlitFramebuffer(int x0, int y0, int x1, int y1,
    int dx0, int dy0, int dx1, int dy1, GLenum mask, GLenum filter) {
  assert(!x0 && !y0 && !dx0 && !dy0 && x1 == dx1 && y1 == dy1);
  assert(mask == GL_COLOR_BUFFER_BIT && filter == GL_NEAREST);
  GLuint src = fb_texture[read_fb], dst = fb_texture[draw_fb];
  assert(src && dst && src != dst);
  assert((unsigned)x1 == gpu[src].w && (unsigned)y1 == gpu[src].h);
  assert(gpu[src].w == gpu[dst].w && gpu[src].h == gpu[dst].h);
  memcpy(gpu[dst].pixels, gpu[src].pixels, (size_t)x1 * y1 * 4);
}
#include "../src/video/opengl/feedback_vita.inc"

#ifdef VITA_ATLAS_CONTENT_REUSE
static void content_reuse_test(void) {
  YglTextureManager *tm = YglTMInit(16,16);
  unsigned before = uploads;
  tm->yMax=16;
  for(unsigned i=0;i<256;++i) tm->texture[i]=i+1;
  YglTmPush(tm); assert(uploads==before+1);
  unsigned id=tm->textureID_in[0];
  YglTmPull(tm,0); YglTmPush(tm); assert(uploads==before+1);
  YglTmPull(tm,0); tm->yMax=8; tm->texture[255]=999;
  YglTmPush(tm);
  assert(uploads==before+1 && tm->vita_gpu_height==16 && gpu[id].h==16);
  assert(gpu[id].pixels[255]==256); /* Tail is not consumed by shorter upload. */
  YglTmPull(tm,0); tm->yMax=16; YglTmPush(tm);
  assert(uploads==before+2 && gpu[id].pixels[255]==999);
  YglTmPull(tm,0); tm->texture[0]=777; YglTmPush(tm);
  assert(uploads==before+3 && gpu[id].pixels[0]==777);
  rotation_written=1; gpu[id].pixels[255]=888;
  YglTmPull(tm,0); tm->yMax=8; YglTmPush(tm);
  assert(!tm->vita_snapshot.height && gpu[id].pixels[255]==888);
  rotation_written=0;
  YglTmPull(tm,0); tm->yMax=16; YglTmPush(tm);
  assert(gpu[id].pixels[255]==999 && tm->vita_snapshot.height==16);
  rotation_pending_atlas=1;
  YglTmPull(tm,0); tm->yMax=0; YglTmPush(tm);
  assert(!tm->vita_snapshot.height); /* GPU-only push invalidates too. */
  rotation_pending_atlas=0;
  YglTmPull(tm,0); tm->yMax=16; YglTmPush(tm);
  YglTmPull(tm,0); YglTMRealloc(tm,16,16);
  assert(!tm->vita_snapshot.height);
  before=uploads;
  YglTmPush(tm); assert(uploads==before+1);
  YglTMDeInit(tm);
  puts("atlas content reuse: repeat, prefix, mutation, GPU writes and realloc passed");
}
#endif

int main(void) {
  unsigned equality_cases=0;
  assert(YglAtlasEqualityTest(&equality_cases)==0 && equality_cases>10000);
#ifdef VITA_ATLAS_CONTENT_REUSE
  content_reuse_test();
#endif
  uploads=0;
#ifdef VITA_ATLAS_REPLACE
  full_uploads=0;
#endif
  YglTextureManager *tm = YglTMInit(8, 8);
  assert(tm->texture && !tm->current && !tm->pixelBufferID_in[0]);
  for (unsigned i = 0; i < 64; ++i) assert(!tm->texture[i]);
  /* An empty push changes producer visibility without issuing a zero upload. */
  YglTmPush(tm); assert(!uploads && !tm->texture);
  YglTmPull(tm, 0);
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 8; ++x) tm->texture[y * 8 + x] = y * 100 + x;
  tm->yMax = 3;
  YglTmPush(tm); assert(uploads == 1 && !tm->texture);
  YglTmPush(tm); assert(uploads == 1);
  unsigned id = tm->textureID_in[0];
  assert(gpu[id].pixels[16] == 200);
#ifdef VITA_ATLAS_REPLACE
  assert(gpu[id].pixels[24] == 300 && full_uploads == 1);
#else
  assert(gpu[id].pixels[24] == 0);
#endif
  YglTmPull(tm, 1);
  tm->texture[0] = 0xaabbccdd;
  assert(gpu[id].pixels[0] == 0); /* No alias with the upload destination. */
  YglTMRealloc(tm, 16, 12);
  assert(!gpu[id].pixels && tm->textureID_in[0] != id);
  for (unsigned y = 0; y < 12; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      unsigned expected = y < 8 && x < 8 ? y * 100 + x : 0;
      if (!x && !y) expected = 0xaabbccdd;
      assert(tm->texture[y * 16 + x] == expected);
    }
  tm->yMax = 8;
#ifdef VITA_ATLAS_TRIM
  rotation_pending_atlas = 1; /* Subsequent GPU-only pixels require full size. */
#endif
  YglTmPush(tm); assert(uploads == 2);
  id = tm->textureID_in[0];
  assert(gpu[id].pixels[16] == 100 && gpu[id].pixels[8] == 0);
  YglTmPull(tm, 0); /* Discard permission does not destroy preserved contents. */
  assert(tm->texture[16] == 100);
#ifdef VITA_ATLAS_REPLACE
  assert(full_uploads == 2);
  rotation_written = 1;
  /* A GPU-only texel outside the modified rows must survive the next push. */
  gpu[id].pixels[16*11] = 0x87654321;
  tm->yMax = 1; tm->texture[0] = 0x12345678;
  YglTmPush(tm);
  assert(full_uploads == 2 && uploads == 3);
  assert(gpu[id].pixels[0] == 0x12345678);
  assert(gpu[id].pixels[16*11] == 0x87654321);
  rotation_written = 0;
#ifdef VITA_ATLAS_TRIM
  rotation_pending_atlas = 0;
  YglTmPull(tm, 0); tm->yMax = 3; YglTmPush(tm);
  assert(tm->height == 12 && tm->vita_gpu_height == 8 && gpu[id].h == 8);
  assert(gpu[id].pixels[16] == 100);
  rotation_pending_atlas = 1;
  YglTmPull(tm, 0); YglTmPush(tm);
  assert(tm->vita_gpu_height == 12 && gpu[id].h == 12);
  rotation_pending_atlas = 0;
  YglTmPull(tm, 0); tm->yMax = 12; YglTmPush(tm);
  assert(tm->vita_gpu_height == 12 && gpu[id].h == 12);
  assert(gpu[id].pixels[16*7] == 700);
#ifdef VITA_ATLAS_PRIVATE_TRIM
  YglAtlasExtentAdd(&tm->vita_cpu_extent,0,0,16,8);
  YglAtlasExtentAdd(&tm->vita_cpu_extent,0,8,16,4);
  private_target=1;
  YglTmPull(tm,0); YglTmPush(tm);
  assert(tm->yMax==12 && tm->height==12);
#ifdef VITA_ATLAS_CONTENT_REUSE
  assert(gpu[id].h==12 && tm->vita_gpu_height==12);
#else
  assert(gpu[id].h==8);
#endif
  assert(gpu[id].pixels[16*7]==700);
  /* CPU fallback for the identical rectangle must retain its rows. */
  private_target=0;
  YglTmPull(tm,0); YglTmPush(tm);
  assert(gpu[id].h==12);
  /* A later ordinary allocation tied with an excluded bottom still wins. */
  YglAtlasExtentAdd(&tm->vita_cpu_extent,8,8,8,4);
  private_target=1;
  YglTmPull(tm,0); YglTmPush(tm);
  assert(gpu[id].h==12);
#endif
#endif
#endif
  YglTMReset(tm); assert(!tm->yMax && !tm->currentX && !tm->currentY);
  assert(!tm->vita_cpu_extent.count);
  YglTMDeInit(tm); assert(!gpu[id].pixels && syncs >= 4);
  YglTMDeInit(NULL);
  u32 *line = YglGetLineColorPointer();
  line[0] = 0x12345678; line[1] = 0xdeadbeef;
  YglSetLineColor(line, 2);
  assert(gpu[_Ygl->lincolor_tex].pixels[1] == 0xdeadbeef);
  line[1] = 0;
  assert(gpu[_Ygl->lincolor_tex].pixels[1] == 0xdeadbeef);
  assert(YglGetLineColorPointer() == line);
  u32 *back = YglGetBackColorPointer();
  back[0] = 0x81abcdef;
  YglSetBackColor(1);
  assert(gpu[_Ygl->back_tex].pixels[0] == 0x81abcdef);
  YglPerLineInfo perline = {0};
  line = YglGetPerlineBuf(&perline, 3, 2);
  for (unsigned i = 0; i < 6; ++i) line[i] = i + 10;
  YglSetPerlineBuf(&perline, line, 3, 2);
  assert(gpu[perline.lincolor_tex].pixels[512] == 13);
  assert(gpu[perline.lincolor_tex].pixels[514] == 15);
  assert(gpu[perline.lincolor_tex].pixels[3] == 0);
  assert(YglGetPerlineBuf(&perline, 512, 2) == line);
  id = perline.lincolor_tex;
  YglGetPerlineBuf(&perline, 512, 1);
  assert(!gpu[id].pixels && perline.vita_depth == 1);
  free(perline.lincolor_buf); glDeleteTextures(1, &perline.lincolor_tex);
  free(_Ygl->lincolor_buf); glDeleteTextures(1, &_Ygl->lincolor_tex);
  free(_Ygl->backcolor_buf); glDeleteTextures(1, &_Ygl->back_tex);
  _Ygl->width = 8; _Ygl->height = 8;
  GLuint source = YglVitaAtlasTexture(8, 8);
  glGenFramebuffers(1, &_Ygl->vdp1fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->vdp1fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  gpu[source].pixels[0] = 0x81aabbcc;
  GLuint snap = YglVitaSnapshotVdp1();
  assert(read_fb == 0 && draw_fb == _Ygl->vdp1fbo);
  assert(gpu[snap].pixels[0] == 0x81aabbcc);
  gpu[source].pixels[0] = 0x12345678;
  assert(gpu[snap].pixels[0] == 0x81aabbcc);
  assert(YglVitaSnapshotVdp1() == snap);
  assert(gpu[snap].pixels[0] == 0x12345678);
  glDeleteTextures(1, &source);
  _Ygl->width = 16;
  source = YglVitaAtlasTexture(16, 8);
  fb_texture[_Ygl->vdp1fbo] = source;
  assert(YglVitaSnapshotVdp1() != snap && !gpu[snap].pixels);
  assert(read_fb == 0 && draw_fb == _Ygl->vdp1fbo);
  glDeleteTextures(1, &source);
  glDeleteTextures(1, &_Ygl->vita_feedback_tex);
  glDeleteFramebuffers(1, &_Ygl->vita_feedback_fbo);
  glDeleteFramebuffers(1, &_Ygl->vdp1fbo);
  puts("vitaGL CPU atlas ownership, bounded upload and row-preserving growth: passed");
  puts("vitaGL line/back/per-line packed uploads, reuse and depth resize: passed");
  puts("vitaGL feedback snapshot nonaliasing, refresh, bindings and resize: passed");
}
