/* SPDX-License-Identifier: GPL-2.0-or-later
 * Included by ygles.c instead of the desktop PBO texture manager.
 * CPU staging never aliases GPU texture storage. Keep vitaGL's checked texture
 * update/deferred-free paths enabled (no TEXTURES_SPEEDHACK).
 */
#ifdef YABAUSE_VITAGL
#include "rotation_gpu.h"
#endif
/* Bitmap reuse (vidogl.c): which atlas frame (bumped at every reset) and
 * which staging storage (bumped at every allocation/realloc) texels were
 * last written in. */
unsigned vita_atlas_frame, vita_atlas_gen;
/* The atlas frame whose texels the current storage still holds, if any
 * (UINT_MAX: none). Without the zero-copy ring it is the previous frame. */
unsigned vita_atlas_prev_frame = ~0u;
static unsigned int *YglVitaAtlasStorage(unsigned width, unsigned height) {
  VT_SCOPE(VT_RENDER_ALLOC);
  /* SGX texture dimensions are bounded; avoid overflow and unbounded growth. */
  if (!width || !height || width > 4096 || height > 4096) {
    fprintf(stderr, "Invalid Vita atlas size %u x %u\n", width, height);
    abort();
  }
  unsigned int *pixels = calloc((size_t)width * height, sizeof(*pixels));
  if (!pixels) {
    fprintf(stderr, "Cannot allocate Vita atlas %u x %u\n", width, height);
    abort();
  }
  return pixels;
}

static GLuint YglVitaAtlasTexture(unsigned width, unsigned height) {
  VT_SCOPE(VT_RENDER_ALLOC);
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLenum error = glGetError();
  if (!texture || error != GL_NO_ERROR) {
    fprintf(stderr, "Cannot create Vita atlas %u x %u (GL %x)\n",
            width, height, (unsigned)error);
    abort();
  }
  return texture;
}

YglTextureManager *YglTMInit(unsigned width, unsigned height) {
  ++vita_atlas_gen;
  YglTextureManager *tm = calloc(1, sizeof(*tm));
  if (!tm) abort();
  tm->width = width;
  tm->height = height;
  tm->vita_gpu_height = height;
  glActiveTexture(GL_TEXTURE0);
#ifdef VITA_ATLAS_ZERO_COPY
  {
    ZcState *z = calloc(1, sizeof(ZcState));
    if (!z) abort();
    tm->vita_zc = z;
    z->gpu = -1;
    tm->textureID_in[0] = YglVitaAtlasTexture(width, height);   /* leaves it bound on unit 0 */
    z->orig = vglGetTexDataPointer(GL_TEXTURE_2D);
    ZcAlloc(&z->buf[0], width, height);
    tm->texture = z->buf[0].pixels;
    ZcUse(tm, 0, 1);
  }
#else
  for (int i = 0; i < NUM_TEXTURE_BUFFER; ++i) {
    tm->texture_in[i] = YglVitaAtlasStorage(width, height);
    tm->textureID_in[i] = YglVitaAtlasTexture(width, height);
  }
  tm->texture = tm->texture_in[0];
#endif
  YglGetColorRamPointer();
  return tm;
}

void YglTMDeInit(YglTextureManager *tm) {
  if (!tm) return;
  Vdp2RgbTextureSync();
#ifdef YABAUSE_VITAGL
  YglVitaRotationShutdown();
#endif
#ifdef VITA_ATLAS_ZERO_COPY
  {
    ZcState *z = tm->vita_zc;
    glFinish();
    ZcPointTexture(tm->textureID_in[0], z->orig);
    glDeleteTextures(1, &tm->textureID_in[0]);
    for (int i = 0; i < ZC_MAX; ++i) ZcFree(&z->buf[i]);
    free(z);
  }
#else
  for (int i = 0; i < NUM_TEXTURE_BUFFER; ++i) {
    glDeleteTextures(1, &tm->textureID_in[i]);
    free(tm->texture_in[i]);
  }
#endif
  free(tm->vita_snapshot.pixels);
  free(tm);
}

void YglTMReset(YglTextureManager *tm) {
  ++vita_atlas_frame;
#ifdef YABAUSE_VITAGL
  YglVitaRotationReset();
#endif
  tm->currentX = tm->currentY = tm->yMax = 0;
  memset(&tm->vita_cpu_extent,0,sizeof(tm->vita_cpu_extent));
#ifdef VITA_ATLAS_ZERO_COPY
  if (tm->vita_zc) {
    ZcState *z = tm->vita_zc;
    const int i = ZcAcquire(tm);
    vita_atlas_prev_frame = z->buf[i].atlas_frame;
    z->buf[i].atlas_frame = vita_atlas_frame;
    ZcUse(tm, i, 0);
    return;
  }
#endif
  vita_atlas_prev_frame = vita_atlas_frame - 1;
}

static void YglTmPushInner(YglTextureManager *tm);
void YglTmPush(YglTextureManager *tm) {
  DIAG_T0(t);
  YglTmPushInner(tm);
  DIAG_T1(t, DT_ATLAS);
}
static void YglTmPushInner(YglTextureManager *tm) {
  /* Includes atlas binding/upload and rotation submission setup. Nested
   * rotation input uploads are charged separately, never counted twice. */
  VT_SCOPE(VT_ATLAS_PUSH);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tm->textureID_in[tm->current]);
  if (!tm->texture) return;
  Vdp2RgbTextureSync();
  if (tm->yMax > tm->height) abort();
#ifdef VITA_ATLAS_ZERO_COPY
  {
    /* The texels are already where the GPU reads them. The texture height
     * follows the replacing upload exactly (shaders normalise coordinates by
     * it, and rows past it clamp); with GPU rotation output present that path
     * kept the previous height, as here. */
    ZcState *z = tm->vita_zc;
    if (z->gpu != z->cur) ZcSample(tm, z->cur);
    z->buf[z->cur].used = vgl_framecount;
    if (tm->yMax && !YglVitaRotationWritten()) {
      unsigned upload_height = tm->height;
#ifdef VITA_ATLAS_TRIM
      if (!YglVitaRotationUsesAtlas()) {
        unsigned cpu_height = tm->yMax;
#ifdef VITA_ATLAS_PRIVATE_TRIM
        if (tm->vita_cpu_extent.count)
          cpu_height = YglAtlasExtentHeight(&tm->vita_cpu_extent, YglVitaRotationPrivateRect);
#endif
        upload_height = (cpu_height + 7u) & ~7u;
        if (!upload_height) upload_height = 8;
        if (upload_height > tm->height) upload_height = tm->height;
      }
#endif
      if (upload_height != tm->vita_gpu_height) {
        GLint active = 0, bound = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glBindTexture(GL_TEXTURE_2D, tm->textureID_in[0]);
        sceGxmTextureSetHeight(vglGetGxmTexture(GL_TEXTURE_2D), upload_height);
        glBindTexture(GL_TEXTURE_2D, bound);
        glActiveTexture(active);
        tm->vita_gpu_height = upload_height;
      }
    }
    VITA_GPU_STAGE(0);
    YglVitaRotationFlush(tm);
    VITA_GPU_STAGE(1);
    tm->texture = NULL;
    return;
  }
#endif
#ifdef VITA_ATLAS_CONTENT_REUSE
  /* A GPU writer can change bytes not present in CPU staging. Invalidate
   * before submission, including pushes with no CPU allocations. */
  int snapshot_safe = !YglVitaRotationWritten() && !YglVitaRotationUsesAtlas();
  if (!snapshot_safe) tm->vita_snapshot.height = 0;
#endif
  if (tm->yMax) {
    /* vitaGL copies these rows synchronously into its own texture backing.
     * If the texture was used recently, checked vitaGL renames that backing.
     * We may reuse CPU staging after this call, but never the GPU pointer.
     */
#ifdef VITA_ATLAS_REPLACE
    if (!YglVitaRotationWritten()) {
      /* CPU staging owns all live pixels before any GPU-only rotation draw.
       * Replacing storage avoids vitaGL copying the entire old GPU backing.
       * Once GPU-only pixels exist, preserve the existing partial-update path. */
      unsigned upload_height = tm->height;
#ifdef VITA_ATLAS_TRIM
      /* Queued atlas rendering uses logical-height viewports. Keep full
       * backing for that path; private rotation targets do not use it. */
      if (!YglVitaRotationUsesAtlas()) {
        unsigned cpu_height=tm->yMax;
#ifdef VITA_ATLAS_PRIVATE_TRIM
        /* The producer has joined and composition has selected private target
         * sources. Only those exact allocations may be omitted. No allocation
         * coordinates or CPU fallback storage are changed. */
        if(tm->vita_cpu_extent.count)
          cpu_height=YglAtlasExtentHeight(&tm->vita_cpu_extent,YglVitaRotationPrivateRect);
#endif
        upload_height = (cpu_height + 7u) & ~7u;
        if(!upload_height) upload_height=8;
        if (upload_height > tm->height) upload_height = tm->height;
      }
#endif
#ifdef VITA_ATLAS_CONTENT_REUSE
      ++tm->vita_snapshot.checks;
      int reuse = snapshot_safe && YglAtlasSnapshotMatches(&tm->vita_snapshot,
          tm->width, upload_height, tm->texture);
      tm->vita_snapshot.hits += reuse;
      if (!reuse)
#endif
      {
#ifdef VITA_DIAG_TIMERS
        { static unsigned n; if ((n++ & 63) == 0)
            YuiMsg("diag_atlas width=%u height=%u yMax=%u upload_height=%u curX=%u curY=%u", tm->width, tm->height,
                   tm->yMax, upload_height, tm->currentX, tm->currentY); }
#endif
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tm->width, upload_height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, tm->texture);
        tm->vita_gpu_height = upload_height;
#ifdef VITA_ATLAS_CONTENT_REUSE
        if (snapshot_safe) YglAtlasSnapshotStore(&tm->vita_snapshot,
            tm->width, upload_height, tm->texture);
#endif
      }
#if defined(VITA_ATLAS_CONTENT_REUSE) && defined(YABAUSE_VITAGL)
      if ((tm->vita_snapshot.checks & 127) == 0)
        YuiMsg("atlas_content_reuse checks=%u hits=%u",tm->vita_snapshot.checks,
            tm->vita_snapshot.hits);
#endif
    } else
#endif
    {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tm->width, tm->yMax,
                      GL_RGBA, GL_UNSIGNED_BYTE, tm->texture);
    }
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      fprintf(stderr, "Vita atlas upload failed (GL %x)\n", (unsigned)error);
      abort();
    }
  }
#ifdef YABAUSE_VITAGL
  VITA_GPU_STAGE(0);
  YglVitaRotationFlush(tm);
  VITA_GPU_STAGE(1);
#endif
  tm->texture = NULL;
}

void YglTmPull(YglTextureManager *tm, u32 preserve) {
  /* Both modes can retain CPU contents; preserve=0 permits discarding them,
   * but does not require it. Callers still reset allocation/cache explicitly.
   */
  (void)preserve;
  if (!tm->texture) tm->texture = tm->texture_in[tm->current];
}

void YglTMRealloc(YglTextureManager *tm, unsigned width, unsigned height) {
  ++vita_atlas_gen;
  VT_SCOPE(VT_RENDER_ALLOC);
#ifdef VITA_DIAG_TIMERS
  YuiMsg("diag_atlas_realloc %ux%u -> %ux%u yMax=%u", tm->width, tm->height, width, height, tm->yMax);
#endif
  Vdp2RgbTextureSync();
#ifdef VITA_ATLAS_ZERO_COPY
  {
    ZcState *z = tm->vita_zc;
    ZcBuf *old = &z->buf[z->cur];
    /* GPU rotation output may be queued into the current buffer. */
    if (YglVitaRotationWritten()) glFinish();
    old->used = vgl_framecount; old->ever_used = 1;
    const unsigned old_w = tm->width, old_h = tm->height;
    tm->width = width; tm->height = height;
    const int i = ZcAcquire(tm);   /* frees idle buffers of the old size */
    ZcBuf *b = &z->buf[i];
    old = &z->buf[z->cur];
    const unsigned rows = height < old_h ? height : old_h;
    const unsigned cols = width < old_w ? width : old_w;
    for (unsigned y = 0; y < rows; ++y)
      memcpy(b->pixels + (size_t)y * width, old->pixels + (size_t)y * old_w, (size_t)cols * 4u);
    /* Resize the texture on its own backing, then adopt the new buffer. */
    ZcPointTexture(tm->textureID_in[0], z->orig);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tm->textureID_in[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "Cannot resize Vita atlas\n"); abort(); }
    z->orig = vglGetTexDataPointer(GL_TEXTURE_2D);
    b->atlas_frame = vita_atlas_frame;
    tm->texture = b->pixels;
    ZcUse(tm, i, 1);
    tm->vita_gpu_height = height;
    tm->vita_snapshot.height = 0;
    return;
  }
#endif
  /* Allocate/copy before replacing anything; a width change needs row-wise
   * copies, not the desktop path's flat old-width byte copy.
   */
  unsigned int *pixels = YglVitaAtlasStorage(width, height);
  unsigned rows = height < tm->height ? height : tm->height;
  unsigned cols = width < tm->width ? width : tm->width;
  for (unsigned y = 0; y < rows; ++y)
    memcpy(pixels + (size_t)y * width,
           tm->texture_in[tm->current] + (size_t)y * tm->width,
           (size_t)cols * sizeof(*pixels));
  glActiveTexture(GL_TEXTURE0);
  GLuint texture = YglVitaAtlasTexture(width, height);
  /* NUM_TEXTURE_BUFFER is one in this inherited renderer. Do not silently
   * duplicate this ownership policy if upstream adds multiple CPU slots.
   */
  _Static_assert(NUM_TEXTURE_BUFFER == 1, "Review Vita atlas slot ownership");
  glDeleteTextures(1, &tm->textureID_in[0]);
  free(tm->texture_in[0]);
  tm->textureID_in[0] = texture;
  tm->texture_in[0] = pixels;
  tm->width = width;
  tm->height = height;
  tm->vita_gpu_height = height;
  tm->vita_snapshot.height = 0;
  tm->texture = pixels;
}
