/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/* CPU reference renderer only. Hardware rendering will present GXM surfaces. */
typedef struct {
  unsigned width, height;
  uint16_t x[480];
  uint32_t row[272];
} VitaPresentMap;

int VitaPresentMapInit(VitaPresentMap *map, unsigned width, unsigned height);
void VitaPresentCopy(const VitaPresentMap *map, uint32_t *destination,
                     const uint32_t *source);
