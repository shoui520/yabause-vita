/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production native-uniform adapter with recording GL calls.
 * This tests register unpacking, not GPU execution or image correctness.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/video/opengl/quad_indices.h"
#include "../src/video/opengl/cell_alpha.h"
#include "../src/video/opengl/opaque_cell.h"
#include "../src/video/opengl/cell_grid.h"
#include "../src/video/opengl/palette_upload.h"
#include "../src/vita/frame_capture.h"
#include "../src/video/opengl/shaders/packed_texture.h"
static void packed_texture_test(void) {
  for (int byte = 0; byte < 256; ++byte) {
    float normalized = (float)byte / 255.0f;
    // Half's maximum rounding error in [0,1] is 1/4096. Both sides must
    // decode identically, including the 0x7f/0x80 validity boundary.
    assert(YglUnorm8(normalized) == byte);
    assert(YglUnorm8(normalized - 1.0f/4096) == byte);
    assert(YglUnorm8(normalized + 1.0f/4096) == byte);
    for (int high = 0; high < 256; ++high)
      assert(YglPaletteIndex(normalized - 1.0f/4096,
                            (float)high/255.0f + 1.0f/4096) == (byte | (high << 8)));
  }
  puts("packed texture bytes: all 256 bytes and 65536 indices with half-rounding bounds: passed");
  for (unsigned word = 0; word < 65536; ++word)
    for (int hi_error = -1; hi_error <= 1; ++hi_error)
      for (int lo_error = -1; lo_error <= 1; ++lo_error) {
        float hi = (float)(word >> 8) / 255.0f + hi_error / 4096.0f;
        float lo = (float)(word & 255) / 255.0f + lo_error / 4096.0f;
        assert(YglUnorm16BE(hi, lo) == (int)word);
      }
  puts("packed words: all 65536 values, nine half-rounding combinations: passed");
}
static void capture_test(void) {
  const uint8_t pixels[] = {0,0,255,17, 255,255,255,18,
                           255,0,0,19, 0,255,0,20};
  const uint8_t expected[] = {'P','6','\n','2',' ','2','\n','2','5','5','\n',
                             255,0,0, 0,255,0, 0,0,255, 255,255,255};
  FILE *file = tmpfile(); assert(file);
  assert(VitaWriteRgbFrame(file, pixels, 2, 2) == 0);
  rewind(file);
  uint8_t actual[sizeof(expected) + 1];
  assert(fread(actual, 1, sizeof(actual), file) == sizeof(expected));
  assert(memcmp(actual, expected, sizeof(expected)) == 0);
  assert(VitaWriteRgbFrame(file, pixels, 0, 2) == -1);
  fclose(file);
  const uint8_t padded[] = {255,0,0,19, 0,255,0,20, 9,9,9,9,
                           0,0,255,17, 255,255,255,18, 8,8,8,8};
  file = tmpfile(); assert(file);
  assert(VitaWriteRgbRows(file, padded, 2, 2, 3, 0) == 0);
  rewind(file);
  assert(fread(actual, 1, sizeof(actual), file) == sizeof(expected));
  assert(memcmp(actual, expected, sizeof(expected)) == 0);
  assert(VitaWriteRgbRows(file, padded, 2, 2, 1, 0) == -1);
  fclose(file);
  puts("frame capture: row orientation, RGB channels and bounds: passed");
}
typedef struct {
  float *quads, *textcoords, *vertexAttribute;
  int currentQuad, maxQuad;
} YglProgram;
static int allocations, fail_allocation;
static void *geometry_malloc(size_t size) {
  if (++allocations == fail_allocation) return NULL;
  return malloc(size);
}
#define malloc geometry_malloc
#include "../src/video/opengl/geometry_buffer.inc"
#undef malloc

static void geometry_test(void) {
  YglProgram p = {0};
  assert(YglReserveGeometry(&p, 768) == 0);
  assert(p.maxQuad == 768 && allocations == 3);
  float *old = p.quads;
  assert(YglReserveGeometry(&p, 12) == 0 && p.quads == old);
  assert(YglReserveGeometry(&p, 768) == 0 && allocations == 3);
  for (int i = 0; i < 768; ++i) p.quads[i] = (float)i;
  for (int i = 0; i < 1536; ++i)
    p.textcoords[i] = p.vertexAttribute[i] = (float)-i;
  p.currentQuad = 768;
  assert(YglReserveGeometry(&p, 12) == 0 && p.maxQuad == 1536);
  for (int i = 0; i < 768; ++i) assert(p.quads[i] == (float)i);
  for (int i = 0; i < 1536; ++i) {
    assert(p.textcoords[i] == (float)-i);
    assert(p.vertexAttribute[i] == (float)-i);
  }
  old = p.quads;
  float *old_tex = p.textcoords, *old_attr = p.vertexAttribute;
  fail_allocation = allocations + 2;
  assert(YglReserveGeometry(&p, 2000) == -1);
  assert(p.quads == old && p.textcoords == old_tex && p.vertexAttribute == old_attr);
  assert(p.maxQuad == 1536 && p.currentQuad == 768);
  assert(YglReserveGeometry(&p, 0xffffffffU) == -1);
  p.currentQuad = p.maxQuad + 1;
  assert(YglReserveGeometry(&p, 12) == -1);
  assert(p.quads == old && p.textcoords == old_tex && p.vertexAttribute == old_attr);
  free(p.quads); free(p.textcoords); free(p.vertexAttribute);
  puts("geometry reservation: exact-fit reuse, bounded growth, preservation and failure: passed");
}
typedef int GLint;
typedef unsigned GLuint;
enum {GL_FRAMEBUFFER=1,GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,
      GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D};
static unsigned color_attachments;
static GLuint attached_color;
static void glFramebufferTexture2D(unsigned target,unsigned attachment,
                                   unsigned type,GLuint object,int level) {
  assert(target==GL_FRAMEBUFFER && attachment==GL_COLOR_ATTACHMENT0);
  assert(type==GL_TEXTURE_2D && level==0);
  ++color_attachments; attached_color=object;
}
static unsigned attachments;
static GLuint attached_depth,attached_stencil;
static void glFramebufferRenderbuffer(unsigned target,unsigned attachment,
                                      unsigned type,GLuint object) {
  assert(target==GL_FRAMEBUFFER && type==GL_RENDERBUFFER);
  if(attachment==GL_DEPTH_ATTACHMENT) attached_depth=object;
  else { assert(attachment==GL_STENCIL_ATTACHMENT); attached_stencil=object; }
  ++attachments;
}
#include "../src/video/opengl/vdp1_attachments.h"
#define PG_MAX 4
typedef struct {
  float u_pri[32], u_alpha[32], u_coloroffset[4];
  float u_cctll, u_emu_height, u_vheight;
  int u_color_ram_offset;
  float u_viewport_offset;
  int u_sprite_window;
} UniformFrameBuffer;
static struct { UniformFrameBuffer fbu_; } state, *_Ygl = &state;
static GLuint _prgid[PG_MAX] = {0, 41, 42, 0};
static const char *names[] = {
  "u_pri", "u_alpha", "u_coloroffset", "u_cctl", "u_emu_height",
  "u_vheight", "u_viewport_offset", "u_color_ram_offset", "u_sprite_window"
};
static float recorded[9][8];
static int counts[9], components[9], lookups, omit_alpha;
static GLint glGetUniformLocation(GLuint program, const char *name) {
  assert(program == 41 || program == 42);
  ++lookups;
  for (int i = 0; i < 9; ++i)
    if (!strcmp(name, names[i])) return omit_alpha && i == 1 ? -1 : i+1;
  assert(0); return -1;
}
static void glUniform1fv(GLint location, int count, const float *values) {
  if (location == -1) return;
  assert(location >= 1 && location <= 9 && count <= 8);
  memcpy(recorded[location-1], values, count * sizeof(float));
  counts[location-1] = count;
  components[location-1] = 1;
}
static void glUniform4fv(GLint location, int count, const float *values) {
  assert(count == 1 || count == 2); glUniform1fv(location, 4*count, values);
  if(location!=-1) components[location-1]=4;
}
static void glUniform1f(GLint location, float value) {
  glUniform1fv(location, 1, &value);
}
static void glUniform1i(GLint location, int value) {
  float converted = value; glUniform1fv(location, 1, &converted);
}
#include "../src/video/opengl/framebuffer_vita.inc"
#include "../src/video/opengl/framebuffer_priority.h"
#include "../src/video/opengl/shaders/mosaic_cell.cg"
int main(void) {
  YglPaletteUpload palette_upload={0};
  uint32_t palette_data[2048]={0};
  assert(!YglPaletteUploadEqual(&palette_upload,palette_data,0,2048));
  YglPaletteUploadRemember(&palette_upload,palette_data,0,1024);
  assert(!palette_upload.valid); /* Partial update cannot initialize unknown backing. */
  YglPaletteUploadRemember(&palette_upload,palette_data,0,2048);
  for(unsigned i=0;i<2048;++i) {
    assert(YglPaletteUploadEqual(&palette_upload,palette_data,0,2048));
    palette_data[i]^=0x87654321u;
    assert(!YglPaletteUploadEqual(&palette_upload,palette_data,i,1));
    if(i) assert(YglPaletteUploadEqual(&palette_upload,palette_data,0,i));
    YglPaletteUploadRemember(&palette_upload,palette_data,i,1);
    assert(YglPaletteUploadEqual(&palette_upload,palette_data,0,2048));
  }
  assert(!YglPaletteUploadEqual(&palette_upload,palette_data,2048,1));
  palette_upload.valid=0;
  assert(!YglPaletteUploadEqual(&palette_upload,palette_data,0,2048));
  float grid[12*16];
  for(unsigned q=0;q<16;++q) {
    float x=(int)(q%4)*8-8,y=(int)(q/4)*8-8;
    float rect[]={x,y,x+8,y,x+8,y+8,x,y,x+8,y+8,x,y+8};
    memcpy(grid+q*12,rect,sizeof(rect));
  }
  assert(YglDisjointCellGrid(grid,16));
  float damaged[12*16];
  memcpy(damaged,grid,sizeof(grid)); memcpy(damaged+12,damaged,12*sizeof(float));
  assert(!YglDisjointCellGrid(damaged,16)); /* duplicate */
  memcpy(damaged,grid,sizeof(grid));
  for(unsigned i=0;i<12;i+=2) damaged[12+i]-=1;
  assert(!YglDisjointCellGrid(damaged,16)); /* overlapping off-grid cell */
  memcpy(damaged,grid,sizeof(grid)); damaged[2]+=.25f;
  assert(!YglDisjointCellGrid(damaged,16));
  memcpy(damaged,grid,sizeof(grid)); damaged[5]=NAN;
  assert(!YglDisjointCellGrid(damaged,16));
  assert(!YglDisjointCellGrid(grid,0));
  uint32_t opaque_pixels[32*32];
  float opaque_uv[]={8.025f,8.025f,0,1,15.975f,8.025f,0,1,15.975f,15.975f,0,1,
                     8.025f,8.025f,0,1,15.975f,15.975f,0,1,8.025f,15.975f,0,1};
  for(unsigned flip=0;flip<4;++flip) {
    float uv[24]; memcpy(uv,opaque_uv,sizeof(uv));
    for(unsigned v=0;v<6;++v) {
      if(flip&1) uv[4*v]=24-uv[4*v];
      if(flip&2) uv[4*v+1]=24-uv[4*v+1];
    }
    for(unsigned i=0;i<1024;++i) opaque_pixels[i]=0x01000000u;
    assert(YglOpaqueCell(opaque_pixels,32,32,uv));
    for(unsigned y=8;y<16;++y) for(unsigned x=8;x<16;++x) {
      opaque_pixels[y*32+x]=0;
      assert(!YglOpaqueCell(opaque_pixels,32,32,uv));
      opaque_pixels[y*32+x]=0x01000000u;
    }
  }
  const float rejected[]={NAN,INFINITY,-1,32,8};
  for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
    float uv[24]; memcpy(uv,opaque_uv,sizeof(uv)); uv[0]=rejected[i];
    assert(!YglOpaqueCell(opaque_pixels,32,32,uv));
  }
  uint32_t texels[8*12];
  for(unsigned alpha=0;alpha<256;++alpha) {
    for(unsigned i=0;i<8*12;++i) texels[i]=(alpha<<24)|0xabcdef;
    assert(YglCellAlphaClass(texels,12,8,8)==(alpha!=0));
    for(unsigned i=0;i<64;++i) {
      unsigned p=i/8*12+i%8;
      texels[p]^=0xff000000u;
      assert(YglCellAlphaClass(texels,12,8,8)==((alpha==0 || alpha==255)?2:1));
      texels[p]^=0xff000000u;
    }
  }
  uint16_t indices[YGL_QUAD_INDEX_COUNT];
  YglFillQuadIndices(indices,YGL_QUAD_INDEX_COUNT);
  const unsigned corners[]={0,1,2,0,2,3};
  for(unsigned i=0;i<YGL_QUAD_INDEX_COUNT;++i) {
    assert(indices[i]<YGL_QUAD_INDEX_COUNT);
    assert(indices[i]/6==i/6 && corners[indices[i]%6]==corners[i%6]);
  }
  YglVdp1AttachColor(1,11,1);
  for(unsigned i=0;i<100;++i) YglVdp1AttachColor(1,11,0);
#ifdef VITA_VDP1_RETAIN_COLOR
  assert(color_attachments==1);
#else
  assert(color_attachments==101);
#endif
  unsigned previous=color_attachments;
  YglVdp1AttachColor(1,12,0); assert(attached_color==12);
  YglVdp1AttachColor(1,11,0); assert(attached_color==11);
  YglVdp1AttachColor(2,11,0);
  YglVdp1AttachColor(2,11,1); /* Rebuilt backing, recycled object IDs. */
  assert(color_attachments==previous+4);
  YglVdp1AttachDepth(17,17,1);
  assert(attachments==2 && attached_depth==17 && attached_stencil==17);
  for(unsigned i=0;i<100;++i) YglVdp1AttachDepth(17,17,0);
#ifdef VITA_VDP1_RETAIN_ATTACHMENTS
  assert(attachments==2);
#else
  assert(attachments==202);
#endif
  YglVdp1AttachDepth(23,29,1);
  assert(attached_depth==23 && attached_stencil==29);
  unsigned priority_cases=0;
  for(unsigned mask=1;mask<256;++mask) {
    float p[32]={0}; unsigned n=0;
    for(unsigned i=0;i<8;++i) if(mask&(1u<<i)) p[4*n++]=i/10.0f+0.05f;
    while(n<8) { p[4*n]=p[0]; ++n; }
    for(unsigned from=0;from<=8;++from) for(unsigned to=from;to<=8;++to) {
      int expected=0;
      for(unsigned i=from;i<to;++i) expected|=!!(mask&(1u<<i));
      assert(YglFramebufferPriorityVisible(p,from/10.0f,to/10.0f)==expected);
      ++priority_cases;
    }
  }
  printf("framebuffer priority intervals: %u cases passed\n",priority_cases);
  {
    float p[32]={0}; p[28]=0.5f;
    assert(YglFramebufferPriorityVisible(p,0.5f,0.5f));
    p[28]=NAN;
    assert(YglFramebufferPriorityVisible(p,0.5f,0.5f));
  }
  packed_texture_test();
  capture_test();
  geometry_test();
  for (int size = 1; size <= 16; ++size)
    for (int coordinate = -4096; coordinate <= 4096; ++coordinate)
      assert(cell(coordinate, size) == (coordinate / size) * size);
  for (int i = 0; i < 32; ++i)
    state.fbu_.u_pri[i] = state.fbu_.u_alpha[i] = -999.0f;
  for (int i = 0; i < 8; ++i) {
    state.fbu_.u_pri[i*4] = i / 10.0f + 0.05f;
    state.fbu_.u_alpha[i*4] = (255.0f-i*8.0f)/255.0f;
  }
  for (int i = 0; i < 4; ++i) state.fbu_.u_coloroffset[i] = i-2.0f;
  state.fbu_.u_cctll = 0.35f;
  state.fbu_.u_emu_height = 0.5f;
  state.fbu_.u_vheight = 448.0f;
  state.fbu_.u_viewport_offset = 16.0f;
  state.fbu_.u_color_ram_offset = 1792;
  state.fbu_.u_sprite_window = 1;
  YglVitaCacheFramebufferUniforms(1);
  assert(lookups == 9);
  YglVitaBindFramebufferUniforms(1);
  assert(counts[0] == 8 && counts[1] == 8 && counts[2] == 4);
  assert(components[0] == 4 && components[1] == 4 && components[2] == 4);
  for (int i = 0; i < 8; ++i) {
    assert(recorded[0][i] == state.fbu_.u_pri[i*4]);
    assert(recorded[1][i] == state.fbu_.u_alpha[i*4]);
  }
  for (int i = 0; i < 4; ++i) assert(recorded[2][i] == i-2.0f);
  assert(recorded[3][0] == 0.35f && recorded[4][0] == 0.5f);
  assert(recorded[5][0] == 448.0f && recorded[6][0] == 16.0f);
  assert(recorded[7][0] == 1792.0f && recorded[8][0] == 1.0f);
  state.fbu_.u_pri[0] = 0.75f;
  YglVitaBindFramebufferUniforms(1);
  assert(lookups == 9 && recorded[0][0] == 0.75f);
  omit_alpha = 1;
  YglVitaCacheFramebufferUniforms(2);
  memset(counts, 0, sizeof(counts));
  YglVitaBindFramebufferUniforms(2);
  assert(counts[1] == 0 && counts[0] == 8 && lookups == 18);
  puts("vitaGL framebuffer register unpacking, cached locations and absent uniforms: passed");
}
