/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "quad_indices.h"
static GLuint vita_quad_index_buffer;
static void YglDrawIndexedVdp2Quads(YglProgram *p) {
  unsigned count=p->currentQuad/2;
  if(!count || count%6 || count>YGL_QUAD_INDEX_COUNT) {
    glDrawArrays(GL_TRIANGLES,0,count);
    return;
  }
  /* YglQuad_in/Offset_in/Rbg0 duplicate vertices0/3 and2/4. */
  if(YuiCapturePending()) {
    unsigned errors=0;
    for(unsigned i=0;i<count;i+=6) {
      errors+=memcmp(p->quads+2*i,p->quads+2*(i+3),2*sizeof(float))!=0;
      errors+=memcmp(p->quads+2*(i+2),p->quads+2*(i+4),2*sizeof(float))!=0;
      errors+=memcmp(p->textcoords+4*i,p->textcoords+4*(i+3),4*sizeof(float))!=0;
      errors+=memcmp(p->textcoords+4*(i+2),p->textcoords+4*(i+4),4*sizeof(float))!=0;
    }
    YuiMsg("vdp2_indexed_quads vertices=%u unique=%u contract_errors=%u",count,count/6*4,errors);
    if(errors) abort();
  }
  GLint previous;
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&previous);
  if(!vita_quad_index_buffer) {
    uint16_t *indices=malloc(YGL_QUAD_INDEX_COUNT*sizeof(*indices));
    if(!indices) abort();
    YglFillQuadIndices(indices,YGL_QUAD_INDEX_COUNT);
    glGenBuffers(1,&vita_quad_index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,vita_quad_index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,YGL_QUAD_INDEX_COUNT*sizeof(*indices),indices,GL_STATIC_DRAW);
    free(indices);
    if(!vita_quad_index_buffer || glGetError()!=GL_NO_ERROR) abort();
  } else glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,vita_quad_index_buffer);
  glDrawRangeElements(GL_TRIANGLES,0,count-1,count,GL_UNSIGNED_SHORT,NULL);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,previous);
}
