/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_PACKED_TEXTURE_H
#define YGL_PACKED_TEXTURE_H
/* Shared C/Cg arithmetic. Nearest-sampled UNORM8 data is an encoded byte,
 * not a continuous color. Sony tex2D returns half4; truncation after conversion
 * to float loses bits for values rounded below n/255. Recover each byte before
 * combining palette indices, testing flags, or reconstructing priorities.
 * Floor explicitly: the physical native-shader test with psp2cgc 3.570.021
 * observed odd bytes rounding upward with the direct (int)(n + 0.5) cast.
 * Casting an already integral floor result is independent of tie rounding.
 */
int YglUnorm8(float value) { return (int)floor(value * 255.0f + 0.5f); }
int YglUnorm16BE(float high, float low) {
  float hi = floor(high * 255.0f + 0.5f);
  float lo = floor(low * 255.0f + 0.5f);
  return (int)(hi * 256.0f + lo);
}
int YglPaletteIndex(float low, float high) {
  return YglUnorm8(low) | (YglUnorm8(high) << 8);
}
#endif
