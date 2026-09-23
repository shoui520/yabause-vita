/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Capture-mode startup test of the production GPU path, not timed gameplay. */
#include "rotation_tile_validation.inc"
#if defined(VITA_ROTATION_VARIANTS) || defined(VITA_ROTATION_ROWS) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE)
/* Compare every immutable format/layout specialization against the unchanged
 * generic shader, in addition to the independent pixel oracles below. */
static unsigned YglVitaValidateRotationVariants(YglTextureManager *tm,
                                                unsigned char *source, u32 *output) {
  u32 *reference = malloc(64 * 64 * sizeof(*reference));
  if (!reference) abort();
  unsigned mismatches = 0, pixels = 0;
  unsigned random = 0x163a942b;
  for (unsigned i=0; i<0x80000; ++i) {
    random = random * 1664525u + 1013904223u;
    source[i] = random >> 24;
  }
  const int formats[] = {0,1,3,4}, overs[] = {0,2,3};
  for (unsigned f=0; f<4; ++f) for (unsigned layout=0; layout<7; ++layout)
  for (unsigned mode=0; mode<3; ++mode) {
    RBGDrawInfo r = {0};
    r.info.colornumber = formats[f]; r.info.isbitmap = layout == 0;
    r.info.patternwh = layout ? 1 + (layout-1)/3 : 1;
    unsigned name = layout ? (layout-1)%3 : 0;
    r.info.patterndatasize = name == 2 ? 2 : 1; r.info.auxmode = name == 1;
    r.info.supplementdata = 0x319; r.info.planew = 1;
    r.info.cellw = 512; r.info.cellh = 256; r.info.alpha = 173;
    r.info.transparencyenable = mode != 0;
    r.info.coloroffset = 0x100; r.info.paladdr = 0x20;
    r.info.specialprimode = 2; r.info.priority = 5;
    r.info.specialcolormode = 2; r.info.specialcode = 0x5a;
    r.rotate_mval_h = r.rotate_mval_v = 1; r.hres = r.vres = 64;
    vdp2rotationparameter_struct p = {0};
    p.screenover = overs[mode]; p.charaddr = 0x60000;
    p.dx = 1.25f; p.dy = -0.125f; p.kx = 0.75f; p.ky = 1.25f;
    p.Xsp = mode == 1 ? -16.5f : 496.25f;
    p.ShiftPaneX = p.ShiftPaneY = 9; p.MaxH = p.MaxV = 2048;
    for (unsigned i=0; i<16; ++i) p.PlaneAddrv[i] = i*0x4000;
#ifdef VITA_ROTATION_MAP_CACHE
    /* Small independent fixture: exercise real warmup, decode and hits without
     * allocating a full gameplay map for every layout. */
    p.MaxH = p.MaxV = 512;
    int alias_fixture = f == 1 && (layout == 4 || layout == 5) && mode != 2;
    int wide_fixture = (f == 1 && layout == 6 && mode == 1) || alias_fixture;
    if (wide_fixture) p.MaxH = p.MaxV = 2048;
    if (alias_fixture) for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
      p.PlaneAddrv[y*4+x] = layout == 4 ? 0 : ((y%2)*2+x%2)*0x4000;
    unsigned cached_before = vita_rotation_map_hits + vita_rotation_map_misses;
    const unsigned passes = 11;
#elif defined(VITA_ROTATION_PATTERN_CACHE)
    // Generic oracle, cold descriptor build, then reuse with unchanged source.
    r.vram_generation=1+2*(f*21+layout*3+mode);
    unsigned pattern_hits_before=vita_rotation_pattern_hits;
    unsigned pattern_builds_before=vita_rotation_pattern_builds;
#ifdef VITA_ROTATION_COEFF_PAIR
    unsigned pair_before=vita_rotation_pair_jobs;
#endif
    int change_source=f == 1 && layout == 2 && mode == 0;
    const unsigned passes = change_source ? 8 : 4;
#else
    const unsigned passes = 2;
#endif
    for (unsigned pass=0; pass<passes; ++pass) {
      vita_rotation_validate_generic = pass == 0;
#ifdef VITA_ROTATION_PATTERN_CACHE
      if (change_source && pass == 4) {
        // Same map metadata, different VRAM: stale cached names must not hit.
        ++r.vram_generation;
        for(unsigned i=0;i<0x80000;++i) source[i]^=0x5a;
        vita_rotation_validate_generic=1;
      }
#endif
      YglVitaRotationReset();
      int slot = YglVitaRotationBegin(&r, &p, source);
      if (slot < 0) abort();
      for (unsigned y=0; y<64; ++y) {
        /* Non-dyadic coefficients and discontinuous adjacent rows expose
         * precision loss or interpolation across Saturn scanlines. */
        p.Xsp = (mode == 1 ? -16.5f : 496.25f) + (y%9)*0.1234567f;
        p.kx = 0.71234567f + (y%7)*0.031234f;
        p.ky = 1.1234567f - (y%5)*0.0154321f;
        p.Ysp = (float)y + (mode == 1 ? -8.25f : 480.5f);
#if defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE) || (defined(VITA_ROTATION_VARIANTS) && defined(VITA_ROTATION_UNIFORMS))
        /* Exercise the admitted constant-k shader, with fractional row
         * origins that cannot survive half-precision coefficient delivery. */
#if defined(VITA_ROTATION_VARIANTS) && defined(VITA_ROTATION_UNIFORMS)
        /* One varying-row fixture must retain the unspecialized fallback;
         * other modes still exercise this format/layout specialization. */
        if (!(f == 0 && layout == 0 && mode == 2))
#endif
        { p.kx = 0.71234567f; p.ky = 1.1234567f; }
#ifdef VITA_ROTATION_MAP_CACHE
        if (wide_fixture) {
          p.Xsp = 1536.1234567f + (y%9)*0.1234567f;
          p.Ysp = 1536.5f + y;
        }
#endif
        YglVitaRotationRow(slot, y, &p);
#else
        YglVitaRotationRow(slot, y, y == 7 ? NULL : &p);
#endif
      }
      YglVitaRotationFlush(tm);
      glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
      if (vita_rotation_validate_generic || pass == passes-1)
        glReadPixels(0, tm->height-64, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE,
                     vita_rotation_validate_generic ? reference : output);
#if defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
      /* These warmup draws model separate frames, not hundreds of uploads in
       * one frame. vitaGL advances deferred retirement/circular storage at
       * SwapBuffers; glReadPixels alone does not advance that lifecycle. */
      glBindFramebuffer(GL_FRAMEBUFFER,0);
      glClear(GL_COLOR_BUFFER_BIT);
      vglSwapBuffers(GL_FALSE);
#endif
    }
    for (unsigned i=0; i<64*64; ++i) if (reference[i] != output[i]) {
      if (mismatches < 4)
        YuiMsg("rotation_variant_sample format=%d layout=%u mode=%u pixel=%u reference=%08x actual=%08x",
               formats[f],layout,mode,i,reference[i],output[i]);
      ++mismatches;
    }
#ifdef VITA_ROTATION_MAP_CACHE
    if (layout && mode != 2 &&
        cached_before == vita_rotation_map_hits + vita_rotation_map_misses) {
      YuiMsg("rotation_map_test_not_exercised format=%d layout=%u mode=%u",formats[f],layout,mode);
      ++mismatches;
    }
#endif
    pixels += 64*64;
#ifdef VITA_ROTATION_COEFF_PAIR
    if (layout && mode != 2 && vita_rotation_pair_jobs==pair_before) {
      YuiMsg("rotation_pair_test_not_exercised format=%d layout=%u mode=%u",
          formats[f],layout,mode);
      ++mismatches;
    }
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
    if (layout && mode != 2 &&
        (vita_rotation_pattern_hits-pattern_hits_before < (change_source ? 2u : 1u) ||
         vita_rotation_pattern_builds-pattern_builds_before < (change_source ? 2u : 1u))) {
      YuiMsg("rotation_pattern_test_not_exercised format=%d layout=%u mode=%u",
          formats[f],layout,mode);
      ++mismatches;
    }
#endif
  }
  vita_rotation_validate_generic = 0;
  YglVitaRotationReset();
  GLenum error = glGetError();
  YuiMsg("rotation_variants_test variants=28 pixels=%u mismatches=%u gl_error=%04x",
         pixels, mismatches, error);
  free(reference);
  return mismatches + (error != GL_NO_ERROR);
}
#endif
/* Two distinct input generations before a readback: unlike one-job tests,
 * this exercises replacement while an earlier draw may still be in flight. */
static unsigned YglVitaValidateRotationReplacement(YglTextureManager *tm,
                                                   unsigned char *source,
                                                   u32 *output) {
  YglVitaRotationReset();
  for (unsigned job = 0; job < 2; ++job) {
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 512; ++x) {
      unsigned a = (0x60000 + (y * 512 + x) * 2) & 0x7ffff;
      unsigned word = 0x8000 | ((x + y * 3 + job * 17011) & 0x7fff);
      source[a] = word >> 8; source[a + 1] = word;
    }
    RBGDrawInfo r = {0};
    r.info.isbitmap = 1; r.info.colornumber = 3;
    r.info.cellw = 512; r.info.cellh = 256; r.info.alpha = 173;
    r.rotate_mval_h = r.rotate_mval_v = 1;
    r.hres = 256; r.vres = 128; r.c.x = job * 256;
    vdp2rotationparameter_struct p = {0};
    p.dx = p.kx = p.ky = 1; p.charaddr = 0x60000;
    int slot = YglVitaRotationBegin(&r, &p, source);
    if (slot < 0) abort();
    for (unsigned y = 0; y < 128; ++y) {
      p.Ysp = y; YglVitaRotationRow(slot, y, &p);
    }
  }
  YglVitaRotationFlush(tm);
  glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
  glReadPixels(0, 0, 512, 128, GL_RGBA, GL_UNSIGNED_BYTE, output);
  unsigned mismatches = 0;
  for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 512; ++x) {
    unsigned word = ((x & 255) + y * 3 + (x >> 8) * 17011) & 0x7fff;
    u32 expected = (173u << 24) | ((word & 31) << 3) |
      (((word >> 5) & 31) << 11) | (((word >> 10) & 31) << 19);
    mismatches += output[y * 512 + x] != expected;
  }
  GLenum error = glGetError();
  YuiMsg("rotation_replacement_test jobs=2 pixels=65536 mismatches=%u gl_error=%04x",
         mismatches, error);
  YglVitaRotationReset();
  return mismatches + (error != GL_NO_ERROR);
}
#ifdef VITA_ROTATION_OUTPUT_REUSE
static unsigned YglVitaValidateRotationOutputReuse(YglTextureManager *tm,
                                                  unsigned char *source,u32 *output) {
  VitaRotationJob *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));
  if(!a || !b) abort();
  a->target=b->target=1; a->width=b->width=a->height=b->height=64;
  unsigned key_failures=!YglVitaRotationOutputEqual(a,b),key_cases=1;
#ifdef VITA_ROTATION_BYTE_RESIDENT
  vita_rotation_byte_valid=1;
  a->generation_owned=1;
  a->map_generation=vita_rotation_byte_generation=7;
  a->map_epoch=vita_rotation_byte_epoch=3;
  key_failures+=!YglVitaRotationByteResident(a,1);
  key_failures+=YglVitaRotationByteResident(a,0);
  vita_rotation_byte_valid=0;
  key_failures+=YglVitaRotationByteResident(a,1);
  vita_rotation_byte_valid=1; a->generation_owned=0;
  key_failures+=YglVitaRotationByteResident(a,1);
  a->generation_owned=1; a->map_generation=8;
  key_failures+=YglVitaRotationByteResident(a,1);
  a->map_generation=7; a->map_epoch=4;
  key_failures+=YglVitaRotationByteResident(a,1);
  a->map_epoch=vita_rotation_byte_epoch=UINT32_MAX;
  key_failures+=YglVitaRotationByteResident(a,1);
  a->map_epoch=vita_rotation_byte_epoch=UINT32_MAX-1;
  key_failures+=!YglVitaRotationByteResident(a,1);
  vita_rotation_byte_valid=0;
  a->map_epoch=a->map_generation=a->generation_owned=0;
  key_cases+=8;
#endif
  /* Each parameter byte, all active rows, all plane addresses and distributed
   * VRAM bytes must invalidate independently. Never execute mutated floats. */
  const size_t offsets[]={offsetof(VitaRotationJob,step),offsetof(VitaRotationJob,rows),
    offsetof(VitaRotationJob,planes),offsetof(VitaRotationJob,vram)};
  const size_t sizes[]={offsetof(VitaRotationJob,x)-offsetof(VitaRotationJob,step),
    64*sizeof(a->rows[0]),16*sizeof(a->planes[0]),sizeof(a->vram)};
  for(unsigned span=0;span<4;++span) for(size_t i=0;i<sizes[span];i+=span==3 ? 4093 : 1) {
    unsigned char *byte=(unsigned char *)b+offsets[span]+i;
    *byte^=1; key_failures+=YglVitaRotationOutputEqual(a,b); *byte^=1; ++key_cases;
  }
  b->width=63; key_failures+=YglVitaRotationOutputEqual(a,b); b->width=64;
  b->height=63; key_failures+=YglVitaRotationOutputEqual(a,b); b->height=64;
  b->target=0; key_failures+=YglVitaRotationOutputEqual(a,b); b->target=1;
  b->x=b->y=17; b->rows[64][0]=1;
  key_failures+=!YglVitaRotationOutputEqual(a,b); key_cases+=4;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  a->generation_owned=b->generation_owned=1;
  a->map_generation=b->map_generation=7;
  key_failures+=!YglVitaRotationOutputEqual(a,b);
  b->vram[0x7ffff]=1; b->map_generation=8;
  key_failures+=YglVitaRotationOutputEqual(a,b);
  b->map_generation=7; b->map_epoch=1;
  key_failures+=YglVitaRotationOutputEqual(a,b);
  a->map_epoch=b->map_epoch=UINT32_MAX;
  key_failures+=YglVitaRotationOutputEqual(a,b);
  a->map_epoch=b->map_epoch=0; b->generation_owned=0;
  key_failures+=YglVitaRotationOutputEqual(a,b); key_cases+=5;
#endif
#ifdef VITA_ROTATION_SNAPSHOT_REUSE
  RBGDrawInfo snapshot_r={0};
  a->map_generation=snapshot_r.vram_generation=7;
  a->map_epoch=snapshot_r.vram_epoch=0;
  a->generation_owned=1;
  key_failures+=!YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  key_failures+=YglVitaRotationSnapshotMatches(a,&snapshot_r,source);
  a->generation_owned=0;
  key_failures+=YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  a->generation_owned=1; snapshot_r.vram_generation=8;
  key_failures+=YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  snapshot_r.vram_generation=7; snapshot_r.vram_epoch=1;
  key_failures+=YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  a->map_epoch=snapshot_r.vram_epoch=UINT32_MAX;
  key_failures+=YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  a->map_epoch=snapshot_r.vram_epoch=UINT32_MAX-1;
  key_failures+=!YglVitaRotationSnapshotMatches(a,&snapshot_r,Vdp2Ram);
  key_cases+=7;
#endif
  free(a); free(b);
  YuiMsg("rotation_output_key_test cases=%u mismatches=%u",key_cases,key_failures);
  RBGDrawInfo r={0};
  r.info.colornumber=4; r.info.isbitmap=1;
  r.info.cellw=r.info.cellh=64; r.info.alpha=255;
  r.rotate_mval_h=r.rotate_mval_v=1; r.hres=r.vres=64;
  vdp2rotationparameter_struct p={0};
  p.dx=p.kx=p.ky=1;
  unsigned failures=key_failures,hits=vita_rotation_output_hits;
  for(unsigned pass=0;pass<4;++pass) {
    /* First/identical, changed VRAM, changed alpha. All pixels have an
     * independent expected result, including after an actual cache hit. */
    unsigned red=pass<2 ? 31 : 83,alpha=pass==3 ? 173 : 255;
    for(unsigned i=0;i<4096;++i) {
      source[i*4]=128; source[i*4+1]=11; source[i*4+2]=19; source[i*4+3]=red;
    }
    r.info.alpha=alpha;
    YglVitaRotationReset();
    int slot=YglVitaRotationBegin(&r,&p,source);
    if(slot<0) abort();
    for(unsigned y=0;y<64;++y) { p.Ysp=y; YglVitaRotationRow(slot,y,&p); }
    if(YglVitaRotationTarget(0,0,64,64)!=1) abort();
    YglVitaRotationFlush(tm);
    glBindFramebuffer(GL_FRAMEBUFFER,vita_rotation_fbo);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,output);
    u32 expected=red|(19u<<8)|(11u<<16)|(alpha<<24);
    for(unsigned i=0;i<4096;++i) failures+=output[i]!=expected;
    if(vita_rotation_output_hits!=hits+(pass>=1)) ++failures;
    vglSwapBuffers(GL_FALSE);
  }
  YuiMsg("rotation_output_test pixels=16384 hits=%u mismatches=%u",
      vita_rotation_output_hits-hits,failures);
  return failures;
}
#endif
int YglVitaValidateRotation(void) {
  unsigned char *source = calloc(1, 0x80000);
  u32 *output = malloc(65536 * sizeof(*output));
  /* The manager embeds multi-megabyte cache tables; never put it on the stack. */
  YglTextureManager *tm = calloc(1, sizeof(*tm));
  if (!source || !output || !tm) { free(source); free(output); free(tm); return -1; }
  GLint read_fb, draw_fb, active, binding;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
  tm->width = 512; tm->height = 128;
  glGenTextures(1, &tm->textureID_in[0]);
  glBindTexture(GL_TEXTURE_2D, tm->textureID_in[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  unsigned total = 0;
  for (unsigned format = 3; format <= 4; ++format) {
    unsigned stride = format == 3 ? 2 : 4;
    for (unsigned i = 0; i < 65536; ++i) {
      unsigned a = (0x60000 + i * stride) & 0x7ffff;
      if (format == 3) { source[a] = i >> 8; source[a+1] = i; }
      else {
        source[a] = i >> 8; source[a+1] = i * 13;
        source[a+2] = i >> 8; source[a+3] = i * 29;
      }
    }
    for (unsigned transparency = 0; transparency < 2; ++transparency) {
      RBGDrawInfo r = {0};
      r.info.isbitmap = 1; r.info.colornumber = format;
      r.info.cellw = 512; r.info.cellh = 256;
      r.info.alpha = 173; r.info.transparencyenable = transparency;
      r.rotate_mval_h = r.rotate_mval_v = 1;
      r.hres = 512; r.vres = 128;
      vdp2rotationparameter_struct p = {0};
      p.dx = p.kx = p.ky = 1; p.charaddr = 0x60000;
      int slot = YglVitaRotationBegin(&r, &p, source);
      if (slot < 0) abort();
      for (unsigned y = 0; y < 128; ++y) {
        p.Ysp = y; YglVitaRotationRow(slot, y, &p);
      }
      YglVitaRotationFlush(tm);
      glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
      glReadPixels(0, 0, 512, 128, GL_RGBA, GL_UNSIGNED_BYTE, output);
      unsigned mismatches = 0;
      for (unsigned i = 0; i < 65536; ++i) {
        unsigned a = (0x60000 + i * stride) & 0x7ffff;
        u32 expected = 173u << 24;
        if (format == 3) {
          unsigned word = (source[a] << 8) | source[a+1];
          expected |= ((word & 31) << 3) | (((word >> 5) & 31) << 11) |
                      (((word >> 10) & 31) << 19);
        } else expected |= source[a+3] | (source[a+2] << 8) | (source[a+1] << 16);
        if (transparency && !(source[a] & 128)) expected = 0;
        if (output[i] != expected) {
          if (mismatches < 3) YuiMsg("rotation_sample index=%u expected=%08x actual=%08x", i, expected, output[i]);
          ++mismatches;
        }
      }
      GLenum error = glGetError();
      YuiMsg("rotation_shader_test format=%u transparency=%u pixels=65536 mismatches=%u gl_error=%04x",
             format, transparency, mismatches, error);
      total += mismatches + (error != GL_NO_ERROR);
      YglVitaRotationReset();
    }
  }
  total += YglVitaValidateRotationTiles(tm, source, output);
  total += YglVitaValidateRotationReplacement(tm, source, output);
#ifdef VITA_ROTATION_OUTPUT_REUSE
  total += YglVitaValidateRotationOutputReuse(tm,source,output);
#endif
#if defined(VITA_ROTATION_VARIANTS) || defined(VITA_ROTATION_ROWS) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE)
  total += YglVitaValidateRotationVariants(tm, source, output);
#endif
  glFinish();
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
  YglVitaRotationShutdown();
  glDeleteTextures(1, &tm->textureID_in[0]);
  glBindTexture(GL_TEXTURE_2D, binding); glActiveTexture(active);
  free(source); free(output); free(tm);
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  /* Capture runs verify the write-tracking proof against actual RAM bytes for
   * every real identity hit. Separate benchmark processes leave this disabled. */
  if(!total) vita_rotation_identity_validate=1;
#endif
#ifdef VITA_ROTATION_ROWS
  vita_rotation_capture_ready = 1;
#endif
  return total ? -1 : 0;
}
