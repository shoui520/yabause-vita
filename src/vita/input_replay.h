/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_INPUT_REPLAY_H
#define VITA_INPUT_REPLAY_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
enum { VITA_REPLAY_EVENTS = 4096 };
typedef struct { unsigned frame, mask; } VitaInputEvent;
typedef struct {
  VitaInputEvent events[VITA_REPLAY_EVENTS];
  unsigned count, next, held;
} VitaInputReplay;
/* Request body after the run ID. Reject the whole script on malformed input;
 * masks use the frontend's Saturn key order, never Vita system buttons. */
static int VitaInputReplayRead(FILE *file, VitaInputReplay *replay) {
  char line[96];
  memset(replay, 0, sizeof(*replay));
  while (fgets(line, sizeof(line), file)) {
    char *end, *text = line;
    if (!isdigit((unsigned char)*text)) goto invalid;
    errno = 0;
    unsigned long frame = strtoul(text, &end, 10);
    if (errno || !isspace((unsigned char)*end)) goto invalid;
    text = end;
    while (*text == ' ' || *text == '\t') ++text;
    if (!isxdigit((unsigned char)*text)) goto invalid;
    errno = 0;
    unsigned long mask = strtoul(text, &end, 16);
    while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
    if (errno || *end != '\n' || end[1] ||
        frame > 1000000 || mask > 0x1fff || replay->count == VITA_REPLAY_EVENTS ||
        (replay->count && frame <= replay->events[replay->count-1].frame)) goto invalid;
    replay->events[replay->count++] = (VitaInputEvent){(unsigned)frame, (unsigned)mask};
  }
  if (ferror(file) || !replay->count || replay->events[replay->count-1].mask) goto invalid;
  return 0;
invalid:
  memset(replay, 0, sizeof(*replay));
  return -1;
}
static int VitaInputReplayMask(VitaInputReplay *replay, unsigned frame, unsigned *mask) {
  if (!replay->count || frame > replay->events[replay->count-1].frame) return 0;
  while (replay->next < replay->count && replay->events[replay->next].frame <= frame)
    replay->held = replay->events[replay->next++].mask;
  *mask = replay->held;
  return 1;
}
#endif
