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
 * how the quads are batched into programs may differ.
 *
 * When only pattern names changed, the other positions are replayed and the
 * changed ones drawn by the walk's own code (Vdp2ReplayNames). Their new
 * cells go past the record's end, so a patched record keeps its quads, though
 * no longer the atlas layout a walk would give. */
#include <stddef.h>

typedef struct { s16 x, y; u16 ax, ay; u8 flip, priority; u16 pos; } Vdp2ReplayQuad;
/* One map position the walk visited: its pattern name address and what
 * Vdp2DrawMapTest passes to PlaneAddr, Vdp2PatternAddrPos and
 * Vdp2DrawPatternPos for it. */
typedef struct { u32 pn; s16 x, y; u8 map, planex, pagex, planey, pagey; } Vdp2ReplayPos;
typedef struct { u64 addr; YglCache c; } Vdp2ReplayCache;
#ifdef VITA_VDP2_CELL_STORE
/* A cell store entry the walk drew from (Vdp2StoreQuad), by its placement
 * serial: a replay needs it still there and in the current storage. */
typedef struct { void *e; unsigned serial; } Vdp2ReplayStoreRef;
static int Vdp2StoreRefValid(const Vdp2ReplayStoreRef *ref);
static void Vdp2StoreRefUse(const Vdp2ReplayStoreRef *ref);
#endif
enum { VDP2_REPLAY_SLOTS = 5, VDP2_REPLAY_KEY_INTS = 64 };
typedef struct {
  int valid;
  unsigned frame, gen;            /* atlas frame and storage generation recorded in */
  int key[VDP2_REPLAY_KEY_INTS];
  Vdp2 regs;
  u32 page_ver[128];
  u32 pages[4];                   /* 4 KiB VRAM pages the walk read */
  u32 chr_pages[4];               /* those it read character data from */
  u8 *page_data;                  /* their bytes, in page order */
  unsigned page_cap;              /* pages page_data holds */
  int has_cram;
  u8 cram[0x1000];
  unsigned start_x, start_y, start_ymax, start_cash;
  u64 start_sig;
  unsigned end_x, end_y, end_ymax, end_cash;
  u64 end_sig;
  YglAtlasExtent end_extent;
  unsigned reuse_n;               /* cell reuse records the walk consumed */
  Vdp2ReplayQuad *quads;
  unsigned nquads, capquads;
  Vdp2ReplayCache *caches;
  unsigned ncaches, capcaches;
  Vdp2ReplayPos *pos;
  unsigned npos, cappos;
#ifdef VITA_VDP2_CELL_STORE
  Vdp2ReplayStoreRef *stores;
  unsigned nstores, capstores;
#endif
  int broken;                     /* something the record cannot express */
  int patched;                    /* holds cells Vdp2ReplayNames redrew, past the walk's */
} Vdp2LayerRecord;

static Vdp2LayerRecord vdp2_replay[4][VDP2_REPLAY_SLOTS];
static Vdp2LayerRecord *vdp2_layer_rec; /* the layer being recorded, else NULL */
static u32 vdp2_replay_pn;              /* the pattern name address just read */
static unsigned vdp2_replay_pos_cur;    /* the position being drawn */
#ifdef VITA_VDP2_LAYER_REPLAY_VERIFY
static Vdp2LayerRecord vdp2_replay_check;
#endif
/* Replayed layers whose cache entries are not in the table yet: the frame
 * often looks nothing up after them. The first starts at pend_cash/sig. */
enum { VDP2_REPLAY_PENDING = 16 };
static const Vdp2LayerRecord *vdp2_replay_pend[VDP2_REPLAY_PENDING];
static unsigned vdp2_replay_npend, vdp2_replay_pend_cash;
static u64 vdp2_replay_pend_sig;

static void Vdp2ReplayCacheFlush(YglTextureManager *tm) {
  const unsigned n = tm->vita_cache_pending ? vdp2_replay_npend : 0;
  tm->vita_cache_pending = 0;
  vdp2_replay_npend = 0;
  if (!n) return;
  tm->CashLink_index = vdp2_replay_pend_cash;
  tm->vita_cache_sig = vdp2_replay_pend_sig;
  for (unsigned i = 0; i < n; ++i)
    for (unsigned j = 0; j < vdp2_replay_pend[i]->ncaches; ++j)
      YglCacheAdd(tm, vdp2_replay_pend[i]->caches[j].addr, &vdp2_replay_pend[i]->caches[j].c);
}

static void Vdp2ReplayCacheDefer(YglTextureManager *tm, const Vdp2LayerRecord *r) {
  if (!tm->vita_cache_pending) vdp2_replay_npend = 0;
  if (vdp2_replay_npend == VDP2_REPLAY_PENDING) Vdp2ReplayCacheFlush(tm);
  if (!vdp2_replay_npend) { vdp2_replay_pend_cash = tm->CashLink_index; vdp2_replay_pend_sig = tm->vita_cache_sig; }
  vdp2_replay_pend[vdp2_replay_npend++] = r;
  tm->vita_cache_flush = Vdp2ReplayCacheFlush;
  tm->vita_cache_pending = 1;
  tm->CashLink_index = r->end_cash;
  tm->vita_cache_sig = r->end_sig;
}

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

static int Vdp2ReplayStoresValid(const Vdp2LayerRecord *r) {
#ifdef VITA_VDP2_CELL_STORE
  for (unsigned i = 0; i < r->nstores; ++i) if (!Vdp2StoreRefValid(&r->stores[i])) return 0;
#else
  (void)r;
#endif
  return 1;
}

/* Everything but VRAM matches. */
static int Vdp2ReplayStateSame(const Vdp2LayerRecord *r, const int *key, const YglTextureManager *tm) {
  extern unsigned vita_atlas_gen, vita_atlas_prev_frame;
  return r->valid && r->frame == vita_atlas_prev_frame && r->gen == vita_atlas_gen &&
         r->start_x == tm->currentX && r->start_y == tm->currentY && r->start_ymax == tm->yMax &&
         r->start_cash == tm->CashLink_index && r->start_sig == tm->vita_cache_sig &&
         !memcmp(r->key, key, sizeof(r->key)) && Vdp2ReplayRegsSame(&r->regs, fixVdp2Regs) &&
         (!r->has_cram || !memcmp(r->cram, Vdp2ColorRam, sizeof(r->cram))) && Vdp2ReplayStoresValid(r);
}

static int Vdp2ReplayMatches(const Vdp2LayerRecord *r, const int *key, const YglTextureManager *tm) {
  return Vdp2ReplayStateSame(r, key, tm) && Vdp2ReplayPagesSame(r);
}

/* Every page that changed was read for pattern names only. */
static int Vdp2ReplayNamesOnly(const Vdp2LayerRecord *r) {
  unsigned k = 0;
  for (unsigned w = 0; w < 4; ++w)
    for (u32 m = r->pages[w]; m; m &= m - 1, ++k) {
      const unsigned page = w * 32 + __builtin_ctz(m);
      if ((r->chr_pages[w] >> (page & 31) & 1) && r->page_ver[page] != vdp2_page_ver[page] &&
          memcmp(r->page_data + (size_t)k * 4096, Vdp2Ram + page * 4096, 4096)) return 0;
    }
  return 1;
}

/* A record whose layer differs from it only in pattern names. */
static Vdp2LayerRecord *Vdp2ReplayFindNames(int id, const int *key, const YglTextureManager *tm) {
  for (int i = 0; i < VDP2_REPLAY_SLOTS; ++i) {
    Vdp2LayerRecord *r = &vdp2_replay[id][i];
    if (Vdp2ReplayStateSame(r, key, tm) && Vdp2ReplayNamesOnly(r)) return r;
  }
  return NULL;
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
                            YglTextureManager *tm) {
  extern unsigned vita_atlas_frame, vita_atlas_gen;
  /* r may be a pending replay; the walk also uses the table. */
  if (tm->vita_cache_pending) Vdp2ReplayCacheFlush(tm);
  r->valid = 0; r->broken = 0; r->patched = 0;
  r->frame = vita_atlas_frame; r->gen = vita_atlas_gen;
  memcpy(r->key, key, sizeof(r->key));
  memcpy(&r->regs, fixVdp2Regs, sizeof(r->regs));
  memcpy(r->page_ver, vdp2_page_ver, sizeof(r->page_ver));
  memset(r->pages, 0, sizeof(r->pages));
  memset(r->chr_pages, 0, sizeof(r->chr_pages));
  r->npos = 0;
  r->has_cram = info->specialcolormode == 3;
  if (r->has_cram) memcpy(r->cram, Vdp2ColorRam, sizeof(r->cram));
  r->start_x = tm->currentX; r->start_y = tm->currentY; r->start_ymax = tm->yMax;
  r->start_cash = tm->CashLink_index; r->start_sig = tm->vita_cache_sig;
  r->reuse_n = 0; r->nquads = 0; r->ncaches = 0;
#ifdef VITA_VDP2_CELL_STORE
  r->nstores = 0;
#endif
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
  r->end_cash = tm->CashLink_index; r->end_sig = tm->vita_cache_sig;
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
                                            (u8)tile->flipfunction, (u8)tile->priority,
                                            (u16)vdp2_replay_pos_cur };
}

static void Vdp2ReplayMark(u32 *pages, u32 addr, u32 bytes) {
  if (!bytes) return;
  if (bytes >= 0x80000) { memset(pages, 0xFF, 4 * sizeof(*pages)); return; }
  for (u32 page = (addr & 0x7FFFF) >> 12, last = ((addr + bytes - 1) & 0x7FFFF) >> 12;; page = (page + 1) & 127) {
    pages[page >> 5] |= 1u << (page & 31);
    if (page == last) break;
  }
}

/* The walk read pattern name bytes [addr, addr + bytes). */
static void Vdp2ReplayReadName(u32 addr, u32 bytes) {
  Vdp2ReplayMark(vdp2_layer_rec->pages, addr, bytes);
  vdp2_replay_pn = addr;
}

/* The walk read character bytes [addr, addr + bytes). */
static void Vdp2ReplayRead(u32 addr, u32 bytes) {
  Vdp2ReplayMark(vdp2_layer_rec->pages, addr, bytes);
  Vdp2ReplayMark(vdp2_layer_rec->chr_pages, addr, bytes);
}

/* The walk is at a map position; the pattern name was just read. */
static void Vdp2ReplayAddPos(int map, int planex, int pagex, int planey, int pagey, int x, int y) {
  Vdp2LayerRecord *r = vdp2_layer_rec;
  if (r->npos == r->cappos) {
    const unsigned cap = r->cappos ? r->cappos * 2 : 4096;
    Vdp2ReplayPos *p = cap <= 65536 ? realloc(r->pos, cap * sizeof(*p)) : NULL;
    if (!p) { r->broken = 1; return; }
    r->pos = p; r->cappos = cap;
  }
  if ((unsigned)map > 255 || (unsigned)planex > 255 || (unsigned)pagex > 255 || (unsigned)planey > 255 ||
      (unsigned)pagey > 255 || x != (s16)x || y != (s16)y) { r->broken = 1; return; }
  vdp2_replay_pos_cur = r->npos;
  r->pos[r->npos++] = (Vdp2ReplayPos){ vdp2_replay_pn, (s16)x, (s16)y, (u8)map, (u8)planex, (u8)pagex,
                                       (u8)planey, (u8)pagey };
}

/* The quad just added drew nothing and was taken back. */
static void Vdp2ReplayDropQuad(void) {
  if (vdp2_layer_rec->nquads) --vdp2_layer_rec->nquads;
  else vdp2_layer_rec->broken = 1;
}

#ifdef VITA_VDP2_CELL_STORE
static void Vdp2ReplayAddStore(void *e, unsigned serial) {
  Vdp2LayerRecord *r = vdp2_layer_rec;
  if (r->nstores == r->capstores) {
    const unsigned cap = r->capstores ? r->capstores * 2 : 1024;
    Vdp2ReplayStoreRef *p = realloc(r->stores, cap * sizeof(*p));
    if (!p) { r->broken = 1; return; }
    r->stores = p; r->capstores = cap;
  }
  r->stores[r->nstores++] = (Vdp2ReplayStoreRef){ e, serial };
}
#endif

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

/* Takes out the quads of the positions marked in drop. */
static void Vdp2ReplayDropPositions(Vdp2LayerRecord *r, const u8 *drop) {
  unsigned n = 0;
  for (unsigned i = 0; i < r->nquads; ++i)
    if (!drop[r->quads[i].pos]) r->quads[n++] = r->quads[i];
  r->nquads = n;
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
#ifdef VITA_VDP2_CELL_STORE
  for (unsigned i = 0; i < r->nstores; ++i) Vdp2StoreRefUse(&r->stores[i]);  /* drawn from in this frame */
#endif
  Vdp2ReplayCacheDefer(tm, r);
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

/* Replays r except at the positions whose pattern names changed, which are
 * drawn as the walk draws them, and updates r to this frame. Returns 0, having
 * drawn nothing, when too much changed to be worth it. */
static int Vdp2ReplayNames(Vdp2LayerRecord *r, vdp2draw_struct *info, YglTexture *texture, YglTextureManager *tm) {
  static u8 *changed;
  static unsigned changed_cap;
  if (r->npos > changed_cap) {
    u8 *c = realloc(changed, r->npos);
    if (!c) return 0;
    changed = c; changed_cap = r->npos;
  }
  int page_k[128];
  unsigned k = 0;
  memset(page_k, 0xFF, sizeof(page_k));
  for (unsigned w = 0; w < 4; ++w)
    for (u32 m = r->pages[w]; m; m &= m - 1) page_k[w * 32 + __builtin_ctz(m)] = (int)k++;
  const unsigned bytes = (unsigned)info->patterndatasize * 2;
  unsigned n = 0;
  for (unsigned i = 0; i < r->npos; ++i) {
    const u32 pn = r->pos[i].pn & 0x7FFFF, page = pn >> 12;
    changed[i] = page_k[page] < 0 || (pn & 4095) + bytes > 4096 ||
                 (r->page_ver[page] != vdp2_page_ver[page] &&
                  memcmp(r->page_data + (size_t)page_k[page] * 4096 + (pn & 4095), Vdp2Ram + pn, bytes));
    n += changed[i];
  }
  /* Changed cells allocate past the record's end; walk again before the
   * record holds many more cells than one walk would. */
  if (n > r->npos / 4 || r->ncaches > r->npos * 2) return 0;
  Vdp2ReplayDropPositions(r, changed);
  Vdp2ReplayEmit(r, info, tm);
  Vdp2ReplayCacheFlush(tm);
  r->valid = 0; r->patched = 1;
  vdp2_layer_rec = r;
  for (unsigned i = 0; i < r->npos; ++i) {
    if (!changed[i]) continue;
    const Vdp2ReplayPos *p = &r->pos[i];
    vdp2_replay_pos_cur = i;
    info->PlaneAddr(info, p->map, fixVdp2Regs);
    Vdp2PatternAddrPos(info, p->planex, p->pagex, p->planey, p->pagey);
    Vdp2DrawPatternPos(info, texture, p->x, p->y, 0, 0, info->lineinc);
  }
  Vdp2ReplayEnd(r, tm);
  return 1;
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

/* r's texels, taken before a walk over the same storage replaces them. */
static u32 *vdp2_replay_texels;
static void Vdp2ReplaySaveTexels(const Vdp2LayerRecord *r, const YglTextureManager *tm, int cell, int lines) {
  static size_t cap;
  const size_t need = (size_t)r->nquads * cell * lines;
  if (need > cap) { free(vdp2_replay_texels); vdp2_replay_texels = malloc(need * 4); cap = vdp2_replay_texels ? need : 0; }
  if (!vdp2_replay_texels) return;
  for (unsigned i = 0; i < r->nquads; ++i)
    for (int row = 0; row < lines; ++row)
      memcpy(vdp2_replay_texels + ((size_t)i * lines + row) * cell,
             tm->texture + (size_t)(r->quads[i].ay + row) * tm->width + r->quads[i].ax, (size_t)cell * 4);
}

/* A walk c draws each position as r does: the same quads, over the same
 * texels (r's saved ones when saved is set). */
static int Vdp2ReplayNamesSame(const Vdp2LayerRecord *r, const Vdp2LayerRecord *c, const YglTextureManager *tm,
                               int cell, int lines, int saved) {
  if (saved && !vdp2_replay_texels) return 0;
  if (!c->valid || r->npos != c->npos || r->nquads != c->nquads) return 0;
  static int *at;
  static unsigned at_cap;
  if (r->npos > at_cap) { free(at); at = malloc(r->npos * sizeof(*at)); at_cap = at ? r->npos : 0; if (!at) return 0; }
  memset(at, 0xFF, r->npos * sizeof(*at));
  for (unsigned i = 0; i < r->nquads; ++i) {
    if (at[r->quads[i].pos] >= 0) return 0; /* one quad per position */
    at[r->quads[i].pos] = (int)i;
  }
  for (unsigned i = 0; i < c->nquads; ++i) {
    const Vdp2ReplayQuad *b = &c->quads[i];
    if (at[b->pos] < 0) return 0;
    const Vdp2ReplayQuad *a = &r->quads[at[b->pos]];
    if (a->x != b->x || a->y != b->y || a->flip != b->flip || a->priority != b->priority) return 0;
    for (int row = 0; row < lines; ++row)
      if (memcmp(saved ? vdp2_replay_texels + ((size_t)at[b->pos] * lines + row) * cell
                       : tm->texture + (size_t)(a->ay + row) * tm->width + a->ax,
                 tm->texture + (size_t)(b->ay + row) * tm->width + b->ax, (size_t)cell * 4)) return 0;
  }
  return 1;
}
#endif
