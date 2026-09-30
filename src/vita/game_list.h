/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_GAME_LIST_H
#define VITA_GAME_LIST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char path[512];
  char title[160];   /* file name without its extension */
} VitaGameEntry;

typedef struct {
  VitaGameEntry *entries;
  unsigned count, capacity;
} VitaGameList;

/* Disc images in root and in its direct subdirectories, sorted by title.
 * A folder with a .cue/.ccd/.mds lists only those (their track files are
 * not separate games); .chd and .iso are listed otherwise. */
int VitaGameListScan(VitaGameList *list, const char *root);
/* 512 KiB files (Saturn BIOS images) directly in each directory. */
int VitaBiosListScan(VitaGameList *list, const char *const *dirs, unsigned dir_count);
/* The image VITA_BIOS_AUTO picks from a scanned list: sega_101.bin before
 * the others (the first by name). -1 for an empty list. */
int VitaBiosPreferred(const VitaGameList *list);
/* The BIOS to boot: the configured file when it exists, else (and for
 * VITA_BIOS_AUTO) the preferred image in dir. -1 when there is none. */
int VitaBiosResolve(char *out, size_t size, const char *configured, const char *dir);
void VitaGameListFree(VitaGameList *list);

#ifdef __cplusplus
}
#endif

#endif
