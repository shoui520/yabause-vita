/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic pattern tables with independently constructed expected pixels.
 * Manual figures 4.7/4.8/4.10: cell order, pages and supplementary name bits. */
static unsigned YglVitaValidateRotationTiles(YglTextureManager *tm,
                                             unsigned char *source, u32 *output) {
  unsigned total = 0;
  for (unsigned format = 0; format < 2; ++format)
  for (unsigned cells = 1; cells <= 2; ++cells)
  for (unsigned nameMode = 0; nameMode < 3; ++nameMode)
  for (unsigned transparency = 0; transparency < 2; ++transparency) {
    unsigned words = nameMode == 2 ? 2 : 1, size = cells * 8;
    unsigned pageWidth = 512 / size, unitCount = cells*cells*(format+1);
    memset(source, 0, 0x80000);
    RBGDrawInfo r = {0};
    r.info.colornumber = format; r.info.patternwh = cells;
    r.info.patterndatasize = words; r.info.auxmode = nameMode == 1;
    r.info.supplementdata = 0x308; r.info.planew = 1;
    r.info.coloroffset = 0x100; r.info.alpha = 173;
    r.info.specialprimode = 2; r.info.priority = 5;
    r.info.specialcolormode = 2; r.info.specialcode = 0x5a;
    r.info.transparencyenable = transparency;
    r.rotate_mval_h = r.rotate_mval_v = 1; r.hres = r.vres = 64;
    vdp2rotationparameter_struct p = {0};
    p.dx = p.kx = p.ky = 1; p.ShiftPaneX = p.ShiftPaneY = 9;
    p.MaxH = p.MaxV = 2048;
    for (unsigned plane = 0; plane < 16; ++plane) {
      p.PlaneAddrv[plane] = plane * 0x4000;
      for (unsigned entry = 0; entry < pageWidth*pageWidth; ++entry) {
        unsigned tile = (entry+plane)&3, flip = (entry>>2)&3;
        unsigned character = 0x2000 + tile*unitCount;
        unsigned palette = tile*(format ? 16 : 1);
        unsigned name;
        if (words == 2) {
          name = (flip<<14) | (((entry>>3)&1)<<13) | (((entry>>4)&1)<<12) | palette;
        } else {
          name = cells == 1 ? character & (nameMode ? 0xfff : 0x3ff) :
                                (character>>2) & (nameMode ? 0xfff : 0x3ff);
          name |= palette << (format ? 8 : 12);
          if (!nameMode) name |= flip << 10;
        }
        unsigned address = p.PlaneAddrv[plane] + entry*words*2;
        source[address] = name>>8; source[address+1] = name;
        if (words == 2) { source[address+2] = character>>8; source[address+3] = character; }
      }
    }
    for (unsigned tile = 0; tile < 4; ++tile)
    for (unsigned y = 0; y < size; ++y)
    for (unsigned x = 0; x < size; ++x) {
      unsigned value = (x*3+y*5+tile*7) & (format ? 255 : 15);
      unsigned linear = ((y/8)*2+x/8)*64 + (y%8)*8 + x%8;
      unsigned address = (0x2000+tile*unitCount)*32 + (format ? linear : linear/2);
      if (format) source[address] = value;
      else source[address] |= value << ((linear%2) ? 0 : 4);
    }
    /* Cross four planes and page edges; the other pass also wraps negatives. */
    int origin = transparency ? -16 : 496;
    p.Xsp = origin;
    int slot = YglVitaRotationBegin(&r, &p, source);
    if (slot < 0) abort();
    for (unsigned y = 0; y < 64; ++y) {
      p.Ysp = origin+(int)y; YglVitaRotationRow(slot, y, &p);
    }
    YglVitaRotationFlush(tm);
    glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
    /* Atlas coordinates address stored rows; vitaGL readback uses bottom Y
     * then copies rows forward (framebuffers.c glReadPixels fast_store). */
    glReadPixels(0, tm->height-64, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, output);
    unsigned mismatches = 0;
    for (unsigned y = 0; y < 64; ++y)
    for (unsigned x = 0; x < 64; ++x) {
      unsigned h = (origin+(int)x)&2047, v = (origin+(int)y)&2047;
      unsigned plane = h/512+(v/512)*4;
      unsigned entry = ((v%512)/size)*pageWidth+(h%512)/size;
      unsigned tile = (entry+plane)&3, flip = nameMode == 1 ? 0 : (entry>>2)&3;
      unsigned tx = h%size, ty = v%size;
      if (flip&1) tx = size-1-tx;
      if (flip&2) ty = size-1-ty;
      unsigned value = (tx*3+ty*5+tile*7)&(format ? 255 : 15);
      unsigned code = 0x100 + ((tile*(format ? 256 : 16)) | value);
      unsigned special = (0x5a >> ((value&15)/2))&1;
      unsigned pr = words == 1 ? 1 : (entry>>3)&1;
      unsigned cc = words == 1 ? 1 : (entry>>4)&1;
      unsigned alpha = cc && special ? 173 : 255;
      u32 expected = code | ((4 | (pr&special))<<16) | (alpha<<24);
      if (transparency && value == 0) expected = 0;
      if (output[y*64+x] != expected) {
        if (mismatches < 2) YuiMsg("rotation_tile_sample x=%u y=%u expected=%08x actual=%08x", x, y, expected, output[y*64+x]);
        ++mismatches;
      }
    }
    GLenum error = glGetError();
    YuiMsg("rotation_tile_test format=%u cells=%u name_mode=%u transparency=%u pixels=4096 mismatches=%u gl_error=%04x",
      format, cells, nameMode, transparency, mismatches, error);
    total += mismatches + (error != GL_NO_ERROR);
    YglVitaRotationReset();
  }
  return total;
}
