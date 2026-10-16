/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_GPUPROF_H
#define VITA_GPUPROF_H
#include <stdint.h>
#include <stdlib.h>
#ifdef VITA_GPUPROF
void VitaGpuProfInit(const char *request_path, const char *output_path, const char *run_id);
void VitaGpuProfStartSampler(void);
void VitaGpuProfThreadBegin(void);
int VitaGpuProfScope(const char *name);
void VitaGpuProfScopeEnd(int scope);
void VitaGpuProfFrame(uint64_t frame);
#else
static inline void VitaGpuProfInit(const char *r, const char *o, const char *id) { (void)r; (void)o; (void)id; }
static inline void VitaGpuProfStartSampler(void) {}
static inline void VitaGpuProfThreadBegin(void) {}
static inline int VitaGpuProfScope(const char *name) { (void)name; return -1; }
static inline void VitaGpuProfScopeEnd(int scope) { (void)scope; }
static inline void VitaGpuProfFrame(uint64_t frame) { (void)frame; }
#endif
#endif
