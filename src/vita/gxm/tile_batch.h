/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

typedef struct { float x, y, u, v; } VitaTileVertex;
typedef struct {
   VitaTileVertex *vertices;
   uint16_t *indices;
   unsigned count, capacity;
} VitaTileBatch;

/* One decoded 8x8 cell, integer placement, point sampling. Clip destination
 * edges and UVs together; flip bits match VDP2 horizontal/vertical flags.
 * No allocation, submission, or ownership transfer. Full batches are unchanged. */
static inline int VitaTileBatchAdd(VitaTileBatch *b, int x, int y,
   unsigned sx, unsigned sy, unsigned aw, unsigned ah,
   unsigned width, unsigned height, unsigned flip)
{
   if (!b || !b->vertices || !b->indices || !b->capacity || b->capacity > 16384 ||
       b->count > b->capacity || aw < 8 || ah < 8 || aw > 4096 || ah > 4096 ||
       sx > aw - 8 || sy > ah - 8 || !width || !height || width > 704 || height > 512 || flip > 3)
      return -1;
   int64_t right = (int64_t)x + 8, bottom = (int64_t)y + 8;
   if (right <= 0 || bottom <= 0 || x >= (int)width || y >= (int)height) return 0;
   if (b->count == b->capacity) return -1;
   int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
   int r = right > width ? (int)width : (int)right;
   int bot = bottom > height ? (int)height : (int)bottom;
   float u0 = (float)(left - x), u1 = (float)(r - x);
   float v0 = (float)(top - y), v1 = (float)(bot - y);
   if (flip & 1) { u0 = 8 - u0; u1 = 8 - u1; }
   if (flip & 2) { v0 = 8 - v0; v1 = 8 - v1; }
   u0 = (sx + u0) / aw; u1 = (sx + u1) / aw;
   v0 = (sy + v0) / ah; v1 = (sy + v1) / ah;
   unsigned base = b->count * 4;
   VitaTileVertex *v = b->vertices + base;
   v[0] = (VitaTileVertex){left, top, u0, v0};
   v[1] = (VitaTileVertex){r, top, u1, v0};
   v[2] = (VitaTileVertex){left, bot, u0, v1};
   v[3] = (VitaTileVertex){r, bot, u1, v1};
   uint16_t *indices = b->indices + b->count * 6;
   const unsigned offsets[6] = {0, 1, 2, 2, 1, 3};
   for (unsigned i = 0; i < 6; ++i) indices[i] = base + offsets[i];
   ++b->count;
   return 1;
}
