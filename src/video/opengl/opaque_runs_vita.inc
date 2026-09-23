/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "opaque_normal_f.h"
#include "opaque_cram_f.h"
#include "opaque_cell.h"
#ifdef VITA_VDP2_OPAQUE_GROUPS
#include "cell_grid.h"
static uint16_t vita_opaque_indices[65532];
#endif
extern int YuiCapturePending(void);
static struct {
  GLuint program;
  GLint matrix,size,offset,texture,color;
} vita_opaque_programs[2];
static GLuint YglOpaqueProgram(unsigned palette) {
  if(vita_opaque_programs[palette].program) return vita_opaque_programs[palette].program;
  GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
  GLuint p=glCreateProgram();
  YglVitaLoadGxp(vs,ygl_vita_normal_v,sizeof(ygl_vita_normal_v),"u_mvpMatrix");
  YglVitaLoadGxp(fs,palette?ygl_vita_opaque_cram_f:ygl_vita_opaque_normal_f,
    palette?sizeof(ygl_vita_opaque_cram_f):sizeof(ygl_vita_opaque_normal_f),NULL);
  glAttachShader(p,vs); glAttachShader(p,fs);
  glBindAttribLocation(p,0,"a_position"); glBindAttribLocation(p,1,"a_texcoord");
  glLinkProgram(p);
  GLint linked=0; glGetProgramiv(p,GL_LINK_STATUS,&linked);
  glDeleteShader(vs); glDeleteShader(fs);
  if(!linked) { glDeleteProgram(p); return 0; }
  vita_opaque_programs[palette].program=p;
#define LOCATION(field,name) vita_opaque_programs[palette].field=glGetUniformLocation(p,name)
  LOCATION(matrix,"u_mvpMatrix"); LOCATION(size,"u_texsize");
  LOCATION(offset,"u_color_offset"); LOCATION(texture,"s_texture"); LOCATION(color,"s_color");
#undef LOCATION
  return p;
}
void YglOpaqueDeInit(void) {
  for(unsigned i=0;i<2;++i) {
    if(vita_opaque_programs[i].program) glDeleteProgram(vita_opaque_programs[i].program);
    memset(&vita_opaque_programs[i],0,sizeof(vita_opaque_programs[i]));
  }
}
/* No sorting, depth/blend changes, or GPU readback. Only normal CPU-atlas
 * cells qualify. Rotation outputs, palette priority, effects and windows'
 * separate geometry remain on their existing paths. */
int YglDrawOpaqueRuns(YglProgram *p,const float *matrix) {
  if((p->prgid!=PG_NORMAL && p->prgid!=PG_VDP2_NORMAL_CRAM) ||
     (p->blendmode&3)!=VDP2_CC_NONE || p->interuput_texture || p->vaid ||
     p->currentQuad<=0 || p->currentQuad%12 ||
     YglVitaRotationWritten() || YglVitaRotationUsesAtlas()) return 0;
  YglTextureManager *tm=_Ygl->texture_manager;
  GLint active,binding;
  glGetIntegerv(GL_ACTIVE_TEXTURE,&active); glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding); glActiveTexture(active);
  if(binding!=tm->textureID_in[tm->current]) return 0;
  unsigned palette=p->prgid==PG_VDP2_NORMAL_CRAM;
  GLuint opaque=YglOpaqueProgram(palette);
  if(!opaque) return 0;
  glUseProgram(opaque);
  glUniformMatrix4fv(vita_opaque_programs[palette].matrix,1,GL_FALSE,matrix);
  glUniform2f(vita_opaque_programs[palette].size,tm->width,YglTextureHeight(tm));
  glUniform4fv(vita_opaque_programs[palette].offset,1,p->color_offset_val);
  glUniform1i(vita_opaque_programs[palette].texture,0);
  if(palette) glUniform1i(vita_opaque_programs[palette].color,1);
  unsigned quads=p->currentQuad/12,begin=0,runs=0,admitted=0;
#ifdef VITA_VDP2_OPAQUE_GROUPS
  if(quads<=65532/6 && YglDisjointCellGrid(p->quads,quads)) {
    unsigned opaque_count=0,mixed_begin=quads*6;
    for(unsigned q=0;q<quads;++q) {
      int cell=YglOpaqueCell(tm->texture_in[tm->current],tm->width,tm->yMax,p->textcoords+q*24);
      unsigned offset;
      if(cell) { offset=opaque_count; opaque_count+=6; }
      else { mixed_begin-=6; offset=mixed_begin; }
      for(unsigned i=0;i<6;++i) vita_opaque_indices[offset+i]=q*6+i;
    }
    GLint old_element;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&old_element);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    if(opaque_count) {
      glUseProgram(opaque);
      glDrawRangeElements(GL_TRIANGLES,0,quads*6-1,opaque_count,GL_UNSIGNED_SHORT,vita_opaque_indices);
    }
    glUseProgram(p->prg);
    if(mixed_begin<quads*6)
      glDrawRangeElements(GL_TRIANGLES,0,quads*6-1,quads*6-mixed_begin,GL_UNSIGNED_SHORT,vita_opaque_indices+mixed_begin);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,old_element);
    if(YuiCapturePending()) YuiMsg("opaque_groups quads=%u admitted=%u draws=%u disjoint_grid=1",quads,opaque_count/6,(opaque_count!=0)+(mixed_begin<quads*6));
    return 1;
  }
#endif
  int kind=YglOpaqueCell(tm->texture_in[tm->current],tm->width,tm->yMax,p->textcoords);
  for(unsigned q=1;q<=quads;++q) {
    int next=q<quads?YglOpaqueCell(tm->texture_in[tm->current],tm->width,tm->yMax,p->textcoords+q*24):-1;
    if(next==kind) continue;
    glUseProgram(kind?opaque:p->prg);
    glDrawArrays(GL_TRIANGLES,begin*6,(q-begin)*6);
    if(kind) admitted+=q-begin;
    ++runs; begin=q; kind=next;
  }
  glUseProgram(p->prg);
  if(YuiCapturePending()) YuiMsg("opaque_runs quads=%u admitted=%u draws=%u",quads,admitted,runs);
  return 1;
}
