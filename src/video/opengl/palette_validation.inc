/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Opt-in startup diagnostic, before guest rendering. Exercise the production
 * shaders with all 2048 palette addresses, independently of guest geometry.
 * Readback is deliberately excluded from normal runs and timing measurements. */
#include "normal_reference_f.h"
#include "atlas_snapshot_test.h"
int YglVitaValidatePalette(void) {
  unsigned failures=0;
  unsigned equality_cases=0;
  unsigned equality_errors=YglAtlasEqualityTest(&equality_cases);
  YuiMsg("atlas_equality_test cases=%u mismatches=%u",equality_cases,equality_errors);
  failures+=equality_errors!=0;
  GLuint tex[3], fbo;
  u32 input[2048], palette[2048], output[2048];
  const float positions[] = {-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};
  const float coords[] = {0,0, 64,0, 0,32, 0,32, 64,0, 64,32};
  const float identity[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  GLint old_viewport[4], old_fbo, old_program, old_active, bindings[2];
  const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE};
  GLboolean enabled[5];
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
  for (unsigned i=0; i<2; ++i) {
    glActiveTexture(GL_TEXTURE0+i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bindings[i]);
  }
  for (unsigned i=0; i<5; ++i) { enabled[i]=glIsEnabled(caps[i]); glDisable(caps[i]); }
  for (unsigned i=0; i<2048; ++i) {
    input[i]=0xff000000u | i;
    palette[i]=0xff000000u | (i & 255) | ((i >> 8)*31 << 8) | (((i >> 3)&255) << 16);
  }
  glActiveTexture(GL_TEXTURE0);
  glGenTextures(3, tex);
  for (unsigned i=0; i<3; ++i) {
    glBindTexture(GL_TEXTURE_2D,tex[i]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,i==1?2048:64,i==1?1:32,0,
                 GL_RGBA,GL_UNSIGNED_BYTE,i==0?input:i==1?palette:NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  glGenFramebuffers(1,&fbo);
  glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[2],0);
  GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status==GL_FRAMEBUFFER_COMPLETE) {
    glViewport(0,0,64,32);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,positions);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,0,coords);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,tex[1]);
    unsigned normal_passes=2;
#ifdef VITA_VDP2_OPAQUE_RUNS
    normal_passes=4;
#endif
    for (unsigned pass=0; pass<normal_passes; ++pass) {
      unsigned pal=pass&1;
      GLuint prg=_prgid[pal?PG_VDP2_NORMAL_CRAM:PG_NORMAL];
#ifdef VITA_VDP2_OPAQUE_RUNS
      if(pass>=2) prg=YglOpaqueProgram(pal);
      if(!prg) { ++failures; continue; }
#endif
      glUseProgram(prg);
      glUniformMatrix4fv(glGetUniformLocation(prg,"u_mvpMatrix"),1,GL_FALSE,identity);
      glUniform2f(glGetUniformLocation(prg,"u_texsize"),64,32);
      glUniform4f(glGetUniformLocation(prg,"u_color_offset"),0,0,0,0);
      glUniform1i(glGetUniformLocation(prg,"s_texture"),0);
      if (pal) glUniform1i(glGetUniformLocation(prg,"s_color"),1);
      glDrawArrays(GL_TRIANGLES,0,6);
      glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,output);
      unsigned direct=0, flipped=0;
      const u32 *expected=pal?palette:input;
      for (unsigned i=0; i<2048; ++i) {
        direct += output[i]!=expected[i];
        flipped += output[i]!=expected[(31-i/64)*64+i%64];
        if (pass && output[i]!=expected[i] && direct<=8)
          YuiMsg("palette_shader_sample index=%u expected=%08x actual=%08x",i,expected[i],output[i]);
      }
      YuiMsg("palette_shader_test pass=%u mismatches=%u flipped=%u first=%08x last=%08x gl_error=%04x",
        pass,direct,flipped,output[0],output[2047],glGetError());
      failures+=direct!=0;
    }
    /* Compare real GPU sampling against the original fragment-side conversion.
     * Fractional edges, flips, scaling and NPOT height expose interpolation and
     * Cg integer-conversion differences that a title screenshot cannot prove. */
    GLuint vs=glCreateShader(GL_VERTEX_SHADER), fs=glCreateShader(GL_FRAGMENT_SHADER);
    GLuint reference=glCreateProgram();
    YglVitaLoadGxp(vs,ygl_vita_normal_v,sizeof(ygl_vita_normal_v),"u_mvpMatrix");
    YglVitaLoadGxp(fs,ygl_vita_normal_reference_f,sizeof(ygl_vita_normal_reference_f),NULL);
    glAttachShader(reference,vs); glAttachShader(reference,fs);
    glBindAttribLocation(reference,0,"a_position");
    glBindAttribLocation(reference,1,"a_texcoord");
    glLinkProgram(reference);
    GLint linked=0; glGetProgramiv(reference,GL_LINK_STATUS,&linked);
    unsigned errors=0, cases=0;
    u32 reference_pixels[2048];
    const float edges[][4]={{0,0,64,32},{0.25f,0.25f,63.25f,31.25f},
      {0.5f,0.5f,63.5f,31.5f},{0.75f,0.75f,63.75f,31.75f},
      {64,32,0,0},{3.125f,2.875f,45.625f,19.375f},
      {-2,-2,66,34},{8,8,16,16},{0,0,128,64}};
    if (linked) for(unsigned height=24;height<=32;height+=8) {
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex[0]);
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,height,0,GL_RGBA,GL_UNSIGNED_BYTE,input);
      for(unsigned e=0;e<sizeof(edges)/sizeof(edges[0]);++e) {
        float x0=edges[e][0], y0=edges[e][1], x1=edges[e][2], y1=edges[e][3];
        float uv[]={x0,y0,x1,y0,x0,y1,x0,y1,x1,y0,x1,y1};
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,0,uv);
        for(unsigned p=0;p<2;++p) {
          GLuint program=p?_prgid[PG_NORMAL]:reference;
          glUseProgram(program);
          glUniformMatrix4fv(glGetUniformLocation(program,"u_mvpMatrix"),1,GL_FALSE,identity);
          glUniform2f(glGetUniformLocation(program,"u_texsize"),64,height);
          glUniform4f(glGetUniformLocation(program,"u_color_offset"),0,0,0,0);
          glUniform1i(glGetUniformLocation(program,"s_texture"),0);
          glDrawArrays(GL_TRIANGLES,0,6);
          glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,p?output:reference_pixels);
        }
        unsigned mismatches=0;
        for(unsigned i=0;i<2048;++i) mismatches+=output[i]!=reference_pixels[i];
        if(mismatches) YuiMsg("normal_sampling_case height=%u edge=%u mismatches=%u",height,e,mismatches);
        errors+=mismatches; ++cases;
      }
    }
    YuiMsg("normal_sampling_test linked=%d cases=%u mismatches=%u",linked,cases,errors);
    failures+=!linked || errors!=0 || cases!=18;
    glUseProgram(0); glDeleteProgram(reference); glDeleteShader(vs); glDeleteShader(fs);
    /* Exercise the actual framebuffer program and its production upload, not
     * just a mocked GL call. Every metadata index selects a distinct priority
     * and alpha; eight disjoint priority intervals detect bad array packing. */
    UniformFrameBuffer saved_fb=_Ygl->fbu_;
    memset(&_Ygl->fbu_,0,sizeof(_Ygl->fbu_));
    for(unsigned i=0;i<8;++i) {
      _Ygl->fbu_.u_pri[i*4]=(float)((3*i+1)&7)/10.0f+0.05f;
      _Ygl->fbu_.u_alpha[i*4]=(float)(255-8*i)/255.0f;
    }
    for(unsigned i=0;i<2048;++i)
      input[i]=((0x80u|(i&63))<<24)|0x00345678u;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex[0]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,32,0,GL_RGBA,GL_UNSIGNED_BYTE,input);
    const float fb_uv[]={0,0,1,0,0,1,0,1,1,0,1,1};
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,0,fb_uv);
    GLuint fb_program=_prgid[PG_VDP2_DRAWFRAMEBUFF_DESTALPHA];
    glUseProgram(fb_program);
    glUniformMatrix4fv(glGetUniformLocation(fb_program,"u_mvpMatrix"),1,GL_FALSE,identity);
    glUniform1i(glGetUniformLocation(fb_program,"s_vdp1FrameBuffer"),0);
    YglVitaBindFramebufferUniforms(PG_VDP2_DRAWFRAMEBUFF_DESTALPHA);
    GLfloat old_clear[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE,old_clear);
    unsigned fb_errors=0;
    for(unsigned k=0;k<8;++k) {
      glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
      glUniform1f(glGetUniformLocation(fb_program,"u_from"),(float)k/10.0f);
      glUniform1f(glGetUniformLocation(fb_program,"u_to"),(float)(k+1)/10.0f);
      glDrawArrays(GL_TRIANGLES,0,6);
      glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,output);
      unsigned errors=0;
      for(unsigned i=0;i<2048;++i) {
        unsigned index=i&7;
        unsigned alpha_index=(i>>3)&7;
        u32 expected=((3*index+1)&7)==k?((255-8*alpha_index)<<24)|0x00345678u:0;
        errors+=output[i]!=expected;
      }
      fb_errors+=errors;
      if(errors) YuiMsg("framebuffer_uniform_case priority=%u mismatches=%u",k,errors);
    }
    YuiMsg("framebuffer_uniform_test pixels=16384 mismatches=%u gl_error=%04x",fb_errors,glGetError());
    failures+=fb_errors!=0;
    glClearColor(old_clear[0],old_clear[1],old_clear[2],old_clear[3]);
    _Ygl->fbu_=saved_fb;
    YglVitaBindFramebufferUniforms(PG_VDP2_DRAWFRAMEBUFF_DESTALPHA);
  } else { YuiMsg("palette_shader_test framebuffer=%04x",status); ++failures; }
  glFinish();
  glBindFramebuffer(GL_FRAMEBUFFER,old_fbo);
  glDeleteFramebuffers(1,&fbo); glDeleteTextures(3,tex);
  for (unsigned i=0; i<2; ++i) { glActiveTexture(GL_TEXTURE0+i); glBindTexture(GL_TEXTURE_2D,bindings[i]); }
  glActiveTexture(old_active); glUseProgram(old_program);
  glViewport(old_viewport[0],old_viewport[1],old_viewport[2],old_viewport[3]);
  for (unsigned i=0; i<5; ++i) if (enabled[i]) glEnable(caps[i]);
  if(failures) YuiMsg("composition_validation_failed groups=%u",failures);
  return failures?-1:0;
}
