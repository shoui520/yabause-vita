/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native uniform locations are cached once; use the same CPU register snapshot
 * as the desktop UBO, explicitly unpacking its std140 scalar-array padding. */
typedef struct {
  GLint priority, alpha, offset, condition, emu_height, height, viewport, ram_offset, sprite_window;
} YglVitaFramebufferUniforms;
static YglVitaFramebufferUniforms vita_fb_uniforms[PG_MAX];

static void YglVitaCacheFramebufferUniforms(int id) {
  YglVitaFramebufferUniforms *u = &vita_fb_uniforms[id];
  u->priority = glGetUniformLocation(_prgid[id], "u_pri");
  u->alpha = glGetUniformLocation(_prgid[id], "u_alpha");
  u->offset = glGetUniformLocation(_prgid[id], "u_coloroffset");
  u->condition = glGetUniformLocation(_prgid[id], "u_cctl");
  u->emu_height = glGetUniformLocation(_prgid[id], "u_emu_height");
  u->height = glGetUniformLocation(_prgid[id], "u_vheight");
  u->viewport = glGetUniformLocation(_prgid[id], "u_viewport_offset");
  u->ram_offset = glGetUniformLocation(_prgid[id], "u_color_ram_offset");
  u->sprite_window = glGetUniformLocation(_prgid[id], "u_sprite_window");
}

static void YglVitaBindFramebufferUniforms(int id) {
  const YglVitaFramebufferUniforms *u = &vita_fb_uniforms[id];
  const UniformFrameBuffer *f = &_Ygl->fbu_;
  float priority[8], alpha[8];
  for (unsigned i = 0; i < 8; ++i) {
    priority[i] = f->u_pri[i * 4];
    alpha[i] = f->u_alpha[i * 4];
  }
  glUniform4fv(u->priority, 2, priority);
  glUniform4fv(u->alpha, 2, alpha);
  glUniform4fv(u->offset, 1, f->u_coloroffset);
  glUniform1f(u->condition, f->u_cctll);
  glUniform1f(u->emu_height, f->u_emu_height);
  glUniform1f(u->height, f->u_vheight);
  glUniform1f(u->viewport, f->u_viewport_offset);
  glUniform1i(u->ram_offset, f->u_color_ram_offset);
  glUniform1i(u->sprite_window, f->u_sprite_window);
}
