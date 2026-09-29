/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host contract for the production loader's GL calls and failure handling.
 * These doubles do not compile Cg or certify GPU output. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime_shader_stubs/vitashark.h"

typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLint;
typedef char GLchar;
#define GL_FALSE 0
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
#define GL_CG_VERTEX_SHADER_EXT 0x7001
#define GL_CG_FRAGMENT_SHADER_EXT 0x7002
#define GL_SHADER_TYPE 0x8b4f
#define GL_COMPILE_STATUS 0x8b81

static jmp_buf escape;
static int shader_type, compile_ok, source_calls, compile_calls, fatal_logs;
static int option_calls, completed_logs, start_logs;
static int create_calls, is_glsl;
static GLenum requested_kind;
static uint64_t clock_value;
static const unsigned char *expected_source;
static void (*logger)(const char *, shark_log_level, int);

static void YuiMsg(const char *format, ...);
static GLuint glCreateShader(GLenum);
static void glGetShaderiv(GLuint, unsigned, GLint *);
static void glShaderSource(GLuint, int, const GLchar *const *, const GLint *);
static void glCompileShader(GLuint);
_Noreturn static void reject(void) { longjmp(escape, 1); }
#define abort() reject()
#include "../src/video/opengl/runtime_shader_vita.inc"
static GLuint production_call_site(GLenum type) { return glCreateShader(type); }
#undef glCreateShader
#undef abort

static GLuint glCreateShader(GLenum type) {
  requested_kind = type; ++create_calls;
  is_glsl = type != GL_CG_VERTEX_SHADER_EXT && type != GL_CG_FRAGMENT_SHADER_EXT;
  shader_type = type == GL_CG_VERTEX_SHADER_EXT ? GL_VERTEX_SHADER :
                type == GL_CG_FRAGMENT_SHADER_EXT ? GL_FRAGMENT_SHADER : (int)type;
  return 7;
}

static void YuiMsg(const char *format, ...) {
  if (!strncmp(format, "fatal:", 6)) ++fatal_logs;
  if (!strncmp(format, "runtime_shader_begin", 20)) ++start_logs;
  if (!strncmp(format, "runtime_shader_complete", 23)) ++completed_logs;
}
static void glGetShaderiv(GLuint handle, unsigned name, GLint *result) {
  assert(handle == 7);
  if (name == GL_SHADER_TYPE) *result = shader_type;
  else { assert(name == GL_COMPILE_STATUS); *result = compile_ok && !is_glsl; }
}
static void glShaderSource(GLuint handle, int count, const GLchar *const *strings, const GLint *length) {
  assert(handle == 7 && count == 1 && strings && length);
  assert((const unsigned char *)strings[0] == expected_source);
  assert(*length == (GLint)strlen((const char *)expected_source));
  ++source_calls;
}
static void glCompileShader(GLuint handle) { assert(handle == 7); ++compile_calls; }
void vglSetupRuntimeShaderCompiler(shark_opt level, int32_t math, int32_t precision, int32_t integers) {
  assert(level == SHARK_OPT_FAST && !math && !precision && !integers);
  ++option_calls;
}
void shark_install_log_cb(void (*callback)(const char *, shark_log_level, int)) { logger = callback; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value += 10; }

static void reset(void) {
  shader_type = GL_VERTEX_SHADER; compile_ok = 1;
  source_calls = compile_calls = fatal_logs = option_calls = completed_logs = start_logs = 0;
  create_calls = is_glsl = 0; requested_kind = 0;
  clock_value = 0; logger = NULL;
}
int main(void) {
  const unsigned char source[] = "// self-contained Cg\nfloat value;\n";
  expected_source = source;
  for (unsigned stage = 0; stage < 2; ++stage) {
    reset(); shader_type = stage ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
    GLuint shader = production_call_site((GLenum)shader_type);
    assert(create_calls == 1 && !is_glsl);
    assert(requested_kind == (stage ? GL_CG_FRAGMENT_SHADER_EXT : GL_CG_VERTEX_SHADER_EXT));
    YglVitaCompileCg(shader, source, sizeof(source));
    assert(source_calls == 1 && compile_calls == 1 && option_calls == 1);
    assert(start_logs == 1 && completed_logs == 1 && !fatal_logs && logger);
  }
  const unsigned char interior_nul[] = {'x', 0, 'y', 0};
  for (unsigned failure = 0; failure < 7; ++failure) {
    reset();
    if (setjmp(escape) == 0) {
      switch (failure) {
      case 0: YglVitaCompileCg(0, source, sizeof(source)); break;
      case 1: YglVitaCompileCg(7, NULL, sizeof(source)); break;
      case 2: YglVitaCompileCg(7, source, sizeof(source) - 1); break;
      case 3: YglVitaCompileCg(7, interior_nul, sizeof(interior_nul)); break;
      case 4: YglVitaCompileCg(7, source, (size_t)INT_MAX + 1); break;
      case 5: shader_type = -1; YglVitaCompileCg(7, source, sizeof(source)); break;
      default: compile_ok = 0; YglVitaCompileCg(7, source, sizeof(source)); break;
      }
      assert(!"invalid shader accepted");
    }
    assert(fatal_logs == 1);
    assert(compile_calls == (failure == 6 ? 1 : 0));
  }
  /* Negative control: bypassing the Cg factory reproduces the old GLSL route. */
  reset();
  if (setjmp(escape) == 0) {
    YglVitaCompileCg(glCreateShader(GL_VERTEX_SHADER), source, sizeof(source));
    assert(!"GLSL-marked Cg accepted");
  }
  assert(is_glsl && compile_calls == 1 && fatal_logs == 1);
  puts("runtime_shader_loader: 2 Cg creation/compile contracts and 8 rejection cases passed");
  return 0;
}
