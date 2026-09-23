/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include "code_arena_layout.h"
unsigned char *VitaSh2CodeArena();
// Requires the same allocation/publication owner as the SH-2 API. This does
// not grant permission for simultaneous cross-thread VM-domain publication.
unsigned char *VitaM68kCodeArena(); // nullptr when no reserve is configured
class VitaCodeWrite {
  void *address;
  std::size_t length;
protected:
  VitaCodeWrite(void *address, std::size_t length, vitacode::Region region);
public:
  ~VitaCodeWrite();
  VitaCodeWrite(const VitaCodeWrite &) = delete;
  VitaCodeWrite &operator=(const VitaCodeWrite &) = delete;
};
class VitaSh2CodeWrite : public VitaCodeWrite {
public:
  VitaSh2CodeWrite(void *p, std::size_t n) : VitaCodeWrite(p, n, vitacode::Region::Sh2) {}
};
class VitaM68kCodeWrite : public VitaCodeWrite {
public:
  VitaM68kCodeWrite(void *p, std::size_t n) : VitaCodeWrite(p, n, vitacode::Region::M68k) {}
};
