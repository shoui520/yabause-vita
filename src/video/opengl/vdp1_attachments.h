/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
/* Caller has bound fbo. VDP1 color backing changes only on rebuild, which
 * must force reattachment even if GL recycles both numeric object names. */
static inline void YglVdp1AttachColor(GLuint fbo, GLuint texture, int rebuild) {
#ifdef VITA_VDP1_RETAIN_COLOR
  static GLuint last_fbo, last_texture;
  static int valid;
  int reuse = valid && !rebuild && last_fbo == fbo && last_texture == texture;
#ifdef YABAUSE_VITAGL
  static unsigned checks, hits;
  ++checks; hits += reuse;
  if ((checks & 255) == 0)
    YuiMsg("vdp1_color_attachment checks=%u hits=%u", checks, hits);
#endif
  if (reuse) return;
  last_fbo = fbo; last_texture = texture; valid = 1;
#else
  (void)fbo; (void)rebuild;
#endif
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
}
/* Both VDP1 color textures have identical dimensions and share one FBO's
 * depth/stencil attachment. Rebuild is the only renderbuffer identity change.
 * Reattaching on vitaGL retires hidden backing even when identity is unchanged.
 * Keep real backing and STORE_DEPTH_STENCIL; never bypass partial-render safety. */
static inline void YglVdp1AttachDepth(GLuint depth, GLuint stencil, int rebuild) {
#ifdef VITA_VDP1_RETAIN_ATTACHMENTS
  if (!rebuild) return;
#else
  (void)rebuild;
#endif
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,stencil);
}
