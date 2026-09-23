/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_DISC_IO_H
#define VITA_DISC_IO_H
#include <stdio.h>
#include "telemetry.h"
/* Stdio latency includes buffering and preemption; it is not physical-device
 * busy time. Keep codec execution outside these scopes. Preserve return values. */
static inline size_t VitaDiscRead(void *buffer, size_t size, size_t count, FILE *file) {
  VT_SCOPE(VT_DISC_IO);
  return fread(buffer, size, count, file);
}
static inline int VitaDiscSeek(FILE *file, long offset, int origin) {
  VT_SCOPE(VT_DISC_IO);
  return fseek(file, offset, origin);
}
#endif
