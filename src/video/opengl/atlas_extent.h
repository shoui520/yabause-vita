/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ATLAS_EXTENT_H
#define YGL_ATLAS_EXTENT_H
/* At most two private rotation rectangles can be excluded. Keeping the three
 * highest allocation bottoms is sufficient to retain the highest non-excluded
 * rectangle, including ties. No per-cell row bitmap or dynamic allocation. */
typedef struct { unsigned x,y,w,h; } YglAtlasRect;
typedef struct { YglAtlasRect top[3]; unsigned count; } YglAtlasExtent;
static inline void YglAtlasExtentAdd(YglAtlasExtent *e,unsigned x,unsigned y,
                                    unsigned w,unsigned h) {
  YglAtlasRect r={x,y,w,h};
  for(unsigned i=0;i<3;++i) {
    if(i==e->count) { e->top[i]=r; ++e->count; return; }
    if(r.y+r.h > e->top[i].y+e->top[i].h) {
      YglAtlasRect old=e->top[i]; e->top[i]=r; r=old;
    }
  }
}
static inline unsigned YglAtlasExtentHeight(const YglAtlasExtent *e,
    int (*excluded)(unsigned,unsigned,unsigned,unsigned)) {
  for(unsigned i=0;i<e->count;++i) {
    const YglAtlasRect *r=&e->top[i];
    if(!excluded(r->x,r->y,r->w,r->h)) return r->y+r->h;
  }
  return 0;
}
#endif
