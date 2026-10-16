/* SPDX-License-Identifier: GPL-2.0-or-later
 * Replay of an unchanged VDP2 cell layer (Vdp2DrawMapTest). Included by
 * vidogl.c after the cell reuse records.
 *
 * A layer's quads depend on its registers, VRAM, the colour RAM (only the
 * MSBs of mode 3), and the atlas allocator and texture cache when it starts.
 * When all of those equal the frame its atlas storage last held, the quads,
 * cache entries and allocator advance are that frame's, and the texels are
 * still in the storage. They are re-emitted here without the map walk,
 * through the program choice and vertex writes of YglCachedQuadOffset; only
 * how the quads are batched into programs may differ. */
#include <stddef.h>

typedef struct { s16 x, y; u16 ax, ay; u8 flip, priority; } Vdp2ReplayQuad;
typedef struct { u64 addr; YglCache c; } Vdp2ReplayCache;
enum { VDP2_REPLAY_SLOTS = 5, VDP2_REPLAY_KEY_INTS = 64 };
typedef struct {
  int valid;
  unsigned frame, gen;            /* atlas frame and storage generation recorded in */
  int key[VDP2_REPLAY_KEY_INTS];
  Vdp2 regs;
  u32 page_ver[128];
  u32 pages[4];                   /* 4 KiB VRAM pages the walk read */
  u8 *page_data;                  /* their bytes, in page order */
  unsigned page_cap;              /* pages page_data holds */
  int has_cram;
  u8 cram[0x1000];
  unsigned start_x, start_y, start_ymax, start_cash;
  u64 start_sig;
  unsigned end_x, end_y, end_ymax;
  YglAtlasExtent end_extent;
  unsigned reuse_n;               /* cell reuse records the walk consumed */
  Vdp2ReplayQuad *quads;
  unsigned nquads, capquads;
  Vdp2ReplayCache *caches;
  unsigned ncaches, capcaches;
  int broken;                     /* something the record cannot express */
} Vdp2LayerRecord;

static Vdp2LayerRecord vdp2_replay[4][VDP2_REPLAY_SLOTS];
static Vdp2LayerRecord *vdp2_layer_rec; /* the layer being recorded, else NULL */
#ifdef VITA_VDP2_LAYER_REPLAY_VERIFY
static Vdp2LayerRecord vdp2_replay_check;
#endif

/* Layers whose walk reads nothing per line or per window. */
static int Vdp2ReplayEligible(const vdp2draw_struct *info) {
  return info->id >= 0 && info->id < 4 && !info->isverticalscroll && !info->islinescroll &&
         info->coordincx == 1.0f && info->coordincy == 1.0f && !info->bEnWin0 && !info->bEnWin1 &&
         info->linescreen == 0 && info->lineTexture == 0 && info->mosaicxmask == 1 &&
         info->mosaicymask == 1;
}

/* The layer inputs that are not registers. Per-pattern fields (charaddr,
 * paladdr, flipfunction, specialfunction, specialcolorfunction) are
 * excluded: the walk sets them. */
static void Vdp2ReplayKey(const vdp2draw_struct *info, int *k) {
  int n = 0;
#define K(v) (k[n++] = (int)(v))
  K(info->id); K(info->priority); K(info->specialprimode); K(info->blendmode); K(info->linescreen);
  K(info->lineTexture); K(info->mosaicxmask); K(info->mosaicymask); K(info->cor); K(info->cog);
  K(info->cob); K(info->colornumber); K(info->patternwh); K(info->patterndatasize); K(info->planew);
  K(info->planeh); K(info->pagewh); K(info->mapwh); K(info->supplementdata); K(info->auxmode);
  K(info->x); K(info->y); K(info->alpha); K(info->coloroffset); K(info->transparencyenable);
  K(info->specialcolormode); K(info->specialcode);
  K(info->isverticalscroll); K(info->islinescroll); K(info->bEnWin0); K(info->bEnWin1);
  K(info->LogicWin); K(info->WindowArea0); K(info->WindowArea1); K(info->lineinc);
  K(info->patternpixelwh); K(info->draww); K(info->drawh);
  for (int i = 0; i < 4; ++i) { K(info->char_bank[i]); K(info->pname_bank[i]); }
  K((uintptr_t)info->PlaneAddr); K(vdp2width); K(vdp2height); K(Vdp2Internal.ColorMode);
#undef K
  while (n < VDP2_REPLAY_KEY_INTS) k[n++] = 0;
}

/* Each page the walk read is unwritten since, or rewritten with the same
 * bytes (games that copy a table every frame). */
static int Vdp2ReplayPagesSame(const Vdp2LayerRecord *r) {
  unsigned k = 0;
  for (unsigned w = 0; w < 4; ++w)
    for (u32 m = r->pages[w]; m; m &= m - 1, ++k) {
      const unsigned page = w * 32 + __builtin_ctz(m);
      if (r->page_ver[page] != vdp2_page_ver[page] &&
          memcmp(r->page_data + (size_t)k * 4096, Vdp2Ram + page * 4096, 4096)) return 0;
    }
  return 1;
}

/* The registers, apart from the rotation screens' map registers (MPABRA to
 * MPOPRB), which only RBG0/RBG1 read. */
_Static_assert(offsetof(Vdp2, SCXIN0) - offsetof(Vdp2, MPABRA) == 16 * sizeof(u16), "Vdp2 register layout");
static int Vdp2ReplayRegsSame(const Vdp2 *a, const Vdp2 *b) {
  return !memcmp(a, b, offsetof(Vdp2, MPABRA)) &&
         !memcmp((const u8 *)a + offsetof(Vdp2, SCXIN0), (const u8 *)b + offsetof(Vdp2, SCXIN0),
                 sizeof(Vdp2) - offsetof(Vdp2, SCXIN0));
}

static int Vdp2ReplayMatches(const Vdp2LayerRecord *r, const int *key, const YglTextureManager *tm) {
  extern unsigned vita_atlas_gen, vita_atlas_prev_frame;
  return r->valid && r->frame == vita_atlas_prev_frame && r->gen == vita_atlas_gen &&
         r->start_x == tm->currentX && r->start_y == tm->currentY && r->start_ymax == tm->yMax &&
         r->start_cash == tm->CashLink_index && r->start_sig == tm->vita_cache_sig &&
         !memcmp(r->key, key, sizeof(r->key)) && Vdp2ReplayRegsSame(&r->regs, fixVdp2Regs) &&
         Vdp2ReplayPagesSame(r) &&
         (!r->has_cram || !memcmp(r->cram, Vdp2ColorRam, sizeof(r->cram)));
}

static Vdp2LayerRecord *Vdp2ReplayFind(int id, const int *key, const YglTextureManager *tm) {
  for (int i = 0; i < VDP2_REPLAY_SLOTS; ++i)
    if (Vdp2ReplayMatches(&vdp2_replay[id][i], key, tm)) return &vdp2_replay[id][i];
  return NULL;
}

/* The record the storage's last frame left, else an empty or the oldest. */
static Vdp2LayerRecord *Vdp2ReplaySlot(int id) {
  extern unsigned vita_atlas_frame, vita_atlas_prev_frame;
  Vdp2LayerRecord *pick = NULL;
  unsigned oldest = 0;
  for (int i = 0; i < VDP2_REPLAY_SLOTS; ++i) {
    Vdp2LayerRecord *r = &vdp2_replay[id][i];
    if (r->valid && r->frame == vita_atlas_prev_frame) return r;
    const unsigned age = r->valid ? vita_atlas_frame - r->frame : ~0u;
    if (!pick || age > oldest) { pick = r; oldest = age; }
  }
  return pick;
}

static void Vdp2ReplayBegin(Vdp2LayerRecord *r, const vdp2draw_struct *info, const int *key,
                            const YglTextureManager *tm) {
  extern unsigned vita_atlas_frame, vita_atlas_gen;
  r->valid = 0; r->broken = 0;
  r->frame = vita_atlas_frame; r->gen = vita_atlas_gen;
  memcpy(r->key, key, sizeof(r->key));
  memcpy(&r->regs, fixVdp2Regs, sizeof(r->regs));
  memcpy(r->page_ver, vdp2_page_ver, sizeof(r->page_ver));
  memset(r->pages, 0, sizeof(r->pages));
  r->has_cram = info->specialcolormode == 3;
  if (r->has_cram) memcpy(r->cram, Vdp2ColorRam, sizeof(r->cram));
  r->start_x = tm->currentX; r->start_y = tm->currentY; r->start_ymax = tm->yMax;
  r->start_cash = tm->CashLink_index; r->start_sig = tm->vita_cache_sig;
  r->reuse_n = 0; r->nquads = 0; r->ncaches = 0;
  vdp2_layer_rec = r;
}

static void Vdp2ReplayEnd(Vdp2LayerRecord *r, const YglTextureManager *tm) {
  extern unsigned vita_atlas_gen;
  vdp2_layer_rec = NULL;
  if (r->broken || r->gen != vita_atlas_gen) return;
  unsigned n = 0;
  for (unsigned w = 0; w < 4; ++w) n += __builtin_popcount(r->pages[w]);
  if (n > r->page_cap) {
    u8 *data = realloc(r->page_data, (size_t)n * 4096);
    if (!data) return;
    r->page_data = data; r->page_cap = n;
  }
  unsigned k = 0;
  for (unsigned w = 0; w < 4; ++w)
    for (u32 m = r->pages[w]; m; m &= m - 1, ++k)
      memcpy(r->page_data + (size_t)k * 4096, Vdp2Ram + (w * 32 + __builtin_ctz(m)) * 4096, 4096);
  r->end_x = tm->currentX; r->end_y = tm->currentY; r->end_ymax = tm->yMax;
  r->end_extent = tm->vita_cpu_extent;
  r->valid = 1;
}

static void Vdp2ReplayAddQuad(const vdp2draw_struct *tile, const YglCache *c) {
  Vdp2LayerRecord *r = vdp2_layer_rec;
  if (r->nquads == r->capquads) {
    const unsigned cap = r->capquads ? r->capquads * 2 : 4096;
    Vdp2ReplayQuad *q = realloc(r->quads, cap * sizeof(*q));
    if (!q) { r->broken = 1; return; }
    r->quads = q; r->capquads = cap;
  }
  const float x = tile->vertices[0], y = tile->vertices[1];
  if (c->x < 0.0f || c->y < 0.0f || c->x > 65535.0f || c->y > 65535.0f ||
      x != (float)(s16)x || y != (float)(s16)y || c->x != (float)(u16)c->x || c->y != (float)(u16)c->y ||
      tile->flipfunction < 0 || tile->flipfunction > 255 || tile->priority < 0 || tile->priority > 255) {
    r->broken = 1; return;
  }
  r->quads[r->nquads++] = (Vdp2ReplayQuad){ (s16)x, (s16)y, (u16)c->x, (u16)c->y,
                                            (u8)tile->flipfunction, (u8)tile->priority };
}

/* The walk read VRAM bytes [addr, addr + bytes). */
static void Vdp2ReplayRead(u32 addr, u32 bytes) {
  u32 *pages = vdp2_layer_rec->pages;
  if (!bytes) return;
  if (bytes >= 0x80000) { memset(pages, 0xFF, 4 * sizeof(*pages)); return; }
  for (u32 page = (addr & 0x7FFFF) >> 12, last = ((addr + bytes - 1) & 0x7FFFF) >> 12;; page = (page + 1) & 127) {
    pages[page >> 5] |= 1u << (page & 31);
    if (page == last) break;
  }
}

/* The quad just added drew nothing and was taken back. */
static void Vdp2ReplayDropQuad(void) {
  if (vdp2_layer_rec->nquads) --vdp2_layer_rec->nquads;
  else vdp2_layer_rec->broken = 1;
}

static void Vdp2ReplayAddCache(u64 addr, const YglCache *c) {
  Vdp2LayerRecord *r = vdp2_layer_rec;
  if (r->ncaches == r->capcaches) {
    const unsigned cap = r->capcaches ? r->capcaches * 2 : 4096;
    Vdp2ReplayCache *e = realloc(r->caches, cap * sizeof(*e));
    if (!e) { r->broken = 1; return; }
    r->caches = e; r->capcaches = cap;
  }
  r->caches[r->ncaches++] = (Vdp2ReplayCache){ addr, *c };
}

/* Vdp2DrawPatternPos's tile for this layer; the walk varies only the
 * vertices, flip and priority. */
static void Vdp2ReplayTile(const vdp2draw_struct *info, vdp2draw_struct *tile) {
  memset(tile, 0, sizeof(*tile));
  tile->colornumber = info->colornumber;
  tile->blendmode = info->blendmode;
  tile->linescreen = info->linescreen;
  tile->mosaicxmask = info->mosaicxmask;
  tile->mosaicymask = info->mosaicymask;
  tile->bEnWin0 = info->bEnWin0;
  tile->WindowArea0 = info->WindowArea0;
  tile->bEnWin1 = info->bEnWin1;
  tile->WindowArea1 = info->WindowArea1;
  tile->LogicWin = info->LogicWin;
  tile->lineTexture = info->lineTexture;
  tile->id = info->id;
  tile->specialprimode = info->specialprimode;
  tile->specialcolormode = info->specialcolormode;
  tile->cellw = tile->cellh = info->patternpixelwh;
  tile->cor = info->cor;
  tile->cog = info->cog;
  tile->cob = info->cob;
}

static void Vdp2ReplayEmit(Vdp2LayerRecord *r, const vdp2draw_struct *info, YglTextureManager *tm) {
  extern unsigned vita_atlas_frame, vita_atlas_prev_frame;
  vdp2draw_struct tile;
  Vdp2ReplayTile(info, &tile);
  const int lines = (int)info->lineinc;
  /* Runs of one priority (one level) share a program; a run is split well
   * under the per-program vertex limit, so only the batching differs. */
  for (unsigned i = 0; i < r->nquads;) {
    unsigned end = i + 1;
    while (end < r->nquads && end - i < 4096 && r->quads[end].priority == r->quads[i].priority) ++end;
    tile.priority = r->quads[i].priority;
    tile.flipfunction = r->quads[i].flip;
    YglProgram *program = YglCachedQuadsBegin(&tile, end - i);
    for (; program && i < end; ++i) {
      const Vdp2ReplayQuad *q = &r->quads[i];
      tile.flipfunction = q->flip;
      tile.vertices[0] = q->x;
      tile.vertices[1] = q->y;
      tile.vertices[2] = q->x + tile.cellw;
      tile.vertices[3] = q->y;
      tile.vertices[4] = q->x + tile.cellh;
      tile.vertices[5] = q->y + lines;
      tile.vertices[6] = q->x;
      tile.vertices[7] = q->y + lines;
      const YglCache c = { q->ax, q->ay };
      YglCachedQuadsWrite(program, &tile, &c);
    }
    i = end;
  }
  for (unsigned i = 0; i < r->ncaches; ++i) YglCacheAdd(tm, r->caches[i].addr, &r->caches[i].c);
  tm->currentX = r->end_x; tm->currentY = r->end_y; tm->yMax = r->end_ymax;
  tm->vita_cpu_extent = r->end_extent;
  /* The walk's reuse records still describe these rectangles. */
  for (unsigned i = 0; i < r->reuse_n; ++i) {
    Vdp2CellReuseRecord *rec = Vdp2CellReuseNext(&vdp2_cell_reuse_seq, vita_atlas_frame, vita_atlas_prev_frame);
    if (rec) rec->frame = vita_atlas_frame;
  }
  r->frame = vita_atlas_frame;
  /* The pages read compared equal: their current stamps describe the bytes. */
  memcpy(r->page_ver, vdp2_page_ver, sizeof(r->page_ver));
}

#ifdef VITA_VDP2_LAYER_REPLAY_VERIFY
/* The walk just recorded into c should equal the replay r would have done. */
static int Vdp2ReplaySame(const Vdp2LayerRecord *r, const Vdp2LayerRecord *c) {
  return c->valid && r->nquads == c->nquads && r->ncaches == c->ncaches && r->reuse_n == c->reuse_n &&
         r->end_x == c->end_x && r->end_y == c->end_y && r->end_ymax == c->end_ymax &&
         !memcmp(&r->end_extent, &c->end_extent, sizeof(r->end_extent)) &&
         !memcmp(r->quads, c->quads, r->nquads * sizeof(*r->quads)) &&
         !memcmp(r->caches, c->caches, r->ncaches * sizeof(*r->caches));
}
#endif
