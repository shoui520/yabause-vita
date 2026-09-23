/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ROTATION_VARIANT_H
#define YGL_ROTATION_VARIANT_H
/* Matches CMake's format-major, seven-layout binary table. Invalid metadata
 * uses the generic shader; bitmap and two-word names ignore unused fields. */
static inline int YglRotationVariant(int format, int bitmap, int cells,
                                      int words, int aux) {
  int group;
  switch (format) {
    case 0: group = 0; break;
    case 1: group = 1; break;
    case 3: group = 2; break;
    case 4: group = 3; break;
    default: return -1;
  }
  if (bitmap == 1) return group * 7;
  if (bitmap != 0 || cells < 1 || cells > 2 || words < 1 || words > 2)
    return -1;
  if (words == 1 && (aux < 0 || aux > 1)) return -1;
  return group * 7 + 1 + (cells - 1) * 3 + (words == 2 ? 2 : aux);
}
#endif
