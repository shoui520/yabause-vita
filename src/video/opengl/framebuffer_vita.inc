/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native uniform locations are cached once; use the same CPU register snapshot
 * as the desktop UBO, explicitly unpacking its std140 scalar-array padding. */
typedef struct {
  GLint priority, alpha, offset, condition, emu_height, height, viewport, ram_offset, sprite_window;
  GLint from, to, meta;
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
  u->from = glGetUniformLocation(_prgid[id], "u_from");
  u->to = glGetUniformLocation(_prgid[id], "u_to");
  u->meta = glGetUniformLocation(_prgid[id], "s_meta");
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

#ifdef VITA_FB_META_LUT
#include "fb_meta_lut.h"
/* Metadata tables of the draws, one texture each; draws of a frame differ in
 * priority range, frames mostly repeat, so a table is uploaded only when no
 * slot holds its contents (the least recently used one is replaced). */
enum { FB_META_SLOTS = 8, FB_META_UNIT = 3 };
static GLuint vita_fb_meta_tex[FB_META_SLOTS];
static uint32_t vita_fb_meta_data[FB_META_SLOTS][256];
static unsigned vita_fb_meta_used[FB_META_SLOTS], vita_fb_meta_clock;

static int YglVitaFramebufferMode(int id, int *mode, int *condition) {
  switch (id) {
  case PG_VDP2_DRAWFRAMEBUFF: *mode = 0; *condition = 0; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_DESTALPHA: *mode = 1; *condition = 0; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_CCOL: *mode = 2; *condition = 0; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_CCOL: *mode = 2; *condition = 1; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_CCOL: *mode = 2; *condition = 2; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_ADD: *mode = 3; *condition = 0; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_ADD: *mode = 3; *condition = 1; return 1;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_ADD: *mode = 3; *condition = 2; return 1;
  default: return 0;
  }
}
#endif

/* u_from/u_to of a composition draw, and its metadata table. Call after
 * YglVitaBindFramebufferUniforms with the same _Ygl->fbu_. */
static void YglVitaFramebufferRange(int id, float from, float to) {
  const YglVitaFramebufferUniforms *u = &vita_fb_uniforms[id];
  glUniform1f(u->from, from);
  glUniform1f(u->to, to);
#ifdef VITA_FB_META_LUT
  int mode, condition;
  if (u->meta < 0 || !YglVitaFramebufferMode(id, &mode, &condition)) return;
  const UniformFrameBuffer *f = &_Ygl->fbu_;
  float priority[8], alpha[8];
  for (unsigned i = 0; i < 8; ++i) {
    priority[i] = f->u_pri[i * 4];
    alpha[i] = f->u_alpha[i * 4];
  }
  uint32_t table[256];
  FbMetaLutBuild(table, mode, condition, priority, alpha, from, to, f->u_cctll, f->u_sprite_window);
  unsigned slot = 0;
  for (unsigned i = 0; i < FB_META_SLOTS; ++i) {
    if (vita_fb_meta_tex[i] && !memcmp(vita_fb_meta_data[i], table, sizeof(table))) { slot = i; goto bind; }
    if (vita_fb_meta_used[i] < vita_fb_meta_used[slot]) slot = i;
  }
  glActiveTexture(GL_TEXTURE0 + FB_META_UNIT);
  if (!vita_fb_meta_tex[slot]) {
    glGenTextures(1, &vita_fb_meta_tex[slot]);
    glBindTexture(GL_TEXTURE_2D, vita_fb_meta_tex[slot]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, table);
  } else {
    glBindTexture(GL_TEXTURE_2D, vita_fb_meta_tex[slot]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, table);
  }
  memcpy(vita_fb_meta_data[slot], table, sizeof(table));
  glActiveTexture(GL_TEXTURE0);
bind:
  vita_fb_meta_used[slot] = ++vita_fb_meta_clock;
  glUniform1i(u->meta, FB_META_UNIT);
  glActiveTexture(GL_TEXTURE0 + FB_META_UNIT);
  glBindTexture(GL_TEXTURE_2D, vita_fb_meta_tex[slot]);
  glActiveTexture(GL_TEXTURE0);
#endif
}
