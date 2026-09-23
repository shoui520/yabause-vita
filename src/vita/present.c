/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "present.h"

int VitaPresentMapInit(VitaPresentMap *map, unsigned width, unsigned height) {
  /* VIDSoft allocates 704 * 512 source pixels. Bound integer products too. */
  if (!width || width > 704 || !height || height > 512) return -1;
  if (map->width == width && map->height == height) return 0;
  for (unsigned x = 0; x < 480; ++x) map->x[x] = x * width / 480;
  for (unsigned y = 0; y < 272; ++y) map->row[y] = (y * height / 272) * width;
  map->width = width;
  map->height = height;
  return 0;
}

void VitaPresentCopy(const VitaPresentMap *map, uint32_t *destination,
                     const uint32_t *source) {
  for (unsigned y = 0; y < 272; ++y) {
    const uint32_t *row = source + map->row[y];
    uint32_t *out = destination + y * 512;
    for (unsigned x = 0; x < 480; ++x)
      out[x] = row[map->x[x]] | 0xff000000u;
  }
}
