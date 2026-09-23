/* SPDX-License-Identifier: GPL-2.0-or-later
 * Snapshot for VDP1 destination-dependent commands. Sampling the texture
 * currently attached for writing is not a valid replacement for a GL barrier.
 * Source and destination never alias. No CPU readback is involved.
 */
GLuint YglVitaSnapshotVdp1(void) {
  const int width = _Ygl->width, height = _Ygl->height;
  GLint draw_fbo, read_fbo;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
  if (!_Ygl->vita_feedback_fbo || width != _Ygl->vita_feedback_width ||
      height != _Ygl->vita_feedback_height) {
    GLuint texture = YglVitaAtlasTexture(width, height);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      abort();
    glDeleteFramebuffers(1, &_Ygl->vita_feedback_fbo);
    glDeleteTextures(1, &_Ygl->vita_feedback_tex);
    _Ygl->vita_feedback_fbo = fbo;
    _Ygl->vita_feedback_tex = texture;
    _Ygl->vita_feedback_width = width;
    _Ygl->vita_feedback_height = height;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, _Ygl->vdp1fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _Ygl->vita_feedback_fbo);
  glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
  if (glGetError() != GL_NO_ERROR) {
    fprintf(stderr, "Vita VDP1 feedback snapshot failed\n");
    abort();
  }
  return _Ygl->vita_feedback_tex;
}
