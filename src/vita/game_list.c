/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "game_list.h"
#include "config.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { KIND_NONE, KIND_SHEET, KIND_IMAGE };

static int kind_of(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return KIND_NONE;
  if (!strcasecmp(dot, ".cue") || !strcasecmp(dot, ".ccd") || !strcasecmp(dot, ".mds"))
    return KIND_SHEET;
  if (!strcasecmp(dot, ".chd") || !strcasecmp(dot, ".iso")) return KIND_IMAGE;
  return KIND_NONE;
}

static int add(VitaGameList *list, const char *dir, const char *name) {
  if (list->count == list->capacity) {
    const unsigned capacity = list->capacity ? list->capacity * 2 : 32;
    VitaGameEntry *entries = realloc(list->entries, capacity * sizeof(*entries));
    if (!entries) return -1;
    list->entries = entries;
    list->capacity = capacity;
  }
  VitaGameEntry *entry = &list->entries[list->count];
  if (snprintf(entry->path, sizeof(entry->path), "%s/%s", dir, name) >= (int)sizeof(entry->path))
    return 0;   /* the core's path buffers are not larger either */
  const char *dot = strrchr(name, '.');
  const int length = dot ? (int)(dot - name) : (int)strlen(name);
  snprintf(entry->title, sizeof(entry->title), "%.*s", length, name);
  ++list->count;
  return 0;
}

static int is_directory(const char *path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* Adds the images of one directory; subdirectories are returned in *subdirs
 * (a NUL-separated list) when requested. */
static int scan_dir(VitaGameList *list, const char *dir, char **subdirs, size_t *subdirs_size) {
  DIR *handle = opendir(dir);
  if (!handle) return 0;
  int sheets = 0;
  struct dirent *entry;
  while ((entry = readdir(handle)))
    if (kind_of(entry->d_name) == KIND_SHEET) sheets = 1;
  rewinddir(handle);
  while ((entry = readdir(handle))) {
    const char *name = entry->d_name;
    if (name[0] == '.') continue;
    const int kind = kind_of(name);
    if (kind == KIND_SHEET || (kind == KIND_IMAGE && !sheets)) {
      if (add(list, dir, name) != 0) { closedir(handle); return -1; }
      continue;
    }
    if (!subdirs || kind != KIND_NONE) continue;
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path) || !is_directory(path))
      continue;
    const size_t length = strlen(name) + 1;
    char *grown = realloc(*subdirs, *subdirs_size + length);
    if (!grown) { closedir(handle); return -1; }
    memcpy(grown + *subdirs_size, name, length);
    *subdirs = grown;
    *subdirs_size += length;
  }
  closedir(handle);
  return 0;
}

static int by_title(const void *a, const void *b) {
  const VitaGameEntry *x = a, *y = b;
  const int order = strcasecmp(x->title, y->title);
  return order ? order : strcmp(x->path, y->path);
}

int VitaGameListScan(VitaGameList *list, const char *root) {
  list->count = 0;
  char *subdirs = NULL;
  size_t subdirs_size = 0;
  int result = scan_dir(list, root, &subdirs, &subdirs_size);
  for (size_t offset = 0; result == 0 && offset < subdirs_size; offset += strlen(subdirs + offset) + 1) {
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", root, subdirs + offset) < (int)sizeof(path))
      result = scan_dir(list, path, NULL, NULL);
  }
  free(subdirs);
  if (list->count) qsort(list->entries, list->count, sizeof(*list->entries), by_title);
  return result;
}

int VitaBiosListScan(VitaGameList *list, const char *const *dirs, unsigned dir_count) {
  list->count = 0;
  for (unsigned i = 0; i < dir_count; ++i) {
    DIR *handle = opendir(dirs[i]);
    if (!handle) continue;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
      char path[512];
      struct stat info;
      if (entry->d_name[0] == '.' ||
          snprintf(path, sizeof(path), "%s/%s", dirs[i], entry->d_name) >= (int)sizeof(path) ||
          stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != 512 * 1024)
        continue;
      if (add(list, dirs[i], entry->d_name) != 0) { closedir(handle); return -1; }
    }
    closedir(handle);
  }
  if (list->count) qsort(list->entries, list->count, sizeof(*list->entries), by_title);
  return 0;
}

int VitaBiosPreferred(const VitaGameList *list) {
  for (unsigned i = 0; i < list->count; ++i)
    if (!strcasecmp(list->entries[i].title, "sega_101")) return (int)i;
  return list->count ? 0 : -1;
}

int VitaBiosResolve(char *out, size_t size, const char *configured, const char *dir) {
  struct stat info;
  if (strcmp(configured, VITA_BIOS_AUTO) && stat(configured, &info) == 0 && S_ISREG(info.st_mode)) {
    snprintf(out, size, "%s", configured);
    return 0;
  }
  VitaGameList list = {0, 0, 0};
  const int pick = VitaBiosListScan(&list, &dir, 1) == 0 ? VitaBiosPreferred(&list) : -1;
  if (pick >= 0) snprintf(out, size, "%s", list.entries[pick].path);
  VitaGameListFree(&list);
  return pick >= 0 ? 0 : -1;
}

void VitaGameListFree(VitaGameList *list) {
  free(list->entries);
  list->entries = NULL;
  list->count = list->capacity = 0;
}
