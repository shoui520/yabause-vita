/* SPDX-License-Identifier: GPL-2.0-or-later
 * CPU-owned line tables, copied by vitaGL rather than mapped as desktop PBOs.
 * Values retain the inherited renderer's packed per-line metadata unchanged.
 */
static void YglVitaCheckLineSize(int count, int depth) {
  if (count < 1 || count > 512 || depth < 1 || depth > 512) {
    fprintf(stderr, "Invalid Vita line table %d x %d\n", count, depth);
    abort();
  }
}

static void YglVitaUploadLines(GLuint texture, const u32 *pixels,
                               int count, int depth) {
  YglVitaCheckLineSize(count, depth);
  if (!pixels || !texture) abort();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  /* Data is tightly packed at count words per row, not the 512-texel
   * destination stride. This matters for multi-row per-line tables.
   */
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, depth,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    fprintf(stderr, "Vita line upload failed (GL %x)\n", (unsigned)error);
    abort();
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

u32 *YglGetLineColorPointer(void) {
  if (!_Ygl->lincolor_buf) _Ygl->lincolor_buf = YglVitaAtlasStorage(512, 1);
  if (!_Ygl->lincolor_tex) {
    glActiveTexture(GL_TEXTURE0);
    _Ygl->lincolor_tex = YglVitaAtlasTexture(512, 1);
  }
  return _Ygl->lincolor_buf;
}

void YglSetLineColor(u32 *pixels, int count) {
  YglVitaUploadLines(_Ygl->lincolor_tex, pixels, count, 1);
}

u32 *YglGetBackColorPointer(void) {
  if (!_Ygl->backcolor_buf) _Ygl->backcolor_buf = YglVitaAtlasStorage(512, 1);
  if (!_Ygl->back_tex) {
    glActiveTexture(GL_TEXTURE0);
    _Ygl->back_tex = YglVitaAtlasTexture(512, 1);
  }
  return _Ygl->backcolor_buf;
}

void YglSetBackColor(int count) {
  YglVitaUploadLines(_Ygl->back_tex, _Ygl->backcolor_buf, count, 1);
}

u32 *YglGetPerlineBuf(YglPerLineInfo *line, int count, int depth) {
  YglVitaCheckLineSize(count, depth);
  if (line->vita_depth != depth || !line->lincolor_buf || !line->lincolor_tex) {
    u32 *pixels = YglVitaAtlasStorage(512, depth);
    glActiveTexture(GL_TEXTURE0);
    GLuint texture = YglVitaAtlasTexture(512, depth);
    if (line->lincolor_tex) glDeleteTextures(1, &line->lincolor_tex);
    free(line->lincolor_buf);
    line->lincolor_buf = pixels;
    line->lincolor_tex = texture;
    line->vita_depth = depth;
  }
  return line->lincolor_buf;
}

void YglSetPerlineBuf(YglPerLineInfo *line, u32 *pixels, int count, int depth) {
  if (depth != line->vita_depth || pixels != line->lincolor_buf) abort();
  YglVitaUploadLines(line->lincolor_tex, pixels, count, depth);
}
