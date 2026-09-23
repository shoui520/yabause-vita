/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "utility_v.h"
#include "back_f.h"
#include "copy_f.h"
#include "copy_written_f.h"
#include "blur_f.h"
#include "mosaic_f.h"
#include "perline_f.h"

enum YglVitaUtility {
  VITA_BACK, VITA_COPY, VITA_COPY_WRITTEN,
  VITA_BLUR, VITA_MOSAIC, VITA_PERLINE
};

static int YglVitaUtilityProgram(enum YglVitaUtility kind) {
  const unsigned char *fragment = NULL;
  size_t fragment_size = 0;
  const int matrix = kind >= VITA_BLUR;
  switch (kind) {
#define BINARY(which, name) case which: fragment = ygl_vita_##name; \
  fragment_size = sizeof(ygl_vita_##name); break
  BINARY(VITA_BACK, back_f);
  BINARY(VITA_COPY, copy_f);
  BINARY(VITA_COPY_WRITTEN, copy_written_f);
  BINARY(VITA_BLUR, blur_f);
  BINARY(VITA_MOSAIC, mosaic_f);
  BINARY(VITA_PERLINE, perline_f);
#undef BINARY
  default: return -1;
  }
  GLuint program = glCreateProgram();
  GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  GLuint pixel = glCreateShader(GL_FRAGMENT_SHADER);
  GLint status = GL_FALSE;
  if (!program || !vertex || !pixel) goto fail;
  if (matrix)
    YglVitaLoadGxp(vertex, ygl_vita_framebuffer_v,
                  sizeof(ygl_vita_framebuffer_v), "u_mvpMatrix");
  else
    YglVitaLoadGxp(vertex, ygl_vita_utility_v, sizeof(ygl_vita_utility_v), NULL);
  YglVitaLoadGxp(pixel, fragment, fragment_size, NULL);
  glGetShaderiv(vertex, GL_COMPILE_STATUS, &status);
  if (!status) goto fail;
  glGetShaderiv(pixel, GL_COMPILE_STATUS, &status);
  if (!status) goto fail;
  glAttachShader(program, vertex);
  glAttachShader(program, pixel);
  glBindAttribLocation(program, 0, matrix ? "a_position" : "aPosition");
  glBindAttribLocation(program, 1, matrix ? "a_texcoord" : "aTexCoord");
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    char log[512];
    GLsizei length = 0;
    glGetProgramInfoLog(program, sizeof(log), &length, log);
    YGLLOG("Native utility shader %d link failed: %.*s\n", kind, (int)length, log);
    goto fail;
  }
  glDeleteShader(vertex);
  glDeleteShader(pixel);
  return (int)program;
fail:
  YGLLOG("Native utility shader %d initialization failed\n", kind);
  if (vertex) glDeleteShader(vertex);
  if (pixel) glDeleteShader(pixel);
  if (program) glDeleteProgram(program);
  return -1;
}
