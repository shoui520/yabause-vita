/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "normal_v.h"
#include "normal_direct_v.h"
#include "normal_f.h"
#include "cram_f.h"
#include "priority_f.h"
#include "line_offset_f.h"
#include "add_color_f.h"
#include "sprite_v.h"
#include "window_v.h"
#include "window_f.h"
#include "line_insert_f.h"
#include "line_insert_cram_f.h"
#include "line_insert_dest_f.h"
#include "line_insert_dest_cram_f.h"
#include "rbg_line_f.h"
#include "framebuffer_v.h"
#include "add_shadow_f.h"
#include "fb_normal_f.h"
#include "fb_dest_f.h"
#include "fb_rate_0_f.h"
#include "fb_add_0_f.h"
#include "fb_rate_1_f.h"
#include "fb_add_1_f.h"
#include "fb_rate_2_f.h"
#include "fb_add_2_f.h"
#include "fb_rate_3_f.h"
#include "fb_add_3_f.h"
#include "fb_normal_hblank_f.h"
#include "fb_dest_hblank_f.h"
#include "fb_rate_0_hblank_f.h"
#include "fb_add_0_hblank_f.h"
#include "fb_rate_1_hblank_f.h"
#include "fb_add_1_hblank_f.h"
#include "fb_rate_2_hblank_f.h"
#include "fb_add_2_hblank_f.h"
#include "fb_rate_3_hblank_f.h"
#include "fb_add_3_hblank_f.h"
#include "fb_dest_line_0_f.h"
#include "fb_rate_line_0_f.h"
#include "fb_add_line_0_f.h"
#include "fb_dest_line_1_f.h"
#include "fb_rate_line_1_f.h"
#include "fb_add_line_1_f.h"
#include "fb_dest_line_2_f.h"
#include "fb_rate_line_2_f.h"
#include "fb_add_line_2_f.h"
#include "fb_dest_line_3_f.h"
#include "fb_rate_line_3_f.h"
#include "fb_add_line_3_f.h"
#include "fb_shadow_f.h"
#include "sprite_normal_f.h"
#include "sprite_gouraud_f.h"
#include "sprite_spd_f.h"
#include "sprite_half_f.h"
#include "sprite_gouraud_half_f.h"
#include "sprite_shadow_f.h"
#include "sprite_mesh_f.h"
#include "sprite_luminance_f.h"

static GLint vita_atlas_size[PG_MAX];

static void YglVitaLoadGxp(GLuint shader, const unsigned char *gxp,
                          size_t size, const char *matrix_name) {
  /* Pinned vitaGL glShaderBinary expects its serialized prefix, not raw GXP.
   * Matrix indices let glUniformMatrix4fv preserve vitaGL's transpose rules.
   * This format requires HAVE_GLSL_TEXTURE_SIZE=0 in the pinned library build;
   * native shaders receive their texture dimensions explicitly.
   */
  const SceGxmProgram *program = (const SceGxmProgram *)gxp;
  uint32_t prefix[2] = {0, 0};
  if (matrix_name) {
    const SceGxmProgramParameter *matrix = sceGxmProgramFindParameterByName(program, matrix_name);
    if (!matrix) abort();
    unsigned count = sceGxmProgramGetParameterCount(program);
    for (unsigned i = 0; i < count; ++i) {
      if (sceGxmProgramGetParameter(program, i) == matrix) {
        prefix[0] = 1; prefix[1] = i; break;
      }
    }
    if (!prefix[0]) abort();
  }
  size_t prefix_size = sizeof(uint32_t) * (prefix[0] + 1);
  unsigned char *serialized = malloc(prefix_size + size);
  if (!serialized) abort();
  memcpy(serialized, prefix, prefix_size);
  memcpy(serialized + prefix_size, gxp, size);
  glShaderBinary(1, &shader, 0, serialized, prefix_size + size);
  free(serialized);
}

static int YglVitaLoadNativePair(int id, GLuint vertex, GLuint fragment) {
  const unsigned char *binary;
  GLsizei size;
  const unsigned char *vertex_binary = ygl_vita_normal_v;
  size_t vertex_size = sizeof(ygl_vita_normal_v);
  switch (id) {
  case PG_VDP2_DRAWFRAMEBUFF_ADDCOLOR_SHADOW:
    binary = ygl_vita_add_shadow_f; size = sizeof(ygl_vita_add_shadow_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF:
    binary = ygl_vita_fb_normal_f; size = sizeof(ygl_vita_fb_normal_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_DESTALPHA:
    binary = ygl_vita_fb_dest_f; size = sizeof(ygl_vita_fb_dest_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_CCOL:
    binary = ygl_vita_fb_rate_0_f; size = sizeof(ygl_vita_fb_rate_0_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_ADD:
    binary = ygl_vita_fb_add_0_f; size = sizeof(ygl_vita_fb_add_0_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_CCOL:
    binary = ygl_vita_fb_rate_1_f; size = sizeof(ygl_vita_fb_rate_1_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_ADD:
    binary = ygl_vita_fb_add_1_f; size = sizeof(ygl_vita_fb_add_1_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_CCOL:
    binary = ygl_vita_fb_rate_2_f; size = sizeof(ygl_vita_fb_rate_2_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_ADD:
    binary = ygl_vita_fb_add_2_f; size = sizeof(ygl_vita_fb_add_2_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_CCOL:
    binary = ygl_vita_fb_rate_3_f; size = sizeof(ygl_vita_fb_rate_3_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_ADD:
    binary = ygl_vita_fb_add_3_f; size = sizeof(ygl_vita_fb_add_3_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_HBLANK:
    binary = ygl_vita_fb_normal_hblank_f; size = sizeof(ygl_vita_fb_normal_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_DESTALPHA_HBLANK:
    binary = ygl_vita_fb_dest_hblank_f; size = sizeof(ygl_vita_fb_dest_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_CCOL_HBLANK:
    binary = ygl_vita_fb_rate_0_hblank_f; size = sizeof(ygl_vita_fb_rate_0_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_ADD_HBLANK:
    binary = ygl_vita_fb_add_0_hblank_f; size = sizeof(ygl_vita_fb_add_0_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_CCOL_HBLANK:
    binary = ygl_vita_fb_rate_1_hblank_f; size = sizeof(ygl_vita_fb_rate_1_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_ADD_HBLANK:
    binary = ygl_vita_fb_add_1_hblank_f; size = sizeof(ygl_vita_fb_add_1_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_CCOL_HBLANK:
    binary = ygl_vita_fb_rate_2_hblank_f; size = sizeof(ygl_vita_fb_rate_2_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_ADD_HBLANK:
    binary = ygl_vita_fb_add_2_hblank_f; size = sizeof(ygl_vita_fb_add_2_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_CCOL_HBLANK:
    binary = ygl_vita_fb_rate_3_hblank_f; size = sizeof(ygl_vita_fb_rate_3_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_ADD_HBLANK:
    binary = ygl_vita_fb_add_3_hblank_f; size = sizeof(ygl_vita_fb_add_3_hblank_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_DESTALPHA_LINE:
    binary = ygl_vita_fb_dest_line_0_f; size = sizeof(ygl_vita_fb_dest_line_0_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_CCOL_LINE:
    binary = ygl_vita_fb_rate_line_0_f; size = sizeof(ygl_vita_fb_rate_line_0_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_LESS_ADD_LINE:
    binary = ygl_vita_fb_add_line_0_f; size = sizeof(ygl_vita_fb_add_line_0_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EQUAL_DESTALPHA_LINE:
    binary = ygl_vita_fb_dest_line_1_f; size = sizeof(ygl_vita_fb_dest_line_1_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_CCOL_LINE:
    binary = ygl_vita_fb_rate_line_1_f; size = sizeof(ygl_vita_fb_rate_line_1_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_EUQAL_ADD_LINE:
    binary = ygl_vita_fb_add_line_1_f; size = sizeof(ygl_vita_fb_add_line_1_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_DESTALPHA_LINE:
    binary = ygl_vita_fb_dest_line_2_f; size = sizeof(ygl_vita_fb_dest_line_2_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_CCOL_LINE:
    binary = ygl_vita_fb_rate_line_2_f; size = sizeof(ygl_vita_fb_rate_line_2_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MORE_ADD_LINE:
    binary = ygl_vita_fb_add_line_2_f; size = sizeof(ygl_vita_fb_add_line_2_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_DESTALPHA_LINE:
    binary = ygl_vita_fb_dest_line_3_f; size = sizeof(ygl_vita_fb_dest_line_3_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_CCOL_LINE:
    binary = ygl_vita_fb_rate_line_3_f; size = sizeof(ygl_vita_fb_rate_line_3_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_MSB_ADD_LINE:
    binary = ygl_vita_fb_add_line_3_f; size = sizeof(ygl_vita_fb_add_line_3_f); goto framebuffer;
  case PG_VDP2_DRAWFRAMEBUFF_SHADOW:
    binary = ygl_vita_fb_shadow_f; size = sizeof(ygl_vita_fb_shadow_f); goto framebuffer;
  case PG_NORMAL:
    binary = ygl_vita_normal_f; size = sizeof(ygl_vita_normal_f); break;
  case PG_WINDOW:
    vertex_binary = ygl_vita_window_v; vertex_size = sizeof(ygl_vita_window_v);
    binary = ygl_vita_window_f; size = sizeof(ygl_vita_window_f); break;
  case PG_VDP2_RBG_CRAM_LINE:
    binary = ygl_vita_rbg_line_f; size = sizeof(ygl_vita_rbg_line_f); break;
  case PG_LINECOLOR_INSERT:
    binary = ygl_vita_line_insert_f; size = sizeof(ygl_vita_line_insert_f); break;
  case PG_LINECOLOR_INSERT_CRAM:
    binary = ygl_vita_line_insert_cram_f; size = sizeof(ygl_vita_line_insert_cram_f); break;
  case PG_LINECOLOR_INSERT_DESTALPHA:
    binary = ygl_vita_line_insert_dest_f; size = sizeof(ygl_vita_line_insert_dest_f); break;
  case PG_LINECOLOR_INSERT_DESTALPHA_CRAM:
    binary = ygl_vita_line_insert_dest_cram_f; size = sizeof(ygl_vita_line_insert_dest_cram_f); break;
  case PG_VDP2_NORMAL_CRAM:
    binary = ygl_vita_cram_f; size = sizeof(ygl_vita_cram_f); break;
  case PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY:
    binary = ygl_vita_priority_f; size = sizeof(ygl_vita_priority_f); break;
  case PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET:
    binary = ygl_vita_line_offset_f; size = sizeof(ygl_vita_line_offset_f); break;
  case PG_VDP2_ADDCOLOR_CRAM:
    binary = ygl_vita_add_color_f; size = sizeof(ygl_vita_add_color_f); break;
  case PG_VDP1_NORMAL:
    binary = ygl_vita_sprite_normal_f; size = sizeof(ygl_vita_sprite_normal_f); goto sprite;
  case PG_VFP1_GOURAUDSAHDING:
    binary = ygl_vita_sprite_gouraud_f; size = sizeof(ygl_vita_sprite_gouraud_f); goto sprite;
  case PG_VFP1_GOURAUDSAHDING_SPD:
    binary = ygl_vita_sprite_spd_f; size = sizeof(ygl_vita_sprite_spd_f); goto sprite;
  case PG_VFP1_HALFTRANS:
    binary = ygl_vita_sprite_half_f; size = sizeof(ygl_vita_sprite_half_f); goto sprite;
  case PG_VFP1_GOURAUDSAHDING_HALFTRANS:
    binary = ygl_vita_sprite_gouraud_half_f; size = sizeof(ygl_vita_sprite_gouraud_half_f); goto sprite;
  case PG_VFP1_SHADOW:
    binary = ygl_vita_sprite_shadow_f; size = sizeof(ygl_vita_sprite_shadow_f); goto sprite;
  case PG_VFP1_MESH:
    binary = ygl_vita_sprite_mesh_f; size = sizeof(ygl_vita_sprite_mesh_f); goto sprite;
  case PG_VFP1_HALF_LUMINANCE:
    binary = ygl_vita_sprite_luminance_f; size = sizeof(ygl_vita_sprite_luminance_f); goto sprite;
  default: return 0;
  }
  goto load;
sprite:
  vertex_binary = ygl_vita_sprite_v;
  vertex_size = sizeof(ygl_vita_sprite_v);
  goto load;
framebuffer:
  vertex_binary = ygl_vita_framebuffer_v;
  vertex_size = sizeof(ygl_vita_framebuffer_v);
load:
#ifdef VITA_NORMAL_DIRECT_UV
  if (id == PG_NORMAL || id == PG_VDP2_NORMAL_CRAM ||
      id == PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY ||
      id == PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET ||
      id == PG_VDP2_ADDCOLOR_CRAM) {
    vertex_binary = ygl_vita_normal_direct_v;
    vertex_size = sizeof(ygl_vita_normal_direct_v);
  }
#endif
  YglVitaLoadGxp(vertex, vertex_binary, vertex_size, "u_mvpMatrix");
  YglVitaLoadGxp(fragment, binary, size, NULL);
  return 1;
}

static void YglVitaAtlasUniform(int id) {
  /* CPU rotation output shares this atlas; compute-generated textures are not
   * a supported Vita path. Locations are cached at link, never queried per draw.
   */
  switch (id) {
  case PG_VDP2_BLUR: case PG_VDP2_MOSAIC: case PG_VDP2_PER_LINE_ALPHA:
  case PG_VDP2_ADDBLEND:
    id = PG_NORMAL; break;
  case PG_VDP2_BLUR_CRAM: case PG_VDP2_MOSAIC_CRAM: case PG_VDP2_PER_LINE_ALPHA_CRAM:
    id = PG_VDP2_NORMAL_CRAM; break;
  }
  glUniform2f(vita_atlas_size[id], (float)YglTM->width, (float)YglTextureHeight(YglTM));
}
