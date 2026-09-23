/* SPDX-License-Identifier: GPL-2.0-or-later
 * CPU scanline descriptors -> GPU bitmap sampling directly into the atlas.
 * Included by yglshaderes.c after the audited binary loader. No GL calls are
 * made by producer methods. Vdp2RgbTextureSync joins before atlas submission.
 */
#include "rotation_gpu.h"
#ifdef VITA_ROTATION_PATTERN_CACHE
#include "rotation_pattern_cache.h"
#endif
#ifdef VITA_ROTATION_MAP_CACHE
#include "rotation_map_cache.h"
#endif
#ifndef YGL_ROTATION_QUEUE_ONLY
#include "../../vita/telemetry.h"
#include "rotation_bitmap_v.h"
#ifdef VITA_ROTATION_COEFF_VARYING
#include "rotation_coeff_v.h"
#endif
#include "rotation_bitmap_f.h"
#ifdef VITA_ROTATION_MAP_CACHE
#include "rotation_decode_map_f.h"
#include "rotation_cached_map_f.h"
#endif
#ifdef VITA_ROTATION_VRAM_REUSE
#include "rotation_split_f.h"
#endif
#ifdef VITA_ROTATION_UNIFORMS
#include "rotation_constk_f.h"
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
#include "rotation_constk_float_f.h"
#endif
#ifdef VITA_ROTATION_PLANE_UNIFORMS
#include "rotation_constk_planes_f.h"
#endif
#endif
#ifdef VITA_ROTATION_ROWS
#include "rotation_rows_v.h"
#include "rotation_rows_f.h"
#endif
#ifdef VITA_ROTATION_VARIANTS
#include "rotation_variants.h"
#include "rotation_variant.h"
#endif
#endif

enum { VITA_ROTATION_JOBS = 2, VITA_ROTATION_MAX_ROWS = 512 };
typedef struct {
  float position[2], pixel[2];
#ifdef VITA_ROTATION_ROWS
  float coeff_low[4], coeff_high[4];
#endif
} VitaRotationVertex;
typedef struct {
  unsigned char vram[0x80000];
  float rows[VITA_ROTATION_MAX_ROWS][4];
  float planes[512]; /* one texture row: first 16 entries are plane addresses */
  float step[4], bitmap[4], pixel[3];
  float map[4], pattern[4], palette[4], special[4];
  unsigned x, y, width, height;
#if defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
  uint32_t map_generation;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  uint32_t map_epoch, generation_owned;
#endif
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
  unsigned pattern_cached;
#endif
#ifdef VITA_ROTATION_TARGET
  unsigned target; /* 1: render into vita_rotation_targets[slot], not the atlas */
#endif
#ifdef VITA_ROTATION_VRAM_REUSE
  /* vram[] is valid only when vram_copied; otherwise the snapshot equals the
   * producer shadow of this generation, already resident in the GPU texture. */
  unsigned vram_generation, vram_copied;
#endif
} VitaRotationJob;
static VitaRotationJob *vita_rotation_jobs[VITA_ROTATION_JOBS];
#ifdef VITA_ROTATION_VRAM_REUSE
/* Producer: Vdp2RamGeneration of the last copied snapshot. The worker copies
 * only after the hand-off, and the next hand-off waits for it to finish, so an
 * unchanged generation means no VRAM write happened during that copy either:
 * the resident texture equals what a new copy would read. */
static u32 vita_rotation_source;
static unsigned vita_rotation_generation;    /* producer: snapshot generation, 0=none */
static unsigned vita_rotation_uploaded;      /* consumer: texture generation, 0=none */
static unsigned vita_rotation_reuse_jobs, vita_rotation_reuse_uploads;
#endif
static unsigned vita_rotation_count;
static int vita_rotation_written;
#ifndef YGL_ROTATION_QUEUE_ONLY
#ifdef VITA_ROTATION_OUTPUT_REUSE
#include <arm_neon.h>
/* Retain producer snapshots by swapping pointers after submission: no extra
 * half-megabyte copy per miss, and no generation/hash collision assumptions.
 * CRAM is consumed later by composition; this output retains palette indices.
 */
static VitaRotationJob *vita_rotation_previous[VITA_ROTATION_JOBS];
static unsigned vita_rotation_previous_valid[VITA_ROTATION_JOBS];
static unsigned vita_rotation_output_jobs, vita_rotation_output_hits;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
static unsigned vita_rotation_identity_hits;
static int vita_rotation_identity_validate; /* capture-mode only, never benchmark timing */
#endif
static int YglVitaRotationVramEqual(const unsigned char *a,const unsigned char *b) {
  /* Cortex-A9 NEON integer equality, not floating-point approximations.
   * Fixed 512 KiB owned snapshots; every load is inside the allocation.
   * Reduce once per 256 bytes so a changed page can reject without a full scan.
   */
  for(unsigned offset=0;offset<0x80000;offset+=256) {
    uint8x16_t d0=vdupq_n_u8(0),d1=d0;
    for(unsigned i=0;i<256;i+=32) {
      d0=vorrq_u8(d0,veorq_u8(vld1q_u8(a+offset+i),vld1q_u8(b+offset+i)));
      d1=vorrq_u8(d1,veorq_u8(vld1q_u8(a+offset+i+16),vld1q_u8(b+offset+i+16)));
    }
    uint32x4_t d=vreinterpretq_u32_u8(vorrq_u8(d0,d1));
    uint32x2_t h=vorr_u32(vget_low_u32(d),vget_high_u32(d));
    if(vget_lane_u32(h,0)|vget_lane_u32(h,1)) return 0;
  }
  return 1;
}
static int YglVitaRotationSourceEqual(const VitaRotationJob *a,const VitaRotationJob *b) {
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  /* Only snapshots from the tracked emulator RAM may use this identity.
   * Synthetic validation inputs and exhausted epochs retain byte comparison.
   * Different generations may still contain equal bytes; fallback is exact.
   */
  if(a->generation_owned && b->generation_owned && a->map_epoch != UINT32_MAX &&
     a->map_epoch==b->map_epoch && a->map_generation==b->map_generation) {
    if(vita_rotation_identity_validate && !YglVitaRotationVramEqual(a->vram,b->vram)) {
      YuiMsg("fatal: rotation input identity disagrees with snapshot bytes");
      abort();
    }
    ++vita_rotation_identity_hits;
    return 1;
  }
#endif
  return YglVitaRotationVramEqual(a->vram,b->vram);
}
static int YglVitaRotationOutputEqual(const VitaRotationJob *a,
                                     const VitaRotationJob *b) {
  if (!a || !b || !a->target || !b->target ||
      a->width != b->width || a->height != b->height) return 0;
  /* x/y are atlas allocation coordinates, irrelevant to private targets.
   * Compare all shader parameters, active coefficient rows and plane addresses
   * before the large VRAM snapshot. Bit equality preserves signed zero/NaNs.
   */
  return !memcmp(a->step,b->step,offsetof(VitaRotationJob,x)-offsetof(VitaRotationJob,step)) &&
         !memcmp(a->rows,b->rows,a->height*sizeof(a->rows[0])) &&
         (a->pattern[3] != 0 || !memcmp(a->planes,b->planes,16*sizeof(a->planes[0]))) &&
         YglVitaRotationSourceEqual(a,b);
}
#endif
typedef struct {
  GLuint program;
  GLint step, bitmap, pixel, map, pattern, palette, special;
#ifdef VITA_ROTATION_PATTERN_CACHE
  GLint pattern_size;
#endif
#ifdef VITA_ROTATION_MAP_CACHE
  GLint cache_size;
#endif
#ifdef VITA_ROTATION_UNIFORMS
  GLint k;
#ifdef VITA_ROTATION_PLANE_UNIFORMS
  GLint planes[4];
#endif
#endif
} VitaRotationProgram;
/* 28 immutable specializations plus generic fallback/control. */
#ifdef VITA_ROTATION_COEFF_PAIR
static VitaRotationProgram vita_rotation_programs[86];
#elif defined(VITA_ROTATION_PATTERN_CACHE)
static VitaRotationProgram vita_rotation_programs[58];
#elif defined(VITA_ROTATION_MAP_CACHE)
static VitaRotationProgram vita_rotation_programs[33];
#elif defined(VITA_ROTATION_PLANE_UNIFORMS) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS)
static VitaRotationProgram vita_rotation_programs[31];
#else
static VitaRotationProgram vita_rotation_programs[30];
#endif
#if defined(VITA_ROTATION_VARIANTS) || defined(VITA_ROTATION_ROWS) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE)
/* Only capture-mode differential validation changes this; never producer code. */
static int vita_rotation_validate_generic;
#endif
#ifdef VITA_ROTATION_ROWS
static int vita_rotation_capture_ready;
extern int YuiCapturePending(void);
#endif
static GLuint vita_rotation_vram, vita_rotation_fbo;
#ifdef VITA_ROTATION_BYTE_RESIDENT
static unsigned vita_rotation_byte_valid, vita_rotation_byte_generation, vita_rotation_byte_epoch;
static unsigned vita_rotation_byte_jobs, vita_rotation_byte_hits;
static unsigned char *vita_rotation_byte_check; /* capture-only last upload */
static int YglVitaRotationByteResident(const VitaRotationJob *job,int byte_vram) {
  return byte_vram && vita_rotation_byte_valid && job->generation_owned &&
      job->map_epoch!=UINT32_MAX && job->map_epoch==vita_rotation_byte_epoch &&
      job->map_generation==vita_rotation_byte_generation;
}
#endif
#ifdef VITA_ROTATION_COEFF_PAIR
#include "rotation_coeff_pair.h"
static GLuint vita_rotation_coeff_pair;
static unsigned vita_rotation_pair_jobs;
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
typedef struct {
  YglRotationPatternKey key, pending;
  GLuint texture;
  unsigned valid, pending_valid;
} VitaRotationPatterns;
static VitaRotationPatterns vita_rotation_patterns[VITA_ROTATION_JOBS];
static unsigned vita_rotation_pattern_hits, vita_rotation_pattern_builds;
static unsigned vita_rotation_pattern_attempts, vita_rotation_pattern_fallbacks;
#endif
#ifdef VITA_ROTATION_MAP_CACHE
static GLuint vita_rotation_map_texture, vita_rotation_map_fbo;
static unsigned vita_rotation_map_width, vita_rotation_map_height;
static VitaRotationMapPolicy vita_rotation_map_policy;
static unsigned vita_rotation_map_hits, vita_rotation_map_misses, vita_rotation_map_fallbacks;
static unsigned vita_rotation_map_attempts, vita_rotation_map_budget_rejections;
#endif
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
static GLuint vita_rotation_float_coeff;
#endif
#ifdef VITA_ROTATION_TARGET
/* One exact-size color target per job slot. A scene over the shared 512x512
 * atlas executes every atlas tile (valid region = whole target, libgxm
 * sceGxmBeginScene) and background-loads each pixel not first covered by an
 * opaque primitive (libgxm Overview, Background Object). This target is fully
 * covered by the opaque rotation quad (no blend, no discard), so no tile loads
 * old color and only the job's own tiles are stored. */
static GLuint vita_rotation_targets[VITA_ROTATION_JOBS];
static unsigned vita_rotation_target_w[VITA_ROTATION_JOBS], vita_rotation_target_h[VITA_ROTATION_JOBS];
#endif
#ifdef VITA_ROTATION_VRAM_REUSE
/* 512x5 RGBA8: the 8192-byte row table then the 2048-byte plane row. */
static GLuint vita_rotation_coeff;
enum { VITA_ROTATION_VRAM_ROWS = 256 };
#else
enum { VITA_ROTATION_VRAM_ROWS = 261 };
#endif
static VitaRotationVertex *vita_rotation_vertices;
#endif

#ifdef VITA_ROTATION_REUSE_PROBE
#include <psp2/kernel/processmgr.h>
/* Diagnostics only: would a VRAM texture separated from per-job rows be
 * reusable? Compares each admitted snapshot with the previous one in 4KiB
 * pages. The extra shadow copy makes timings from this build non-benchmarks. */
static void YglVitaRotationReuseProbe(VitaRotationJob *job, const unsigned char *vram) {
  enum { PAGE = 4096, PAGES = 0x80000 / PAGE, REPORT = 32 };
  static unsigned char *previous;
  static unsigned jobs, identical, changed_pages, first_sum, last_sum;
  static uint64_t compare_us, copy_us;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  unsigned changed = PAGES, first = 0, last = PAGES - 1;
  if (previous) {
    changed = 0; first = PAGES; last = 0;
    for (unsigned i = 0; i < PAGES; ++i) if (memcmp(previous + i*PAGE, vram + i*PAGE, PAGE)) {
      if (!changed) first = i;
      last = i; ++changed;
    }
  }
  uint64_t t1 = sceKernelGetProcessTimeWide();
  memcpy(job->vram, vram, sizeof(job->vram));
  uint64_t t2 = sceKernelGetProcessTimeWide();
  if (!previous && !(previous = malloc(0x80000))) abort();
  memcpy(previous, vram, 0x80000);
  ++jobs; identical += changed == 0; changed_pages += changed;
  if (changed) { first_sum += first; last_sum += last; }
  compare_us += t1 - t0; copy_us += t2 - t1;
  if (jobs % REPORT == 0) {
    unsigned differing = jobs - identical;
    YuiMsg("rotation_reuse jobs=%u identical=%u changed_pages=%u/%u avg_first=%u avg_last=%u compare_us=%u copy_us=%u",
           jobs, identical, changed_pages, jobs * PAGES,
           differing ? first_sum / differing : 0, differing ? last_sum / differing : 0,
           (unsigned)compare_us, (unsigned)copy_us);
  }
}
#endif

#ifdef VITA_ROTATION_SNAPSHOT_REUSE
static int YglVitaRotationSnapshotMatches(const VitaRotationJob *job,
    const RBGDrawInfo *rbg, const unsigned char *vram) {
  return job->generation_owned && vram == Vdp2Ram &&
      rbg->vram_epoch != UINT32_MAX && job->map_epoch == rbg->vram_epoch &&
      job->map_generation == rbg->vram_generation;
}
#endif

int YglVitaRotationBegin(RBGDrawInfo *rbg, vdp2rotationparameter_struct *p,
                         const unsigned char *vram) {
  const vdp2draw_struct *info = &rbg->info;
  if (info->colornumber < 0 || info->colornumber > 4 || info->colornumber == 2 || info->LineColorBase ||
      (info->colornumber < 3 && info->specialcolormode == 3) ||
      (!info->isbitmap && (info->patternwh < 1 || info->patternwh > 2 ||
                          info->patterndatasize < 1 || info->patterndatasize > 2)) ||
      p->screenover == OVERMODE_SELPATNAME || p->coefmode == 3 ||
      (p->coefenab && p->deltaKAx != 0.0f) ||
      rbg->rotate_mval_h != 1.0f || rbg->rotate_mval_v != 1.0f ||
      rbg->hres <= 0 || rbg->vres <= 0 || rbg->vres > VITA_ROTATION_MAX_ROWS ||
      vita_rotation_count == VITA_ROTATION_JOBS) return -1;
  unsigned slot = vita_rotation_count;
  VitaRotationJob *job = vita_rotation_jobs[slot];
  if (!job) {
    job = calloc(1, sizeof(*job));
    if (!job) return -1; // CPU fallback before skipping any output
    vita_rotation_jobs[slot] = job;
  }
#ifdef VITA_ROTATION_REUSE_PROBE
  YglVitaRotationReuseProbe(job, vram);
#elif defined(VITA_ROTATION_VRAM_REUSE)
#ifdef VITA_ROTATION_VRAM_FORCE_COPY
  if (0) {
#else
  if (vita_rotation_generation && rbg->vram_generation == vita_rotation_source) {
#endif
    job->vram_copied = 0;
  } else {
    memcpy(job->vram, vram, sizeof(job->vram));
    vita_rotation_source = rbg->vram_generation;
    if (!++vita_rotation_generation) ++vita_rotation_generation; // 0 means none
    job->vram_copied = 1;
  }
  job->vram_generation = vita_rotation_generation;
#else
#ifdef VITA_ROTATION_SNAPSHOT_REUSE
  /* Each job owns its complete previous snapshot even when job pointers swap
   * after drawing. Check its OLD identity before publishing the new identity.
   * The worker handoff/join excludes guest VRAM writes during this operation.
   * Unowned buffers and exhausted epochs always take the original copy path.
   */
  static unsigned snapshot_jobs, snapshot_hits;
  int snapshot_hit = YglVitaRotationSnapshotMatches(job,rbg,vram);
  if (snapshot_hit) {
    if (vita_rotation_identity_validate && memcmp(job->vram,vram,sizeof(job->vram))) {
      YuiMsg("fatal: rotation snapshot identity disagrees with live bytes");
      abort();
    }
    ++snapshot_hits;
  } else
#endif
  memcpy(job->vram, vram, sizeof(job->vram));
#ifdef VITA_ROTATION_SNAPSHOT_REUSE
  if (!(++snapshot_jobs & 127))
    YuiMsg("rotation_snapshot_reuse jobs=%u hits=%u byte_checked=%d",
           snapshot_jobs,snapshot_hits,vita_rotation_identity_validate);
#endif
#endif
#ifdef VITA_ROTATION_TARGET
  job->target = 0;
#endif
  job->x = rbg->c.x; job->y = rbg->c.y;
#if defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
  job->map_generation = rbg->vram_generation;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  job->map_epoch=rbg->vram_epoch;
  job->generation_owned=vram==Vdp2Ram;
#endif
#endif
  job->width = rbg->hres; job->height = rbg->vres;
  job->step[0] = p->dx; job->step[1] = p->dy;
  job->step[2] = p->Xp; job->step[3] = p->Yp;
  job->bitmap[0] = p->charaddr; job->bitmap[1] = info->cellw;
  job->bitmap[2] = info->cellh; job->bitmap[3] = p->screenover;
  job->pixel[0] = info->alpha; job->pixel[1] = info->transparencyenable;
  job->pixel[2] = info->colornumber;
  job->map[0] = p->ShiftPaneX; job->map[1] = p->ShiftPaneY;
  job->map[2] = info->planew; job->map[3] = info->patternwh;
  job->pattern[0] = info->patterndatasize; job->pattern[1] = info->auxmode;
  job->pattern[2] = info->supplementdata; job->pattern[3] = info->isbitmap != 0;
  job->palette[0] = info->coloroffset; job->palette[1] = info->paladdr;
  job->palette[2] = info->specialcolormode; job->palette[3] = info->specialcode;
  job->special[0] = info->specialprimode; job->special[1] = info->priority;
  job->special[2] = info->specialfunction; job->special[3] = info->specialcolorfunction;
  if (!info->isbitmap) {
    job->bitmap[1] = p->MaxH; job->bitmap[2] = p->MaxV;
    for (unsigned i = 0; i < 16; ++i) job->planes[i] = p->PlaneAddrv[i];
  }
  ++vita_rotation_count;
  return (int)slot;
}

void YglVitaRotationRow(int slot, int y, const vdp2rotationparameter_struct *p) {
  float *row = vita_rotation_jobs[slot]->rows[y];
  row[0] = p ? p->Xsp : 0.0f; row[1] = p ? p->Ysp : 0.0f;
  row[2] = p ? p->kx : -2000000.0f; row[3] = p ? p->ky : 0.0f;
}

#ifdef VITA_ROTATION_TARGET
int YglVitaRotationTarget(unsigned x, unsigned y, unsigned w, unsigned h) {
  for (unsigned slot = 0; slot < vita_rotation_count; ++slot) {
    VitaRotationJob *job = vita_rotation_jobs[slot];
    if (job->x == x && job->y == y && job->width == w && job->height == h) {
      job->target = 1;
      return (int)slot + 1;
    }
  }
  return 0;
}
#endif

#ifdef VITA_ROTATION_UNIFORMS
/* kx/ky are uniform only if every row carries bit-identical values (this
 * includes the all-transparent case, whose sentinel the shader still tests). */
static int YglVitaRotationConstK(const VitaRotationJob *job) {
  for (unsigned y = 1; y < job->height; ++y)
    if (memcmp(&job->rows[y][2], &job->rows[0][2], 2 * sizeof(float))) return 0;
  return 1;
}
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
/* Do not assume texture sampling preserves NaN payloads, denormals or -0.
 * Ordinary finite normal values and +0 suffice for this specialization. */
static int YglVitaRotationFloatSafe(const VitaRotationJob *job) {
  /* A separate table must not clamp an out-of-range plane lookup differently
   * from the combined VRAM texture. Prove all tiled lookups address 0..15. */
  if (job->pattern[3] == 0) {
    if ((job->map[0] != 9 && job->map[0] != 10) ||
        (job->map[1] != 9 && job->map[1] != 10)) return -1;
    /* Mode 2 rejects outside dimensions before addressing; mode 3 rejects
     * outside inclusive [0,512], hence at most plane index 5. Mode 0 masks. */
    if (job->bitmap[3] != 3 &&
        ((job->bitmap[3] != 0 && job->bitmap[3] != 2) ||
         job->bitmap[1] <= 0 || job->bitmap[1] > (4u << (unsigned)job->map[0]) ||
         job->bitmap[2] <= 0 || job->bitmap[2] > (4u << (unsigned)job->map[1]))) return -1;
  }
  for (unsigned i = 0; i < job->height * 4 + 16; ++i) {
    const float *value = i < job->height * 4 ?
        &job->rows[i / 4][i % 4] : &job->planes[i - job->height * 4];
    uint32_t bits; memcpy(&bits, value, sizeof(bits));
    uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0x7f800000u || (!exponent && bits)) return -2;
  }
  return 1;
}
#endif
#endif
#ifndef YGL_ROTATION_QUEUE_ONLY
#ifdef VITA_ROTATION_PATTERN_CACHE
static int YglVitaRotationPreparePatterns(const VitaRotationJob *job, unsigned slot) {
  if (vita_rotation_validate_generic || job->pattern[3] != 0 ||
      (job->bitmap[3] != 0 && job->bitmap[3] != 2) ||
      !YglVitaRotationConstK(job)) return 0;
  YglRotationPatternKey key={0};
  key.generation=job->map_generation;
  key.width=(unsigned)job->bitmap[1]; key.height=(unsigned)job->bitmap[2];
  key.shift_x=(unsigned)job->map[0]; key.shift_y=(unsigned)job->map[1];
  key.pages_x=(unsigned)job->map[2]; key.cells=(unsigned)job->map[3];
  key.words=(unsigned)job->pattern[0]; key.aux=(unsigned)job->pattern[1];
  key.supplement=(unsigned)job->pattern[2]; key.format=(unsigned)job->pixel[2];
  for(unsigned i=0;i<16;++i) key.planes[i]=(unsigned)job->planes[i];
  size_t bytes=YglRotationPatternBytes(&key);
  if (!bytes || bytes > 0x100000) return 0;
  VitaRotationPatterns *cache=&vita_rotation_patterns[slot];
  int hit=key.generation && cache->valid && !memcmp(&key,&cache->key,sizeof(key));
  // A changing map must retain the fast existing shader, not repeatedly decode
  // a whole name table. Require two observations before paying the cold cost.
  if (!hit && key.generation &&
      (!cache->pending_valid || memcmp(&key,&cache->pending,sizeof(key)))) {
    cache->pending=key; cache->pending_valid=1;
    return 0;
  }
  glActiveTexture(GL_TEXTURE1);
  if (hit) {
    glBindTexture(GL_TEXTURE_2D,cache->texture);
    ++vita_rotation_pattern_hits;
    cache->pending_valid=0;
  } else {
    uint8_t *data=malloc(bytes);
    if (!data) { glActiveTexture(GL_TEXTURE0); return 0; }
    if (!YglRotationPatternBuild(&key,job->vram,data,bytes)) abort();
    if (!cache->texture) glGenTextures(1,&cache->texture);
    glBindTexture(GL_TEXTURE_2D,cache->texture);
    cache->valid=0;
    // Checked replacement/deferred retirement: never overwrite GPU-owned data.
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,key.width/(key.cells*8),
                 key.height/(key.cells*8),0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    free(data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) { glActiveTexture(GL_TEXTURE0); return 0; }
    cache->key=key; cache->valid=1;
    cache->pending_valid=0;
    ++vita_rotation_pattern_builds;
  }
  glActiveTexture(GL_TEXTURE0);
  return 1;
}
#endif
static VitaRotationProgram *YglVitaRotationProgram(const VitaRotationJob *job, int map_mode) {
  int index = 28;
#ifdef VITA_ROTATION_VRAM_REUSE
  const unsigned char *binary = ygl_vita_rotation_split_f;
  unsigned binary_size = sizeof(ygl_vita_rotation_split_f);
#elif defined(VITA_ROTATION_UNIFORMS)
  const unsigned char *binary = ygl_vita_rotation_bitmap_f;
  unsigned binary_size = sizeof(ygl_vita_rotation_bitmap_f);
  if (YglVitaRotationConstK(job)) {
    index = 29;
    binary = ygl_vita_rotation_constk_f; binary_size = sizeof(ygl_vita_rotation_constk_f);
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
    static unsigned jobs, admitted, map_rejected, value_rejected;
    int safe = YglVitaRotationFloatSafe(job);
    if (!vita_rotation_validate_generic && safe > 0) {
      index = 30;
      binary = ygl_vita_rotation_constk_float_f;
      binary_size = sizeof(ygl_vita_rotation_constk_float_f);
    }
    if (!vita_rotation_validate_generic) {
      ++jobs; admitted += safe > 0;
      map_rejected += safe == -1; value_rejected += safe == -2;
      if (!(jobs & 127))
        YuiMsg("rotation_float_jobs total=%u admitted=%u map_rejected=%u value_rejected=%u",
               jobs, admitted, map_rejected, value_rejected);
    }
#endif
#ifdef VITA_ROTATION_PLANE_UNIFORMS
    /* Repeat mode masks coordinates into positive map dimensions. For the
     * Saturn's 512/1024-dot planes, these bounds prove plane index 0..15.
     * All other jobs retain the original table read, not a shader-side guess. */
    if (job->pattern[3] == 0 && job->bitmap[3] == 0 &&
        (job->map[0] == 9 || job->map[0] == 10) &&
        (job->map[1] == 9 || job->map[1] == 10) &&
        job->bitmap[1] > 0 && job->bitmap[1] <= (4u << (unsigned)job->map[0]) &&
        job->bitmap[2] > 0 && job->bitmap[2] <= (4u << (unsigned)job->map[1])) {
      index = 30;
      binary = ygl_vita_rotation_constk_planes_f;
      binary_size = sizeof(ygl_vita_rotation_constk_planes_f);
    }
#endif
  }
#else
  const unsigned char *binary = ygl_vita_rotation_bitmap_f;
  unsigned binary_size = sizeof(ygl_vita_rotation_bitmap_f);
#endif
  const unsigned char *vertex = ygl_vita_rotation_bitmap_v;
  unsigned vertex_size = sizeof(ygl_vita_rotation_bitmap_v);
#ifdef VITA_ROTATION_ROWS
  if (!vita_rotation_validate_generic) {
    index = 29;
    binary = ygl_vita_rotation_rows_f; binary_size = sizeof(ygl_vita_rotation_rows_f);
    vertex = ygl_vita_rotation_rows_v; vertex_size = sizeof(ygl_vita_rotation_rows_v);
  }
#endif
#ifdef VITA_ROTATION_VARIANTS
  int variant = YglRotationVariant((int)job->pixel[2], (int)job->pattern[3],
      (int)job->map[3], (int)job->pattern[0], (int)job->pattern[1]);
  if (variant >= 0 && !vita_rotation_validate_generic
#ifdef VITA_ROTATION_UNIFORMS
      && YglVitaRotationConstK(job)
#endif
      ) {
    index = variant;
    binary = vita_rotation_binaries[index].data;
    binary_size = vita_rotation_binaries[index].size;
#ifdef VITA_ROTATION_PATTERN_CACHE
    if (job->pattern_cached) {
      binary=vita_rotation_pattern_binaries[variant].data;
      binary_size=vita_rotation_pattern_binaries[variant].size;
      index=30+variant;
#ifdef VITA_ROTATION_COEFF_PAIR
      if (YglRotationPairSafe(job->rows,job->height)) {
        binary=vita_rotation_pair_binaries[variant].data;
        binary_size=vita_rotation_pair_binaries[variant].size;
        index=58+variant;
#ifdef VITA_ROTATION_COEFF_VARYING
        vertex=ygl_vita_rotation_coeff_v;
        vertex_size=sizeof(ygl_vita_rotation_coeff_v);
#endif
      }
#endif
    }
#endif
  }
#else
  (void)job;
#endif
  /* Cache mode is selected only after allocation/ownership admission. */
#ifdef VITA_ROTATION_MAP_CACHE
  if (map_mode == 1) {
    index = 32; binary = ygl_vita_rotation_decode_map_f;
    binary_size = sizeof(ygl_vita_rotation_decode_map_f);
  } else if (map_mode == 2) {
    index = 31; binary = ygl_vita_rotation_cached_map_f;
    binary_size = sizeof(ygl_vita_rotation_cached_map_f);
  }
#else
  (void)map_mode;
#endif
  VitaRotationProgram *p = &vita_rotation_programs[index];
  if (p->program) return p;
  GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
  YglVitaLoadGxp(vs, vertex, vertex_size, NULL);
  YglVitaLoadGxp(fs, binary, binary_size, NULL);
  p->program = glCreateProgram();
  glAttachShader(p->program, vs); glAttachShader(p->program, fs);
  glBindAttribLocation(p->program, 0, "a_position");
  glBindAttribLocation(p->program, 1, "a_pixel");
#ifdef VITA_ROTATION_ROWS
  if (index == 29) {
    glBindAttribLocation(p->program, 2, "a_coeff_low");
    glBindAttribLocation(p->program, 3, "a_coeff_high");
  }
#endif
  glLinkProgram(p->program);
  GLint linked = 0; glGetProgramiv(p->program, GL_LINK_STATUS, &linked);
  if (!linked) { YuiMsg("fatal: rotation bitmap shader link failed"); abort(); }
  glDeleteShader(vs); glDeleteShader(fs);
  p->step = glGetUniformLocation(p->program, "u_step");
  p->bitmap = glGetUniformLocation(p->program, "u_bitmap");
  p->pixel = glGetUniformLocation(p->program, "u_pixel");
  p->map = glGetUniformLocation(p->program, "u_map");
  p->pattern = glGetUniformLocation(p->program, "u_pattern");
  p->palette = glGetUniformLocation(p->program, "u_palette");
  p->special = glGetUniformLocation(p->program, "u_special");
#ifdef VITA_ROTATION_UNIFORMS
  p->k = glGetUniformLocation(p->program, "u_k");
#ifdef VITA_ROTATION_PLANE_UNIFORMS
  const char *plane_names[] = {"u_planes0", "u_planes1", "u_planes2", "u_planes3"};
  for (unsigned i = 0; i < 4; ++i)
    p->planes[i] = index == 30 ? glGetUniformLocation(p->program, plane_names[i]) : -1;
#endif
#endif
  glUseProgram(p->program);
  glUniform1i(glGetUniformLocation(p->program, "s_vram"), 0);
#ifdef VITA_ROTATION_BYTE_VRAM
  glUniform1i(glGetUniformLocation(p->program,"s_byte_vram"),0);
#endif
#ifdef VITA_ROTATION_COEFF_PAIR
  if (index>=58) glUniform1i(glGetUniformLocation(p->program,"s_coeff_pair"),2);
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
  p->pattern_size=glGetUniformLocation(p->program,"u_pattern_size");
  glUniform1i(glGetUniformLocation(p->program,"s_patterns"),1);
#endif
#ifdef VITA_ROTATION_MAP_CACHE
  p->cache_size = index == 31 ? glGetUniformLocation(p->program, "u_cache_size") : -1;
  if (index == 31) glUniform1i(glGetUniformLocation(p->program, "s_decoded_map"), 1);
#endif
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
  if (index == 30) glUniform1i(glGetUniformLocation(p->program, "s_coeff_float"), 1);
#endif
#ifdef VITA_ROTATION_VRAM_REUSE
  glUniform1i(glGetUniformLocation(p->program, "s_coeff"), 1);
#endif
  YuiMsg("rotation_program variant=%d", index);
  return p;
}

static void YglVitaRotationInitialize(void) {
  if (vita_rotation_vram) return;
  glGenTextures(1, &vita_rotation_vram);
  glBindTexture(GL_TEXTURE_2D, vita_rotation_vram);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, VITA_ROTATION_VRAM_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#ifdef VITA_ROTATION_VRAM_REUSE
  glGenTextures(1, &vita_rotation_coeff);
  glBindTexture(GL_TEXTURE_2D, vita_rotation_coeff);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 5, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  vita_rotation_uploaded = 0; // new storage holds no snapshot
#endif
  glGenFramebuffers(1, &vita_rotation_fbo);
#ifdef VITA_ROTATION_ROWS
  vita_rotation_vertices = malloc(6 * VITA_ROTATION_MAX_ROWS * sizeof(*vita_rotation_vertices));
#else
  vita_rotation_vertices = malloc(6 * sizeof(*vita_rotation_vertices));
#endif
  if (!vita_rotation_vertices) abort();
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
  glGenTextures(1, &vita_rotation_float_coeff);
  glBindTexture(GL_TEXTURE_2D, vita_rotation_float_coeff);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif
}

#ifdef VITA_ROTATION_MAP_CACHE
static void YglVitaRotationMapUniforms(VitaRotationProgram *p, const VitaRotationJob *job) {
  glUseProgram(p->program);
  glUniform4fv(p->step, 1, job->step);
  glUniform4fv(p->bitmap, 1, job->bitmap);
  glUniform3fv(p->pixel, 1, job->pixel);
  glUniform4fv(p->map, 1, job->map);
  glUniform4fv(p->pattern, 1, job->pattern);
  glUniform4fv(p->palette, 1, job->palette);
  glUniform4fv(p->special, 1, job->special);
  if (p->k != -1) glUniform2f(p->k, job->rows[0][2], job->rows[0][3]);
}
/* Called after the immutable source upload and before the final rotation draw.
 * Render-to-texture and later fragment sampling stay in the same GXM context,
 * whose scenes are ordered. Never touch mapped storage or bypass retirement. */
static void YglVitaRotationTryMap(const VitaRotationJob *job, unsigned target_w, unsigned target_h) {
  if (vita_rotation_validate_generic) return;
  if (!YglVitaRotationConstK(job)) { ++vita_rotation_map_fallbacks; return; }
  VitaRotationMapKey key = {0};
  key.generation = job->map_generation;
#define MAP_KEY_COPY(field) memcpy(key.field, job->field, sizeof(key.field))
  MAP_KEY_COPY(bitmap); MAP_KEY_COPY(pixel); MAP_KEY_COPY(map);
  MAP_KEY_COPY(pattern); MAP_KEY_COPY(palette); MAP_KEY_COPY(special); MAP_KEY_COPY(planes);
#undef MAP_KEY_COPY
  int action = YglRotationMapObserve(&vita_rotation_map_policy, &key);
  if (!action) { ++vita_rotation_map_fallbacks; return; }
  unsigned w = YglRotationMapExtent(&key,0), h = YglRotationMapExtent(&key,1);
  if (vita_rotation_map_texture && (w != vita_rotation_map_width || h != vita_rotation_map_height)) {
    /* Retire old-sized resources normally. Try allocation on a later frame;
     * do not force a whole-device drain just to replace a cache. */
    glDeleteFramebuffers(1, &vita_rotation_map_fbo);
    glDeleteTextures(1, &vita_rotation_map_texture);
    vita_rotation_map_texture = vita_rotation_map_fbo = 0;
    vita_rotation_map_policy.valid = 0;
    ++vita_rotation_map_fallbacks; return;
  }
  if (!vita_rotation_map_texture) {
    size_t bytes = (size_t)w * h * 4;
    size_t ram = vglMemFree(VGL_MEM_RAM), vram = vglMemFree(VGL_MEM_VRAM);
    GLint limit; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    /* vitaGL allocates matching DF32 depth even with depth testing disabled.
     * Leave headroom for render-target/patcher/ordinary frame allocations. */
    if (w > (unsigned)limit || h > (unsigned)limit ||
        ram + vram < 2 * bytes + 8 * 1024 * 1024 ||
        (ram < bytes + 2 * 1024 * 1024 && vram < bytes + 2 * 1024 * 1024)) {
      ++vita_rotation_map_budget_rejections;
      ++vita_rotation_map_fallbacks; return;
    }
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &vita_rotation_map_texture);
    glBindTexture(GL_TEXTURE_2D, vita_rotation_map_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &vita_rotation_map_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_map_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          vita_rotation_map_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) abort();
    glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
    glActiveTexture(GL_TEXTURE0);
    vita_rotation_map_width = w; vita_rotation_map_height = h;
    vita_rotation_map_policy.valid = 0;
    action = 2;
    YuiMsg("rotation_map_allocate width=%u height=%u logical_width=%u logical_height=%u color_bytes=%u depth_bytes=%u",
           w,h,(unsigned)key.bitmap[1],(unsigned)key.bitmap[2],(unsigned)bytes,(unsigned)bytes);
  }
  if (action == 2) {
    VitaRotationProgram *decode = YglVitaRotationProgram(job, 1);
    YglVitaRotationMapUniforms(decode,job);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER,vita_rotation_map_fbo);
    glViewport(0,0,w,h);
    static const unsigned cx[6]={0,1,0,0,1,1}, cy[6]={0,0,1,1,0,1};
    VitaRotationVertex vertices[6];
    for (unsigned i=0;i<6;++i) {
      vertices[i].position[0] = 2.0f*cx[i]-1.0f;
      vertices[i].position[1] = 2.0f*cy[i]-1.0f;
      vertices[i].pixel[0] = (float)(cx[i]*w);
      vertices[i].pixel[1] = (float)(cy[i]*h);
    }
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(VitaRotationVertex),vertices[0].position);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(VitaRotationVertex),vertices[0].pixel);
    glDrawArrays(GL_TRIANGLES,0,6);
    glBindFramebuffer(GL_FRAMEBUFFER,vita_rotation_fbo);
    glViewport(0,0,target_w,target_h);
    YglRotationMapCommit(&vita_rotation_map_policy,&key);
    ++vita_rotation_map_misses;
  } else ++vita_rotation_map_hits;
  VitaRotationProgram *sample = YglVitaRotationProgram(job,2);
  YglVitaRotationMapUniforms(sample,job);
  glUniform2f(sample->cache_size,(float)w,(float)h);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,vita_rotation_map_texture);
  glActiveTexture(GL_TEXTURE0);
}
#endif

void YglVitaRotationFlush(YglTextureManager *tm) {
  VT_SCOPE(VT_GPU_SUBMIT);
  if (!vita_rotation_count) return;
  GLint viewport[4], read_fb, draw_fb, program, active, texture, buffer;
  GLboolean mask[4], enabled[5];
  const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE};
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
#ifdef VITA_ROTATION_COEFF_PAIR
  GLint texture2;
  glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture2);
#endif
#if defined(VITA_ROTATION_VRAM_REUSE) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
  GLint texture1;
  glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture1);
#endif
  glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  for (unsigned i=0; i<5; ++i) { enabled[i] = glIsEnabled(caps[i]); glDisable(caps[i]); }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  YglVitaRotationInitialize();
  glBindFramebuffer(GL_FRAMEBUFFER, vita_rotation_fbo);
#ifdef VITA_ROTATION_TARGET
  int atlas_written = 0;
#else
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tm->textureID_in[tm->current], 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) abort();
  glViewport(0, 0, tm->width, tm->height);
#endif
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glDisableVertexAttribArray(2);
  for (unsigned slot=0; slot<vita_rotation_count; ++slot) {
    VitaRotationJob *job = vita_rotation_jobs[slot];
#ifdef VITA_ROTATION_OUTPUT_REUSE
    ++vita_rotation_output_jobs;
    int output_hit = vita_rotation_previous_valid[slot] &&
        YglVitaRotationOutputEqual(job,vita_rotation_previous[slot]);
    /* Differential shader tests must actually execute their selected program. */
    if (vita_rotation_validate_generic) output_hit=0;
    if (output_hit) {
      ++vita_rotation_output_hits;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
      /* A byte-equal new generation is now the same retained source too. */
      vita_rotation_previous[slot]->map_generation=job->map_generation;
      vita_rotation_previous[slot]->map_epoch=job->map_epoch;
      vita_rotation_previous[slot]->generation_owned=job->generation_owned;
#endif
    }
    if (!(vita_rotation_output_jobs&127))
      YuiMsg("rotation_output_reuse jobs=%u hits=%u",vita_rotation_output_jobs,vita_rotation_output_hits);
#ifdef VITA_ROTATION_OUTPUT_GENERATION
    if (!(vita_rotation_output_jobs&127))
      YuiMsg("rotation_output_identity hits=%u byte_checked=%d",
          vita_rotation_identity_hits,vita_rotation_identity_validate);
#endif
    if (output_hit) continue;
    vita_rotation_previous_valid[slot]=0;
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
    { VT_SCOPE(VT_ROTATION_UPLOAD);
      job->pattern_cached=YglVitaRotationPreparePatterns(job,slot);
    }
    ++vita_rotation_pattern_attempts;
    vita_rotation_pattern_fallbacks += !job->pattern_cached;
    if (!(vita_rotation_pattern_attempts&127))
      YuiMsg("rotation_pattern_cache jobs=%u hits=%u builds=%u fallbacks=%u",
          vita_rotation_pattern_attempts,vita_rotation_pattern_hits,
          vita_rotation_pattern_builds,vita_rotation_pattern_fallbacks);
#endif
    VitaRotationProgram *p = YglVitaRotationProgram(job, 0);
    glUseProgram(p->program);
#ifdef VITA_ROTATION_PATTERN_CACHE
    if (p->pattern_size != -1)
      glUniform2f(p->pattern_size,job->bitmap[1]/(job->map[3]*8),
                  job->bitmap[2]/(job->map[3]*8));
#endif
    if (job->x + job->width > tm->width || job->y + job->height > tm->height) abort();
    /* Origin and size of the color target, in pixels. */
    unsigned origin_x = job->x, origin_y = job->y, target_w = tm->width, target_h = tm->height;
#ifdef VITA_ROTATION_TARGET
    if (job->target) {
      GLuint *t = &vita_rotation_targets[slot];
      if (!*t || vita_rotation_target_w[slot] != job->width || vita_rotation_target_h[slot] != job->height) {
        /* Size changes only: vitaGL clears NULL-data storage (gpu_utils.c). */
        if (!*t) glGenTextures(1, t);
        glBindTexture(GL_TEXTURE_2D, *t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, job->width, job->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        vita_rotation_target_w[slot] = job->width; vita_rotation_target_h[slot] = job->height;
      }
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *t, 0);
      origin_x = origin_y = 0; target_w = job->width; target_h = job->height;
    } else {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             tm->textureID_in[tm->current], 0);
      atlas_written = 1;
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) abort();
    glViewport(0, 0, target_w, target_h);
#endif
    unsigned strips = 1;
#ifdef VITA_ROTATION_ROWS
    if (!vita_rotation_validate_generic) strips = job->height;
#endif
    static const unsigned corner_x[6] = {0,1,0,0,1,1}, corner_y[6] = {0,0,1,1,0,1};
    for (unsigned row=0; row<strips; ++row) {
#ifdef VITA_ROTATION_ROWS
      float low[4], high[4];
      for (unsigned k=0; k<4; ++k) {
        uint32_t bits;
        memcpy(&bits, &job->rows[row][k], sizeof(bits));
        low[k] = (float)(bits & 65535);
        high[k] = (float)(bits >> 16);
      }
#endif
    for (unsigned c=0; c<6; ++c) {
      VitaRotationVertex *v = &vita_rotation_vertices[row*6+c];
      unsigned y = strips == 1 ? corner_y[c]*job->height : row + corner_y[c];
      v->position[0] = 2.0f*(origin_x + corner_x[c]*job->width)/target_w - 1.0f;
      v->position[1] = 2.0f*(origin_y + y)/target_h - 1.0f;
      v->pixel[0] = (float)(corner_x[c]*job->width);
      v->pixel[1] = (float)y;
#ifdef VITA_ROTATION_ROWS
      memcpy(v->coeff_low, low, sizeof(low));
      memcpy(v->coeff_high, high, sizeof(high));
#endif
    }
    }
    glBindTexture(GL_TEXTURE_2D, vita_rotation_vram);
    _Static_assert(offsetof(VitaRotationJob, rows) == 0x80000, "VRAM/float byte upload layout");
    _Static_assert(sizeof(float) == 4, "Shader requires 32-bit IEEE floats");
    _Static_assert(offsetof(VitaRotationJob, step) == 512*261*4, "VRAM/row/plane upload layout");
#ifdef VITA_ROTATION_VRAM_REUSE
    { VT_SCOPE(VT_ROTATION_UPLOAD);
      /* Same full-replacement rule as below, per texture. VRAM storage is
       * replaced only for a new producer generation; an unchanged snapshot is
       * byte-identical to the resident texture, so re-sending it is removed. */
      ++vita_rotation_reuse_jobs;
      if (job->vram_copied) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, job->vram);
        vita_rotation_uploaded = job->vram_generation;
        ++vita_rotation_reuse_uploads;
      } else if (job->vram_generation != vita_rotation_uploaded) {
        YuiMsg("fatal: rotation VRAM generation %u not resident (%u)",
               job->vram_generation, vita_rotation_uploaded);
        abort();
      }
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, vita_rotation_coeff);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 5, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, job->rows);
      glActiveTexture(GL_TEXTURE0);
      if ((vita_rotation_reuse_jobs & 255) == 0)
        YuiMsg("rotation_vram_reuse jobs=%u vram_uploads=%u",
               vita_rotation_reuse_jobs, vita_rotation_reuse_uploads);
    }
#else
    { VT_SCOPE(VT_ROTATION_UPLOAD);
      /* Every texel is replaced. Checked vitaGL SubImage would first copy
       * the entire in-flight backing only to overwrite it here. Image uses
       * the same deferred retirement but uploads directly into new storage. */
      unsigned upload_rows=261;
#ifdef VITA_ROTATION_BYTE_VRAM
      int byte_vram=p>=&vita_rotation_programs[58] && p<&vita_rotation_programs[72];
      if (byte_vram) upload_rows=256;
#endif
#ifdef VITA_ROTATION_BYTE_RESIDENT
      ++vita_rotation_byte_jobs;
      int resident=YglVitaRotationByteResident(job,byte_vram);
      if (resident) {
        ++vita_rotation_byte_hits;
        if (vita_rotation_identity_validate &&
            (!vita_rotation_byte_check || memcmp(vita_rotation_byte_check,job->vram,sizeof(job->vram)))) {
          YuiMsg("fatal: resident rotation VRAM differs from snapshot"); abort();
        }
      } else {
        vita_rotation_byte_valid=0;
#endif
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, upload_rows, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, job);
#ifdef VITA_ROTATION_BYTE_VRAM
      if (byte_vram) {
        // Same 512 KiB snapshot, no alias or extra copy. Only the freshly
        // allocated descriptor changes: 512*4 == 2048*1 bytes per row.
        SceGxmTexture *descriptor=vglGetGxmTexture(GL_TEXTURE_2D);
        int rc=sceGxmTextureInitLinear(descriptor,sceGxmTextureGetData(descriptor),
            SCE_GXM_TEXTURE_FORMAT_U8_R,2048,256,1);
        if (rc<0) { YuiMsg("fatal: byte VRAM texture init=%08x",rc); abort(); }
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      }
#endif
#ifdef VITA_ROTATION_BYTE_RESIDENT
        if (byte_vram && job->generation_owned && job->map_epoch!=UINT32_MAX) {
          if (vita_rotation_identity_validate) {
            if (!vita_rotation_byte_check) vita_rotation_byte_check=malloc(sizeof(job->vram));
            if (!vita_rotation_byte_check) abort();
            memcpy(vita_rotation_byte_check,job->vram,sizeof(job->vram));
          }
          vita_rotation_byte_generation=job->map_generation;
          vita_rotation_byte_epoch=job->map_epoch;
          vita_rotation_byte_valid=1;
        }
      }
      if (!(vita_rotation_byte_jobs&127))
        YuiMsg("rotation_byte_resident jobs=%u hits=%u byte_checked=%d",
            vita_rotation_byte_jobs,vita_rotation_byte_hits,vita_rotation_identity_validate);
#endif
    }
#endif
    glUniform4fv(p->step, 1, job->step);
#ifdef VITA_ROTATION_COEFF_PAIR
    if (p>=&vita_rotation_programs[58]) {
      VT_SCOPE(VT_ROTATION_UPLOAD);
      if (!(++vita_rotation_pair_jobs&127))
        YuiMsg("rotation_coeff_pair jobs=%u",vita_rotation_pair_jobs);
      glActiveTexture(GL_TEXTURE2);
      if (!vita_rotation_coeff_pair) glGenTextures(1,&vita_rotation_coeff_pair);
      glBindTexture(GL_TEXTURE_2D,vita_rotation_coeff_pair);
      // Same 8192 bytes / 2048-byte row pitch: allocate and retire normally,
      // then reinterpret ONLY the newly allocated private texture descriptor.
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,4,0,GL_RGBA,GL_UNSIGNED_BYTE,job->rows);
      SceGxmTexture *descriptor=vglGetGxmTexture(GL_TEXTURE_2D);
      // SetFormat requires unchanged bits-per-pixel (libgxm Reference).
      // Reinitialize the fresh descriptor instead: default 256*8 == 512*4
      // bytes/row; linear experiment views the same 8192 bytes as 1024*8.
      int rc=sceGxmTextureInitLinear(descriptor,sceGxmTextureGetData(descriptor),
#ifdef VITA_ROTATION_COEFF_LINEAR
          SCE_GXM_TEXTURE_FORMAT_F32F32_GR,1024,1,1);
#else
          SCE_GXM_TEXTURE_FORMAT_F32F32_GR,256,4,1);
#endif
      if (rc<0) { YuiMsg("fatal: paired coefficient texture init=%08x",rc); abort(); }
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      glActiveTexture(GL_TEXTURE0);
    }
#endif
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
    if (p == &vita_rotation_programs[30]) {
      VT_SCOPE(VT_ROTATION_UPLOAD);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, vita_rotation_float_coeff);
      /* Same 4-byte texel storage, copied and retired by vitaGL normally.
       * Only this private descriptor changes interpretation after replacement;
       * no alias to the VRAM texture and no in-flight backing mutation. */
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 5, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, job->rows);
      if (sceGxmTextureSetFormat(vglGetGxmTexture(GL_TEXTURE_2D),
                                SCE_GXM_TEXTURE_FORMAT_F32_R) < 0) abort();
      glActiveTexture(GL_TEXTURE0);
    }
#endif
    glUniform4fv(p->bitmap, 1, job->bitmap);
    glUniform3fv(p->pixel, 1, job->pixel);
    glUniform4fv(p->map, 1, job->map);
    glUniform4fv(p->pattern, 1, job->pattern);
    glUniform4fv(p->palette, 1, job->palette);
    glUniform4fv(p->special, 1, job->special);
#ifdef VITA_ROTATION_UNIFORMS
    if (p->k != -1) glUniform2f(p->k, job->rows[0][2], job->rows[0][3]);
#ifdef VITA_ROTATION_PLANE_UNIFORMS
    for (unsigned i = 0; i < 4; ++i)
      if (p->planes[i] != -1) glUniform4fv(p->planes[i], 1, job->planes + i * 4);
#endif
#endif
#ifdef VITA_ROTATION_MAP_CACHE
    YglVitaRotationTryMap(job,target_w,target_h);
    if (!(++vita_rotation_map_attempts & 127))
      YuiMsg("rotation_map_cache jobs=%u hits=%u rebuilds=%u fallbacks=%u budget_rejected=%u",
             vita_rotation_map_attempts,vita_rotation_map_hits,vita_rotation_map_misses,
             vita_rotation_map_fallbacks,vita_rotation_map_budget_rejections);
#endif
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VitaRotationVertex), vita_rotation_vertices[0].position);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VitaRotationVertex), vita_rotation_vertices[0].pixel);
#ifdef VITA_ROTATION_ROWS
    if (!vita_rotation_validate_generic) {
      glEnableVertexAttribArray(2);
      glEnableVertexAttribArray(3);
      glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(VitaRotationVertex), vita_rotation_vertices[0].coeff_low);
      glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(VitaRotationVertex), vita_rotation_vertices[0].coeff_high);
    } else { glDisableVertexAttribArray(2); glDisableVertexAttribArray(3); }
#endif
    glDrawArrays(GL_TRIANGLES, 0, 6*strips);
#ifdef VITA_ROTATION_OUTPUT_REUSE
    if (job->target && !vita_rotation_validate_generic) {
      vita_rotation_jobs[slot]=vita_rotation_previous[slot];
      vita_rotation_previous[slot]=job;
      vita_rotation_previous_valid[slot]=1;
    }
#endif
  }
#ifdef VITA_ROTATION_ROWS
  glDisableVertexAttribArray(2);
  glDisableVertexAttribArray(3);
  if (vita_rotation_capture_ready && !vita_rotation_validate_generic && YuiCapturePending()) {
    size_t bytes = (size_t)tm->width * tm->height * sizeof(u32);
    u32 *row_output = malloc(bytes), *generic_output = malloc(bytes);
    if (!row_output || !generic_output) abort();
    glReadPixels(0, 0, tm->width, tm->height, GL_RGBA, GL_UNSIGNED_BYTE, row_output);
    /* Reuse precisely the same queued snapshots, geometry and uniforms. The
     * generic draw also leaves authoritative output in the atlas for capture. */
    vita_rotation_validate_generic = 1;
    YglVitaRotationFlush(tm);
    vita_rotation_validate_generic = 0;
    glReadPixels(0, 0, tm->width, tm->height, GL_RGBA, GL_UNSIGNED_BYTE, generic_output);
    unsigned mismatches = 0;
    for (size_t i=0; i<bytes/sizeof(u32); ++i) if (row_output[i] != generic_output[i]) {
      if (mismatches < 4) YuiMsg("rotation_live_sample x=%u y=%u rows=%08x generic=%08x",
          (unsigned)(i%tm->width), (unsigned)(i/tm->width), row_output[i], generic_output[i]);
      ++mismatches;
    }
    YuiMsg("rotation_live_test pixels=%u mismatches=%u", (unsigned)(bytes/sizeof(u32)), mismatches);
    free(row_output); free(generic_output);
  }
#endif
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) { YuiMsg("fatal: rotation bitmap GL error=%x",error); abort(); }
#ifdef VITA_ROTATION_TARGET
  /* Atlas texels are GPU-only only when some job still rendered into it. */
  if (atlas_written) vita_rotation_written = 1;
  vita_rotation_count = 0;
#else
  vita_rotation_written = 1; vita_rotation_count = 0;
#endif
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
#ifdef VITA_ROTATION_COEFF_PAIR
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,texture2);
#endif
#if defined(VITA_ROTATION_VRAM_REUSE) || defined(VITA_ROTATION_FLOAT_COEFFICIENTS) || defined(VITA_ROTATION_MAP_CACHE) || defined(VITA_ROTATION_PATTERN_CACHE)
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texture1);
  glActiveTexture(GL_TEXTURE0);
#endif
  glBindTexture(GL_TEXTURE_2D, texture); glActiveTexture(active); glUseProgram(program);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glColorMask(mask[0], mask[1], mask[2], mask[3]);
  for (unsigned i=0; i<5; ++i) if (enabled[i]) glEnable(caps[i]);
  /* The consumer installs its own attribute pointers before every draw. */
}
void YglVitaRotationReset(void) {
  if (vita_rotation_count) abort();
  vita_rotation_written = 0;
}
int YglVitaRotationWritten(void) { return vita_rotation_written; }
int YglVitaRotationUsesAtlas(void) {
#ifdef VITA_ROTATION_TARGET
  for (unsigned i=0;i<vita_rotation_count;++i)
    if (!vita_rotation_jobs[i]->target) return 1;
  return 0;
#else
  return vita_rotation_count != 0;
#endif
}
int YglVitaRotationPrivateRect(unsigned x,unsigned y,unsigned w,unsigned h) {
#ifdef VITA_ROTATION_TARGET
  _Static_assert(VITA_ROTATION_JOBS <= 2,"Atlas extent keeps three highest rectangles");
  for(unsigned i=0;i<vita_rotation_count;++i) {
    const VitaRotationJob *job=vita_rotation_jobs[i];
    if(job->target && job->x==x && job->y==y && job->width==w && job->height==h)
      return 1;
  }
#endif
  return 0;
}
#ifdef VITA_ROTATION_TARGET
/* Composition: sample job `id` (from YglVitaRotationTarget) on unit 0. The
 * program addresses int(texcoord) texels normalised by u_texsize, so the
 * target's own size replaces the atlas size (native_vita.inc). */
static void YglVitaRotationSource(int id, int program_id) {
  unsigned slot = (unsigned)id - 1;
  if (slot >= VITA_ROTATION_JOBS || !vita_rotation_targets[slot]) abort();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vita_rotation_targets[slot]);
  glUniform2f(vita_atlas_size[program_id], (float)vita_rotation_target_w[slot],
              (float)vita_rotation_target_h[slot]);
}
#endif
void YglVitaRotationShutdown(void) {
#ifdef VITA_ROTATION_BYTE_RESIDENT
  vita_rotation_byte_valid=vita_rotation_byte_jobs=vita_rotation_byte_hits=0;
  free(vita_rotation_byte_check); vita_rotation_byte_check=NULL;
#endif
#ifdef VITA_ROTATION_OUTPUT_REUSE
  for (unsigned i=0;i<VITA_ROTATION_JOBS;++i) {
    free(vita_rotation_previous[i]); vita_rotation_previous[i]=NULL;
    vita_rotation_previous_valid[i]=0;
  }
  vita_rotation_output_jobs=vita_rotation_output_hits=0;
#ifdef VITA_ROTATION_OUTPUT_GENERATION
  vita_rotation_identity_hits=0;
#endif
#endif
#ifdef VITA_ROTATION_PATTERN_CACHE
  for(unsigned i=0;i<VITA_ROTATION_JOBS;++i) {
    if (vita_rotation_patterns[i].texture)
      glDeleteTextures(1,&vita_rotation_patterns[i].texture);
  }
  memset(vita_rotation_patterns,0,sizeof(vita_rotation_patterns));
  vita_rotation_pattern_hits=vita_rotation_pattern_builds=0;
  vita_rotation_pattern_attempts=vita_rotation_pattern_fallbacks=0;
#endif
#ifdef VITA_ROTATION_MAP_CACHE
  if (vita_rotation_map_fbo) glDeleteFramebuffers(1, &vita_rotation_map_fbo);
  if (vita_rotation_map_texture) glDeleteTextures(1, &vita_rotation_map_texture);
  vita_rotation_map_fbo = vita_rotation_map_texture = 0;
  vita_rotation_map_width = vita_rotation_map_height = 0;
  vita_rotation_map_attempts = vita_rotation_map_budget_rejections = 0;
  vita_rotation_map_hits = vita_rotation_map_misses = vita_rotation_map_fallbacks = 0;
  memset(&vita_rotation_map_policy, 0, sizeof(vita_rotation_map_policy));
#endif
#ifdef VITA_ROTATION_FLOAT_COEFFICIENTS
  if (vita_rotation_float_coeff) glDeleteTextures(1, &vita_rotation_float_coeff);
  vita_rotation_float_coeff = 0;
#endif
#ifdef VITA_ROTATION_COEFF_PAIR
  if (vita_rotation_coeff_pair) glDeleteTextures(1,&vita_rotation_coeff_pair);
  vita_rotation_coeff_pair=0;
  vita_rotation_pair_jobs=0;
#endif
  if (vita_rotation_fbo) glDeleteFramebuffers(1, &vita_rotation_fbo);
  if (vita_rotation_vram) glDeleteTextures(1, &vita_rotation_vram);
#ifdef VITA_ROTATION_TARGET
  for (unsigned i = 0; i < VITA_ROTATION_JOBS; ++i) {
    if (vita_rotation_targets[i]) glDeleteTextures(1, &vita_rotation_targets[i]);
    vita_rotation_targets[i] = 0; vita_rotation_target_w[i] = vita_rotation_target_h[i] = 0;
  }
#endif
#ifdef VITA_ROTATION_VRAM_REUSE
  if (vita_rotation_coeff) glDeleteTextures(1, &vita_rotation_coeff);
  vita_rotation_coeff = 0; vita_rotation_uploaded = 0;
  /* The next admitted job must copy: its generation was never uploaded. */
  vita_rotation_generation = 0;
#endif
  for (unsigned i=0; i<sizeof(vita_rotation_programs)/sizeof(vita_rotation_programs[0]); ++i) {
    if (vita_rotation_programs[i].program) glDeleteProgram(vita_rotation_programs[i].program);
  }
  memset(vita_rotation_programs, 0, sizeof(vita_rotation_programs));
  vita_rotation_fbo = vita_rotation_vram = 0;
  for (unsigned i=0; i<VITA_ROTATION_JOBS; ++i) { free(vita_rotation_jobs[i]); vita_rotation_jobs[i] = NULL; }
  free(vita_rotation_vertices); vita_rotation_vertices = NULL;
  vita_rotation_count = 0; vita_rotation_written = 0;
}
#endif
