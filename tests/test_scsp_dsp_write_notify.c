/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/scspdsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static u16 ram[0x40000];
static unsigned calls;
static u32 notified_address, notified_size;
static u16 expected_value;
ScspDsp scsp_dsp;

void M68KWriteNotify(u32 address, u32 size) {
  ++calls;
  notified_address = address;
  notified_size = size;
  // An invalidation callback must observe the completed write, not old data.
  assert(size == 2 && address < sizeof(ram));
  assert(ram[address / 2] == expected_value);
}

int main(void) {
  const u32 addresses[] = {0, 1, 127, 128, 0x1ffff, 0x3ffff, 0x40000};
  for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
    memset(&scsp_dsp, 0, sizeof(scsp_dsp));
    scsp_dsp.write_pending = 1;
    scsp_dsp.io_addr = addresses[i];
    scsp_dsp.write_value = expected_value = (u16)(0x8123 + i);
    calls = 0;
    ScspDspExec(&scsp_dsp, 0, (u8 *)ram);
    assert(scsp_dsp.write_pending == 0);
    if (addresses[i] & 0x40000) assert(calls == 0);
    else {
      assert(calls == 1);
      assert(notified_address == addresses[i] * 2 && notified_size == 2);
    }
    ScspDspExec(&scsp_dsp, 0, (u8 *)ram);
    assert(calls == ((addresses[i] & 0x40000) ? 0u : 1u));
  }
  puts("SCSP DSP: completed writes notify once; rejected and absent writes do not notify");
}
