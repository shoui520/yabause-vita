/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YGL_ATLAS_SNAPSHOT_TEST_H
#define YGL_ATLAS_SNAPSHOT_TEST_H
#include "atlas_snapshot.h"
/* Shared host/device oracle; device uses the actual NEON implementation. */
static unsigned YglAtlasEqualityTest(unsigned *cases) {
  unsigned char a[520], b[520];
  const unsigned sizes[]={0,1,3,4,15,16,31,32,63,64,65,127,128,129,255,256,257,511,512,513};
  unsigned errors=0; *cases=0;
  for(unsigned offset=0;offset<4;++offset)
    for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);++k) {
      unsigned n=sizes[k];
      for(unsigned i=0;i<n;++i) a[offset+i]=b[3-offset+i]=(unsigned char)(i*79+offset);
      errors+=!YglAtlasBytesEqual(a+offset,b+3-offset,n); ++*cases;
      for(unsigned i=0;i<n;++i) {
        b[3-offset+i]^=0x80;
        errors+=YglAtlasBytesEqual(a+offset,b+3-offset,n); ++*cases;
        b[3-offset+i]^=0x80;
      }
    }
  return errors;
}
#endif
