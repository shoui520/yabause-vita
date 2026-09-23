/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/vdp2_generation.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  uint32_t lo=0,epoch=0;
  Vdp2GenerationAdvance(&lo,&epoch); assert(lo==1 && epoch==0);
  lo=UINT32_MAX; Vdp2GenerationAdvance(&lo,&epoch); assert(lo==0 && epoch==1);
  lo=UINT32_MAX; epoch=UINT32_MAX-1;
  Vdp2GenerationAdvance(&lo,&epoch); assert(lo==0 && epoch==UINT32_MAX);
  lo=UINT32_MAX; Vdp2GenerationAdvance(&lo,&epoch); assert(lo==0 && epoch==UINT32_MAX);
  Vdp2GenerationAdvance(&lo,&epoch); assert(lo==1 && epoch==UINT32_MAX);
  puts("VDP2 generation: increment, wrap and permanent exhaustion passed");
}
