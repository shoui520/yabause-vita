/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/cpu_affinity.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
int main(void) {
  for (int request = 0; request <= 7; ++request) {
    assert(VitaAffinityForRole(YAB_THREAD_SCSP, request) == 2);
    assert(VitaAffinityForRole(YAB_THREAD_OPENAL, request) == 2);
    assert(VitaAffinityForRole(YAB_THREAD_VDP, request) == 4);
    for (int role = YAB_THREAD_VIDSOFT_LAYER_NBG3; role < YAB_NUM_THREADS; ++role)
      assert(VitaAffinityForRole(role, request) == 4);
    assert(VitaAffinityForRole(-1, request) == request);
    assert(VitaAffinityForRole(YAB_THREAD_NETLINKCLIENT, request) == request);
  }
  for (int mask = 0; mask <= 7; ++mask) {
    assert(VitaAffinityEncode(mask) == mask * 0x10000);
    assert(VitaAffinityDecode(VitaAffinityEncode(mask)) == mask);
  }
  assert(VitaAffinityEncode(3) == 0x30000);
  assert(VitaAffinityEncode(-1) == -1);
  assert(VitaAffinityEncode(INT_MIN) == -1);
  assert(VitaAffinityEncode(INT_MAX) == -1);
  assert(VitaAffinityEncode(8) == -1);
  assert(VitaAffinityEncode(0x10000) == -1);
  assert(VitaAffinityDecode(-123) == -123);
  assert(VitaAffinityDecode(0x80000) == -1);
  assert(VitaAffinityDecode(1) == -1);
  assert(VitaAffinityDecode(0x70001) == -1);
  puts("Vita affinity: all logical masks, round trips, errors and invalid bits passed");
}
