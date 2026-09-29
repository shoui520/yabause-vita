/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <limits.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <vitashark.h>

static GLuint YglVitaCreateCgShader(GLenum type) {
  if (type == GL_VERTEX_SHADER) type = GL_CG_VERTEX_SHADER_EXT;
  else if (type == GL_FRAGMENT_SHADER) type = GL_CG_FRAGMENT_SHADER_EXT;
  return glCreateShader(type);
}

/* All compiler calls run on the current graphics owner. vitaGL owns the
 * compiler lifetime and records matrix metadata from Cg reflection. */
static void YglVitaCgLog(const char *message, shark_log_level level, int line) {
  YuiMsg("runtime_shader_diagnostic level=%d line=%d message=%s",
         (int)level, line, message ? message : "");
}

static void YglVitaCompileCg(GLuint shader, const unsigned char *source, size_t size) {
  if (!shader || !source || size < 2 || size > INT_MAX || source[size - 1] ||
      strlen((const char *)source) != size - 1) {
    YuiMsg("fatal: invalid embedded Cg source");
    abort();
  }
  GLint type = 0, compiled = GL_FALSE;
  glGetShaderiv(shader, GL_SHADER_TYPE, &type);
  if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
    YuiMsg("fatal: invalid runtime shader type");
    abort();
  }
  uint32_t identity = UINT32_C(2166136261);
  for (size_t i = 0; i + 1 < size; ++i)
    identity = (identity ^ source[i]) * UINT32_C(16777619);
  /* Do not inherit vitaGL's default fast-math policy for these native shaders.
   * This preserves requested precision, not binary equivalence to psp2cgc. */
  vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_FALSE, GL_FALSE, GL_FALSE);
  shark_install_log_cb(YglVitaCgLog);
  const uint64_t begin = sceKernelGetProcessTimeWide();
  const GLchar *text = (const GLchar *)source;
  const GLint length = (GLint)size - 1;
  YuiMsg("runtime_shader_begin source=%08x type=%s bytes=%u start_us=%llu",
         identity, type == GL_VERTEX_SHADER ? "vertex" : "fragment",
         (unsigned)length, (unsigned long long)begin);
  glShaderSource(shader, 1, &text, &length);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  const uint64_t end = sceKernelGetProcessTimeWide();
  YuiMsg("runtime_shader_complete source=%08x status=%s elapsed_us=%llu end_us=%llu",
         identity, compiled ? "PASS" : "FAIL", (unsigned long long)(end - begin),
         (unsigned long long)end);
  if (!compiled) {
    YuiMsg("fatal: runtime Cg compilation failed source=%08x", identity);
    abort();
  }
}

/* This translation unit's Vita paths all load native Cg, including rotation,
 * utility and palette programs. Standard GL kinds select GLSL in pinned VitaGL;
 * changing only glShaderBinary to glShaderSource would invoke its translator. */
#define glCreateShader(type) YglVitaCreateCgShader(type)
