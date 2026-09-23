/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/video/opengl/rotation_coefficient.h"

int main(void)
{
    /* Independent manual oracle: enumerate signed values, encode their
     * two's-complement payload, and independently toggle the control bit. */
    for (int value = -16384; value <= 16383; ++value) {
        uint16_t encoding = (uint16_t)((uint32_t)value & 0x7fff);
        float expected = (float)((double)value / 1024.0);
        assert(Vdp2RotationCoefficient1Word(encoding) == expected);
        assert(Vdp2RotationCoefficient1Word(encoding | 0x8000) == expected);
    }
    assert(Vdp2RotationCoefficient1Word(0x4000) == -16.0f);
    assert(Vdp2RotationCoefficient1Word(0x7c00) == -1.0f);
    assert(Vdp2RotationCoefficient1Word(0x7fff) == -1.0f / 1024.0f);
    assert(Vdp2RotationCoefficient1Word(0x0400) == 1.0f);
    puts("VDP2 one-word coefficients: all 65536 encodings passed");
    return 0;
}
