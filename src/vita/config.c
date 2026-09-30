/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA "ux0:data/yabause-vita/"

static const char *const saturn_names[VITA_SATURN_BUTTONS] = {
  "up", "right", "down", "left", "r", "c", "b", "a", "y", "z", "l", "start", "x"
};

static const char *const button_names[VITA_BUTTONS] = {
  "none", "up", "right", "down", "left", "cross", "circle", "square",
  "triangle", "l", "r", "start", "rstick_left", "rstick_right"
};

/* SCE_CTRL_* values (psp2common/ctrl.h), repeated so host tests build. */
static const unsigned button_masks[VITA_BUTTONS] = {
  0, 0x0010, 0x0020, 0x0040, 0x0080, 0x4000, 0x2000, 0x8000,
  0x1000, 0x0100, 0x0200, 0x0008, VITA_CTRL_RSTICK_LEFT, VITA_CTRL_RSTICK_RIGHT
};

static const char *const cart_names[VITA_CARTS] = {
  "none", "backup_4mbit", "ram_1mb", "ram_4mb"
};

/* smpc.h REGION_* values with their config names. */
static const struct { int region; const char *name; } regions[] = {
  {0, "auto"}, {1, "japan"}, {4, "north_america"}, {12, "europe"},
  {2, "asia_ntsc"}, {10, "asia_pal"}, {6, "korea"},
  {5, "south_america_ntsc"}, {13, "south_america_pal"}
};

const int vita_config_regions[] = {0, 1, 4, 12, 2, 10, 6, 5, 13};
const unsigned vita_config_region_count =
  sizeof(vita_config_regions) / sizeof(vita_config_regions[0]);

void VitaConfigDefaults(VitaConfig *config) {
  static const unsigned char pad[VITA_SATURN_BUTTONS] = {
    VITA_BUTTON_UP, VITA_BUTTON_RIGHT, VITA_BUTTON_DOWN, VITA_BUTTON_LEFT,
    VITA_BUTTON_RSTICK_RIGHT, VITA_BUTTON_R, VITA_BUTTON_CIRCLE, VITA_BUTTON_CROSS,
    VITA_BUTTON_TRIANGLE, VITA_BUTTON_L, VITA_BUTTON_RSTICK_LEFT, VITA_BUTTON_START,
    VITA_BUTTON_SQUARE
  };
  memset(config, 0, sizeof(*config));
  snprintf(config->bios, sizeof(config->bios), "%s", VITA_BIOS_AUTO);
  snprintf(config->games_dir, sizeof(config->games_dir), "%s", DATA "disc");
  memcpy(config->pad, pad, sizeof(pad));
  config->cartridge = VITA_CART_BACKUP_4MBIT;
  config->backup_slot = 1;
}

static int lookup(const char *const *names, int count, const char *name) {
  for (int i = 0; i < count; ++i)
    if (!strcmp(names[i], name)) return i;
  return -1;
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) ++text;
  size_t n = strlen(text);
  while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
  return text;
}

static void copy_path(char *out, size_t size, const char *value) {
  if (strlen(value) < size) memcpy(out, value, strlen(value) + 1);
}

int VitaConfigLoad(VitaConfig *config, const char *path) {
  VitaConfigDefaults(config);
  FILE *file = fopen(path, "r");
  if (!file) return 1;
  char line[640];
  while (fgets(line, sizeof(line), file)) {
    char *text = trim(line);
    if (!*text || *text == '#' || *text == ';') continue;
    char *equals = strchr(text, '=');
    if (!equals) continue;
    *equals = 0;
    char *key = trim(text), *value = trim(equals + 1);
    if (!strcmp(key, "bios")) copy_path(config->bios, sizeof(config->bios), value);
    else if (!strcmp(key, "games_dir")) copy_path(config->games_dir, sizeof(config->games_dir), value);
    else if (!strcmp(key, "last_game")) copy_path(config->last_game, sizeof(config->last_game), value);
    else if (!strcmp(key, "region")) {
      for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i)
        if (!strcmp(regions[i].name, value)) config->region = regions[i].region;
    } else if (!strcmp(key, "cartridge")) {
      const int cartridge = lookup(cart_names, VITA_CARTS, value);
      if (cartridge >= 0) config->cartridge = cartridge;
    } else if (!strcmp(key, "backup_slot")) {
      const int slot = atoi(value);
      if (slot >= 1 && slot <= VITA_BACKUP_SLOTS) config->backup_slot = slot;
    } else if (!strncmp(key, "pad_", 4)) {
      const int saturn = lookup(saturn_names, VITA_SATURN_BUTTONS, key + 4);
      const int button = lookup(button_names, VITA_BUTTONS, value);
      if (saturn >= 0 && button >= 0) config->pad[saturn] = (unsigned char)button;
    }
  }
  fclose(file);
  return 0;
}

int VitaConfigSave(const VitaConfig *config, const char *path) {
  /* Written beside the target and renamed, so a power loss keeps the old file. */
  char temp[512];
  if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return -1;
  FILE *file = fopen(temp, "w");
  if (!file) return -1;
  fprintf(file, "bios = %s\ngames_dir = %s\nlast_game = %s\nregion = %s\n"
          "cartridge = %s\nbackup_slot = %d\n",
          config->bios, config->games_dir, config->last_game,
          VitaConfigRegionName(config->region), VitaConfigCartName(config->cartridge),
          config->backup_slot);
  for (int i = 0; i < VITA_SATURN_BUTTONS; ++i)
    fprintf(file, "pad_%s = %s\n", saturn_names[i], VitaConfigButtonName(config->pad[i]));
  if (fclose(file) != 0) { remove(temp); return -1; }
  remove(path);
  return rename(temp, path) == 0 ? 0 : -1;
}

unsigned VitaConfigButtonMask(int button) {
  return button > 0 && button < VITA_BUTTONS ? button_masks[button] : 0;
}

const char *VitaConfigSaturnName(int saturn) {
  return saturn >= 0 && saturn < VITA_SATURN_BUTTONS ? saturn_names[saturn] : "";
}

const char *VitaConfigButtonName(int button) {
  return button >= 0 && button < VITA_BUTTONS ? button_names[button] : "none";
}

const char *VitaConfigCartName(int cartridge) {
  return cartridge >= 0 && cartridge < VITA_CARTS ? cart_names[cartridge] : "none";
}

void VitaConfigBackupPath(char *out, size_t size, int slot) {
  snprintf(out, size, VITA_MEMORY_DIR "/bkram%d.bin", slot);
}

void VitaConfigStatePath(char *out, size_t size, const char *game, int slot) {
  if (slot <= 1) snprintf(out, size, DATA "states/%s.yss", game);
  else snprintf(out, size, DATA "states/%s.%d.yss", game, slot);
}

const char *VitaConfigRegionName(int region) {
  for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i)
    if (regions[i].region == region) return regions[i].name;
  return "auto";
}
