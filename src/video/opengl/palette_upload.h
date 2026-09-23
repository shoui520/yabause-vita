/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_PALETTE_UPLOAD_H
#define YGL_PALETTE_UPLOAD_H
#include <stdint.h>
#include <string.h>
typedef struct {
  uint32_t pixels[2048];
  unsigned valid;
} YglPaletteUpload;
static inline int YglPaletteUploadEqual(const YglPaletteUpload *s,
    const uint32_t *pixels,unsigned first,unsigned count) {
  return s->valid && first<=2048 && count<=2048-first &&
    !memcmp(s->pixels+first,pixels+first,count*sizeof(uint32_t));
}
static inline void YglPaletteUploadRemember(YglPaletteUpload *s,
    const uint32_t *pixels,unsigned first,unsigned count) {
  if(first>2048 || count>2048-first) return;
  memcpy(s->pixels+first,pixels+first,count*sizeof(uint32_t));
  if(first==0 && count==2048) s->valid=1;
}
#endif
