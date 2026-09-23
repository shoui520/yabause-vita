/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* VDP1 atlas texels retain the framebuffer word, not a CRAM-resolved RGB color.
 * R/G contain low/high bytes; A=255 means draw. VDP2 must subsequently interpret
 * the word using SPCTL, sprite priority and calculation-ratio registers. */
typedef struct {
   uint32_t source;
   uint16_t color, mode;
   unsigned width, height, reverse_x;
} VitaVdp1Texture;
int VitaDecodeVdp1Texture(const uint8_t *ram, const VitaVdp1Texture *texture,
                         uint32_t *out, unsigned stride);

/* Separate metadata is required: CRAM's MSB controls special calculation and
 * the source dot controls per-dot priority. Alpha cannot encode all three. */
typedef struct {
   uint32_t rgba;
   uint16_t dot;
   uint8_t color_msb, visible;
} VitaVdp2Texel;
typedef struct {
   uint32_t source;
   unsigned format, palette, color_offset, transparency;
} VitaVdp2Cell;
int VitaDecodeVdp2Cell(const uint8_t *ram, const uint32_t palette[2048],
                      const VitaVdp2Cell *cell, VitaVdp2Texel out[64]);
