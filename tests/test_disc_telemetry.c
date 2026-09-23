/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/disc_io.h"
#include <assert.h>
#include <string.h>
static unsigned entered, left, depth;
int VitaTelemetryMode = VT_DETAILED;
void *VitaTelemetryEnter(VitaTelemetryPhase phase) {
  assert(phase == VT_DISC_IO && depth == 0); ++depth; ++entered;
  return NULL;
}
void VitaTelemetryLeaveSample(void *sample) { (void)sample; assert(0); }
void VitaTelemetryLeave(VitaTelemetryPhase phase) {
  assert(phase == VT_DISC_IO && depth == 1); --depth; ++left;
}
int main(void) {
  FILE *file = tmpfile(); assert(file);
  assert(fwrite("abcde", 1, 5, file) == 5);
  assert(VitaDiscSeek(file, 0, SEEK_SET) == 0);
  char data[8] = {0};
  assert(VitaDiscRead(data, 2, 3, file) == 2);
  assert(!memcmp(data, "abcde", 5));
  assert(feof(file));
  assert(VitaDiscRead(data, 1, 1, file) == 0);
  assert(VitaDiscSeek(file, -2, SEEK_END) == 0);
  assert(!feof(file));
  assert(VitaDiscRead(data, 1, 2, file) == 2);
  assert(!memcmp(data, "de", 2));
  assert(entered == 5 && left == 5 && !depth);
  assert(fclose(file) == 0);
  puts("disc telemetry: seek, short reads, EOF and scope balance passed");
}
