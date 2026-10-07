/* SPDX-License-Identifier: GPL-2.0-or-later
 * Reserve coordinate floats, not a fixed worst-case tessellation batch.
 * On failure the old buffers/capacity remain intact.
 */
#ifdef VITA_GEOM_ARENA
/* VDP1 geometry is written by the CPU straight into GPU-mapped memory and
 * drawn in place through one VBO whose storage is the whole arena, so
 * vitaGL's per-draw copy into uncached memory (bandwidth-bound, about
 * 270 MB/s on the A9) disappears. USER_RW is CPU-cached and the GPU snoops
 * CPU caches on a miss (libgxm Overview, "Memory Block Types"), so no cache
 * maintenance is needed. The arena is a FIFO ring: each VDP1 render retires
 * every block of the VDP1 level with vitaGL's frame number, and a retired
 * range is reused only once that frame is GA_LAG frames old (the display
 * queue admits one pending swap, so frame F-2 has completed while F is
 * recorded). Blocks that do not fit fall back to the heap and the old
 * copying draw path. */
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>
extern uint32_t vgl_framecount;
#define GA_BYTES (16u << 20)
#define GA_LAG 3u
#define GA_FENCES 64u
static uint8_t *ga_base;
static int ga_state;            /* 0 untried, 1 ready, -1 unavailable */
static uint32_t ga_head, ga_tail, ga_live;  /* live bytes incl. wrap waste */
static struct { uint32_t end, bytes, frame; } ga_fence[GA_FENCES];
static unsigned ga_first, ga_count;
static uint32_t ga_pending;     /* bytes allocated since the last fence */
static int ga_want;             /* set while reserving for the VDP1 level */
static uint32_t ga_heap_fallbacks; /* VDP1 blocks that did not fit the arena */
static GLuint ga_vbo;

static int YglGeomOwned(const void *p) {
  return ga_base && (const uint8_t *)p >= ga_base && (const uint8_t *)p < ga_base + GA_BYTES;
}
static int YglGeomArenaInit(void) {
  void *base = NULL;
  SceUID blk = sceKernelAllocMemBlock("yab_geom", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, GA_BYTES, NULL);
  if (blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 ||
      sceGxmMapMemory(base, GA_BYTES, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
    YuiMsg("geom_arena_unavailable blk=%08x", (unsigned)blk);
    if (blk >= 0) sceKernelFreeMemBlock(blk);
    return -1;
  }
  ga_base = base;
  return 0;
}
/* Mapping the arena clears 16 MiB (about 40 ms), so it is done at startup
 * rather than inside the first frame that draws VDP1 geometry. */
static void YglGeomPrepare(void) {
  if (ga_state == 0) ga_state = YglGeomArenaInit() == 0 ? 1 : -1;
}
static void YglGeomReclaim(void) {
  while (ga_count && vgl_framecount - ga_fence[ga_first].frame >= GA_LAG) {
    ga_tail = ga_fence[ga_first].end;
    ga_live -= ga_fence[ga_first].bytes;
    ga_first = (ga_first + 1) % GA_FENCES; --ga_count;
  }
  if (!ga_live) ga_head = ga_tail = 0;
}
static void *YglGeomAlloc(uint32_t bytes) {
  uint32_t at, waste = 0;
  YglGeomPrepare();
  if (ga_state < 0) return NULL;
  bytes = (bytes + 63u) & ~63u;
  YglGeomReclaim();
  if (ga_live == 0 || ga_head > ga_tail) {
    if (GA_BYTES - ga_head >= bytes) at = ga_head;
    else if (ga_tail > bytes) { at = 0; waste = GA_BYTES - ga_head; }
    else return NULL;
  } else {
    if (ga_tail - ga_head > bytes) at = ga_head; else return NULL;
  }
  ga_head = at + bytes;
  ga_live += bytes + waste; ga_pending += bytes + waste;
  return ga_base + at;
}
/* After a VDP1 render: every VDP1-level block is now owned by the GPU.
 * Each slot's next block is sized from what it drew in this render, not
 * from the largest it ever grew to: slots are reassigned to whatever
 * primitives come next, so inherited peaks would ratchet every slot up until
 * a frame's reservations overflow the arena. Heap blocks (the copying draw
 * path, already consumed by the draw) are returned for the same reason; they
 * would otherwise keep their peak size in the heap indefinitely. */
static void YglGeomRetireLevel(YglLevel *level) {
  int j;
  if (ga_state <= 0) return;
  for (j = 0; j < level->prgcount; j++) {
    YglProgram *p = &level->prg[j];
    if (p->currentQuad != 0 || !p->quads) continue;
    if (!YglGeomOwned(p->quads)) {
      free(p->quads); free(p->textcoords); free(p->vertexAttribute);
    }
    p->quads = p->textcoords = p->vertexAttribute = NULL;
    {
      unsigned hint = (unsigned)p->usedQuad + (unsigned)p->usedQuad / 4;
      p->maxQuad = (int)(hint < 1024u ? 1024u : (hint + 1023u) & ~1023u);
    }
    p->usedQuad = 0;
  }
  if (!ga_pending) return;
  if (ga_count == GA_FENCES) {
    glFinish();
    ga_count = 0; ga_live = 0; ga_head = ga_tail = 0;
    for (j = 0; j < level->prgcount; j++) if (YglGeomOwned(level->prg[j].quads)) ga_live = 1;
    if (ga_live) abort();   /* live blocks never survive a retire */
    ga_pending = 0;
    return;
  }
  unsigned slot = (ga_first + ga_count) % GA_FENCES;
  ga_fence[slot].end = ga_head; ga_fence[slot].bytes = ga_pending; ga_fence[slot].frame = vgl_framecount;
  ++ga_count; ga_pending = 0;
}
/* Bind the arena VBO and point the program's attributes at its block. */
static void YglGeomBind(YglProgram *p, GLint *old_array) {
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, old_array);
  if (!ga_vbo) {
    glGenBuffers(1, &ga_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ga_vbo);
    vglBufferData(GL_ARRAY_BUFFER, ga_base);
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, ga_vbo);
  }
  glVertexAttribPointer(p->vertexp, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)((uint8_t *)p->quads - ga_base));
  glVertexAttribPointer(p->texcoordp, 4, GL_FLOAT, GL_FALSE, 0, (GLvoid *)((uint8_t *)p->textcoords - ga_base));
  if (p->vaid != 0)
    glVertexAttribPointer(p->vaid, 4, GL_FLOAT, GL_FALSE, 0, (GLvoid *)((uint8_t *)p->vertexAttribute - ga_base));
}
#else
static int YglGeomOwned(const void *p) { (void)p; return 0; }
#endif

static int YglReserveGeometry(YglProgram *program, unsigned needed) {
  const unsigned limit = 0x7fffffffU / (5 * sizeof(float));
  if (!needed || needed > limit || program->currentQuad < 0 ||
      program->maxQuad < 0 || program->currentQuad > program->maxQuad ||
      (unsigned)program->currentQuad > limit - needed)
    return -1;
  unsigned used = (unsigned)program->currentQuad;
  unsigned required = used + needed;
  if (required <= (unsigned)program->maxQuad && program->quads &&
      program->textcoords && program->vertexAttribute) return 0;
#ifdef VITA_TESS_WORKER
  YglTessDrain();   /* queued grids write into the buffers being replaced */
#endif
  unsigned capacity = (unsigned)program->maxQuad;
  if (capacity > limit) return -1;
  capacity = capacity > limit / 2 ? limit : capacity * 2;
  if (capacity < required) capacity = required;
  float *positions = NULL, *texcoords = NULL, *attributes = NULL;
#ifdef VITA_GEOM_ARENA
  if (ga_want && capacity <= (GA_BYTES / 4) / (5 * sizeof(float))) {
    /* A retired block leaves its capacity behind as the size hint. */
    if (!program->quads) capacity = (unsigned)program->maxQuad;
    if (capacity < required) capacity = required;
    float *block = YglGeomAlloc(capacity * 5 * sizeof(float));
    if (block) { positions = block; texcoords = block + capacity; attributes = block + capacity * 3; }
    else ++ga_heap_fallbacks;
  }
  if (!positions)
#endif
  {
    positions = malloc(capacity * sizeof(float));
    texcoords = malloc(capacity * 2 * sizeof(float));
    attributes = malloc(capacity * 2 * sizeof(float));
  }
  if (!positions || !texcoords || !attributes ||
      (used && (!program->quads || !program->textcoords || !program->vertexAttribute))) {
    if (!YglGeomOwned(positions)) { free(positions); free(texcoords); free(attributes); }
    return -1;
  }
  if (used) {
    memcpy(positions, program->quads, used * sizeof(float));
    memcpy(texcoords, program->textcoords, used * 2 * sizeof(float));
    memcpy(attributes, program->vertexAttribute, used * 2 * sizeof(float));
  }
  if (!YglGeomOwned(program->quads)) {
    free(program->quads); free(program->textcoords); free(program->vertexAttribute);
  }
  program->quads = positions;
  program->textcoords = texcoords;
  program->vertexAttribute = attributes;
  program->maxQuad = (int)capacity;
  return 0;
}
