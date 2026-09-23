/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "texture.h"

/* VRAM is Yabause's T1 (big-endian byte stream), bounded to 512 KiB. */
static unsigned byte(const uint8_t *ram, uint32_t address) { return ram[address & 0x7ffff]; }
static unsigned word(const uint8_t *ram, uint32_t address) {
   return (byte(ram, address) << 8) | byte(ram, address + 1);
}
static uint32_t rgb555(unsigned v) {
   return ((v & 31) << 3) | ((v & 0x3e0) << 6) | ((v & 0x7c00) << 9);
}

int VitaDecodeVdp1Texture(const uint8_t *ram, const VitaVdp1Texture *t,
                         uint32_t *out, unsigned stride) {
   if (!ram || !t || !out || !t->width || t->width > 504 || (t->width & 7) ||
       !t->height || t->height > 255 || stride < t->width || stride > 4096) return -1;
   unsigned format = (t->mode >> 3) & 7;
   if (format > 5) return -1;
   unsigned spd = t->mode & 0x40, ecd = t->mode & 0x80;
   unsigned end = format < 2 ? 15 : format < 5 ? 255 : 0x7fff;
   unsigned mask = format < 2 ? 15 : format == 2 ? 63 : format == 3 ? 127 : 255;
   for (unsigned y = 0; y < t->height; ++y) {
      unsigned end_count = 0;
      for (unsigned i = 0; i < t->width; ++i) {
         unsigned x = t->reverse_x ? t->width - 1 - i : i;
         unsigned pos = y * t->width + x, dot;
         if (format < 2) {
            dot = byte(ram, t->source + pos / 2);
            dot = pos & 1 ? dot & 15 : dot >> 4;
         } else if (format < 5) dot = byte(ram, t->source + pos);
         else dot = word(ram, t->source + pos * 2);
         uint32_t pixel = 0;
         // VDP1 6.3: end codes are tested BEFORE 64/128-color masking.
         // Two encountered in traversal order terminate the row; they need
         // not be adjacent. Vertical reversal does not change this rule.
         if (!ecd && dot == end) ++end_count;
         else if ((ecd || end_count < 2) &&
                  (spd || (format == 5 ? (dot & 0x8000) != 0 : dot != 0))) {
            // VDP1 table 6.2 prohibits MSB-clear nonzero RGB codes. Match
            // the existing renderer's transparent treatment of those codes
            // rather than assigning them a new visible interpretation.
            unsigned value;
            if (format == 1) value = word(ram, (uint32_t)t->color * 8 + dot * 2);
            else if (format == 5) value = dot;
            else value = (t->color & ~mask) | (dot & mask);
            pixel = 0xff000000u | value;
         }
         out[y * stride + x] = pixel;
      }
   }
   return 0;
}

int VitaDecodeVdp2Cell(const uint8_t *ram, const uint32_t palette[2048],
                      const VitaVdp2Cell *c, VitaVdp2Texel out[64]) {
   if (!ram || !c || !out || c->format > 4 || (c->format < 3 && !palette)) return -1;
   // VDP2 4.3: cells are 8x8. A 16x16 character comprises four such cells;
   // character ordering and flips belong to the map/geometry frontend.
   for (unsigned i = 0; i < 64; ++i) {
      unsigned dot, valid, msb;
      uint32_t color;
      if (c->format < 3) {
         if (c->format == 0) {
            dot = byte(ram, c->source + i / 2);
            dot = i & 1 ? dot & 15 : dot >> 4;
         } else if (c->format == 1) dot = byte(ram, c->source + i);
         else dot = word(ram, c->source + i * 2);
         unsigned index = c->format == 2 ? dot & 0x7ff : dot;
         valid = !c->transparency || index != 0;
         if (c->format < 2) index |= c->palette;
         color = palette[(index + c->color_offset) & 0x7ff];
         msb = color >> 31;
      } else if (c->format == 3) {
         dot = word(ram, c->source + i * 2);
         msb = dot >> 15;
         valid = !c->transparency || msb;
         color = rgb555(dot);
      } else {
         uint32_t high = word(ram, c->source + i * 4);
         dot = word(ram, c->source + i * 4 + 2);
         msb = high >> 15;
         valid = !c->transparency || msb;
         color = ((high & 255) << 16) | dot;
      }
      out[i].rgba = (color & 0xffffff) | (valid ? 0xff000000u : 0);
      out[i].dot = dot;
      out[i].color_msb = msb;
      out[i].visible = valid;
   }
   return 0;
}
