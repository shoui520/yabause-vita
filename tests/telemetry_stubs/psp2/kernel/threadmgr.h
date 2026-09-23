#pragma once
#include <stdint.h>
typedef struct {
  unsigned size, status, waitType;
  int currentPriority;
  int waitId;
  uint64_t runClocks;
} SceKernelThreadInfo;
int sceKernelGetThreadId(void);
int sceKernelGetThreadInfo(int tid, SceKernelThreadInfo *info);
