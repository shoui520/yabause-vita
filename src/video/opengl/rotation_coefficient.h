/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YABAUSE_ROTATION_COEFFICIENT_H
#define YABAUSE_ROTATION_COEFFICIENT_H

#include <stdint.h>

/* VDP2 User's Manual, section 6.4, figure 6.7 (p.165): modes 0-2
 * contain a signed 15-bit coefficient with ten fractional bits. Bit 15
 * controls transparency/parameter selection and is NOT the sign bit.
 * Subtraction keeps sign extension defined without signed shifts or a
 * narrowing unsigned-to-signed conversion. All results are exact floats.
 */
static inline float Vdp2RotationCoefficient1Word(uint16_t data)
{
    const int magnitude = data & 0x3fff;
    const int signed_value = magnitude - (int)(data & 0x4000);
    return (float)signed_value * (1.0f / 1024.0f);
}

#endif
