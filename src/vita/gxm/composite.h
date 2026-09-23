/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/* Interchange format shared by the reference rasterizer and native layers.
 * Two RGBA8 texels per Saturn pixel: packed color, then selection metadata.
 * Layer order: NBG3, NBG2, NBG1, NBG0/RBG1, RBG0, sprite (VDP2 table 11.1).
 * Alpha retains the reference core's six-bit ratio and calculation flag; it
 * is NOT host alpha. Never blend these layer textures with host alpha blending.
 */
typedef struct VitaGxmPixel {
   uint32_t pixel;
   uint8_t priority, linescreen, shadow_type, shadow_enabled;
} VitaGxmPixel;

typedef struct {
   unsigned width, height;
   unsigned line_increment, field;
   unsigned blend_mode, sprite_window;
   const VitaGxmPixel *layers[6];
   const VitaGxmPixel *back;
   const uint32_t *line[3];
} VitaGxmCompositeFrame;

#ifdef __cplusplus
static_assert(sizeof(VitaGxmPixel) == 8, "GPU layer interchange size");
#else
_Static_assert(sizeof(VitaGxmPixel) == 8, "GPU layer interchange size");
#endif
