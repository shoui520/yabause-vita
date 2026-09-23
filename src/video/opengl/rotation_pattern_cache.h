/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ROTATION_PATTERN_CACHE_H
#define YGL_ROTATION_PATTERN_CACHE_H
#include <stdint.h>
#include <stddef.h>

/* VDP2 manual 4.6, figures 4.9/4.10: 26 bits per decoded pattern name.
 * Four explicit texture bytes: character[7:0], character[14:8], palette,
 * flip[1:0] | PR<<2 | CC<<3. Character remains a number, not a byte address.
 * This is NOT a decoded-pixel cache: character pixels and CRAM stay live.
 */
typedef struct {
  uint32_t generation, width, height, shift_x, shift_y, pages_x;
  uint32_t cells, words, aux, supplement, format, planes[16];
} YglRotationPatternKey;

static inline int YglRotationPatternAdmit(const YglRotationPatternKey *k) {
  if ((k->shift_x != 9 && k->shift_x != 10) ||
      (k->shift_y != 9 && k->shift_y != 10) ||
      k->pages_x != (1u << (k->shift_x - 9)) ||
      (k->cells != 1 && k->cells != 2) ||
      (k->words != 1 && k->words != 2) || k->aux > 1 ||
      k->supplement > 0x3ff ||
      (k->format != 0 && k->format != 1 && k->format != 3 && k->format != 4)) return 0;
  if (k->width < 512 || k->width > (4u << k->shift_x) ||
      k->height < 512 || k->height > (4u << k->shift_y) ||
      (k->width & (k->width-1)) || (k->height & (k->height-1))) return 0;
  for (unsigned i=0;i<16;++i)
    if (k->planes[i] >= 0x80000 || (k->planes[i] & 1)) return 0;
  return 1;
}

static inline unsigned YglRotationPatternWord(const uint8_t *ram, unsigned a) {
  return ((unsigned)ram[a & 0x7ffff] << 8) | ram[(a+1) & 0x7ffff];
}

static inline void YglRotationPatternDecode(const YglRotationPatternKey *k,
    unsigned name, unsigned second, uint8_t out[4]) {
  unsigned character, palette, flip, pr, cc, s=k->supplement;
  if (k->words == 2) {
    character=second & 0x7fff;
    palette=name & (k->format == 0 ? 0x7f : 0x70);
    flip=(name>>14)&3; pr=(name>>13)&1; cc=(name>>12)&1;
  } else {
    palette=k->format == 0 ? (name>>12)|((s&0xe0)>>1) : (name&0x7000)>>8;
    pr=(s>>9)&1; cc=(s>>8)&1;
    flip=k->aux == 0 ? (name>>10)&3 : 0;
    if (k->aux == 0)
      character=k->cells == 1 ? (name&0x3ff)|((s&0x1f)<<10) :
          ((name&0x3ff)<<2)|(s&3)|((s&0x1c)<<10);
    else
      character=k->cells == 1 ? (name&0xfff)|((s&0x1c)<<10) :
          ((name&0xfff)<<2)|(s&3)|((s&0x10)<<10);
  }
  out[0]=(uint8_t)character; out[1]=(uint8_t)(character>>8);
  out[2]=(uint8_t)palette; out[3]=(uint8_t)(flip|(pr<<2)|(cc<<3));
}

static inline size_t YglRotationPatternBytes(const YglRotationPatternKey *k) {
  if (!YglRotationPatternAdmit(k)) return 0;
  return (size_t)(k->width/(k->cells*8))*(k->height/(k->cells*8))*4;
}

/* Row-major logical tiles. Collapse plane/page indirection once per source
 * generation, not once per shaded pixel. Wrapped big-endian reads preserve
 * the existing shader's 512 KiB VRAM address semantics.
 */
static inline int YglRotationPatternBuild(const YglRotationPatternKey *k,
    const uint8_t *ram, uint8_t *out, size_t capacity) {
  size_t needed=YglRotationPatternBytes(k);
  if (!ram || !out || !needed || capacity < needed) return 0;
  unsigned shift=2+k->cells, page_width=512>>shift;
  unsigned tw=k->width>>shift, th=k->height>>shift;
  for (unsigned ty=0;ty<th;++ty) for (unsigned tx=0;tx<tw;++tx) {
    unsigned h=tx<<shift, v=ty<<shift;
    unsigned plane=(h>>k->shift_x)+((v>>k->shift_y)<<2);
    unsigned x=h&((1u<<k->shift_x)-1), y=v&((1u<<k->shift_y)-1);
    unsigned entry=((y>>9)*k->pages_x+(x>>9))*page_width*page_width+
        ((y&511)>>shift)*page_width+((x&511)>>shift);
    unsigned address=k->planes[plane]+(entry<<k->words);
    unsigned name=YglRotationPatternWord(ram,address);
    unsigned second=k->words == 2 ? YglRotationPatternWord(ram,address+2) : 0;
    YglRotationPatternDecode(k,name,second,out+((size_t)ty*tw+tx)*4);
  }
  return 1;
}
#endif
