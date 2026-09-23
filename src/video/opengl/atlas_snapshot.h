/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ATLAS_SNAPSHOT_H
#define YGL_ATLAS_SNAPSHOT_H
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
/* Equality only: no first-differing-byte ordering and no reads past length. */
static inline int YglAtlasBytesEqual(const void *left, const void *right, size_t n) {
  const unsigned char *a = left, *b = right;
#if defined(__ARM_NEON)
  while (n >= 64) {
    uint8x16_t x0 = veorq_u8(vld1q_u8(a), vld1q_u8(b));
    uint8x16_t x1 = veorq_u8(vld1q_u8(a+16), vld1q_u8(b+16));
    uint8x16_t x2 = veorq_u8(vld1q_u8(a+32), vld1q_u8(b+32));
    uint8x16_t x3 = veorq_u8(vld1q_u8(a+48), vld1q_u8(b+48));
    uint32x4_t bits = vreinterpretq_u32_u8(vorrq_u8(vorrq_u8(x0,x1),vorrq_u8(x2,x3)));
    uint32x2_t pair = vorr_u32(vget_low_u32(bits),vget_high_u32(bits));
    pair = vpmax_u32(pair,pair);
    if (vget_lane_u32(pair,0)) return 0;
    a+=64; b+=64; n-=64;
  }
#endif
  return !memcmp(a,b,n);
}
typedef struct {
  void *pixels;
  size_t capacity;
  unsigned width, height;
  unsigned checks, hits;
} YglAtlasSnapshot;
static inline int YglAtlasSnapshotMatches(const YglAtlasSnapshot *s,
    unsigned width, unsigned height, const void *pixels) {
  return pixels && width && height && s->pixels && s->width == width &&
    height <= s->height &&
    YglAtlasBytesEqual(s->pixels, pixels, (size_t)width * height * 4);
}
static inline void YglAtlasSnapshotStore(YglAtlasSnapshot *s,
    unsigned width, unsigned height, const void *pixels) {
  s->width = s->height = 0;
  if (!pixels || !width || !height || width > 4096 || height > 4096) return;
  size_t bytes = (size_t)width * height * 4;
  if (bytes > s->capacity) {
    void *next = realloc(s->pixels, bytes);
    if (!next) return; /* Upload remains authoritative on allocation failure. */
    s->pixels = next;
    s->capacity = bytes;
  }
  memcpy(s->pixels, pixels, bytes);
  s->width = width; s->height = height;
}
#endif
