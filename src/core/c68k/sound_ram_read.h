/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C68K_SOUND_RAM_READ_H
#define C68K_SOUND_RAM_READ_H
#include "c68k.h"
static inline u32 C68k_ReadByte(c68k_struc *cpu, u32 address) {
#ifdef VITA_C68K_RAM_READS
    if (cpu->DirectReadRam && address < 0x80000u)
        return cpu->DirectReadRam[address ^ 1u];
#endif
    return cpu->Read_Byte(address);
}
static inline u32 C68k_ReadWord(c68k_struc *cpu, u32 address) {
#ifdef VITA_C68K_RAM_READS
    if (cpu->DirectReadRam && address < 0x80000u && !(address & 1u))
        return *(const u16 *)(cpu->DirectReadRam + address);
#endif
    return cpu->Read_Word(address);
}
#endif
