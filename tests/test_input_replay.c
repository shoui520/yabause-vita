/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include "../src/vita/input_replay.h"
static int parse(const char *text, VitaInputReplay *replay) {
  FILE *file = tmpfile(); assert(file);
  fputs(text, file); rewind(file);
  int rc = VitaInputReplayRead(file, replay);
  fclose(file); return rc;
}
int main(void) {
  VitaInputReplay replay;
  assert(parse("10 800\n20 80\n30 0\n", &replay) == 0);
  unsigned mask = 99;
  assert(VitaInputReplayMask(&replay, 0, &mask) && mask == 0);
  assert(VitaInputReplayMask(&replay, 10, &mask) && mask == 0x800);
  assert(VitaInputReplayMask(&replay, 19, &mask) && mask == 0x800);
  assert(VitaInputReplayMask(&replay, 25, &mask) && mask == 0x80);
  assert(VitaInputReplayMask(&replay, 30, &mask) && mask == 0);
  assert(!VitaInputReplayMask(&replay, 31, &mask));
  const char *invalid[] = {"", "1 800\n", "1 0\n1 0\n", "2 0\n1 0\n",
    "1000001 0\n", "-1 0\n", "1 -0\n", "1 2000\n", "1 0", "1 0x\n",
    "1 0 extra\n", "4294967296 0\n", "1 100000000\n",
    "9999999999999999999999999999999999 0\n"};
  for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
    assert(parse(invalid[i], &replay) == -1);
    assert(replay.count == 0 && !VitaInputReplayMask(&replay, 0, &mask));
  }
  FILE *file = tmpfile(); assert(file);
  for (unsigned i = 0; i < 257; ++i) fprintf(file, "%u 0\n", i);
  rewind(file); assert(VitaInputReplayRead(file, &replay) == -1); fclose(file);
  puts("input replay: frame boundaries, release, strict parsing and bounded events passed");
}
