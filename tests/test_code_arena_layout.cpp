/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/code_arena_layout.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
int main() {
  using L = vitacode::Layout;
  using R = vitacode::Region;
  static_assert(L::Sh2 + L::M68k == L::Total);
  assert(L::Contains(R::Sh2, 0, L::Sh2));
  assert(!L::Contains(R::Sh2, L::Sh2, 1));
  assert(!L::Contains(R::Sh2, L::Sh2 - 1, 2));
  assert(!L::Contains(R::Sh2, 0, 0));
  assert(!L::Contains(R::Sh2, 0, std::numeric_limits<size_t>::max()));
  assert(!L::Contains(R::M68k, std::numeric_limits<size_t>::max(), 1));
  assert(!L::Contains(R::M68k, L::Sh2 - 1, 2));
  assert(!L::Contains(R::M68k, L::Total, 1));
  if (L::M68k) {
    assert(L::Contains(R::M68k, L::Sh2, L::M68k));
    assert(L::Contains(R::M68k, L::Total - 1, 1));
    assert(!L::Contains(R::M68k, L::Sh2, L::M68k + 1));
  } else assert(!L::Contains(R::M68k, L::Sh2, 1));
  std::vector<unsigned> visits(L::Sh2Blocks);
  unsigned next = 0;
  for (unsigned i = 0; i < L::Sh2Blocks * 3; ++i) {
    assert(next < L::Sh2Blocks);
    ++visits[next];
    assert(L::Contains(R::Sh2, next * L::Block, L::Block));
    next = L::NextSh2Block(next);
  }
  assert(next == 0);
  for (unsigned count : visits) assert(count == 3);
  printf("Code arena: SH2=%zu M68K=%zu; partition bounds and block recycling passed\n", L::Sh2, L::M68k);
}
