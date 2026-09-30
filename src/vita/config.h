/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_CONFIG_H
#define VITA_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User settings, kept as "key = value" lines in DATA "config.ini". The file
 * is plain C so the launcher, the in-game menu and host tests share it. */

/* Saturn pad inputs, in the order of the frontend's PerSetKey table. */
enum {
  VITA_SATURN_UP, VITA_SATURN_RIGHT, VITA_SATURN_DOWN, VITA_SATURN_LEFT,
  VITA_SATURN_R, VITA_SATURN_C, VITA_SATURN_B, VITA_SATURN_A,
  VITA_SATURN_Y, VITA_SATURN_Z, VITA_SATURN_L, VITA_SATURN_START,
  VITA_SATURN_X, VITA_SATURN_BUTTONS
};

/* Assignable Vita buttons. SELECT is reserved for the in-game menu. */
enum {
  VITA_BUTTON_NONE, VITA_BUTTON_UP, VITA_BUTTON_RIGHT, VITA_BUTTON_DOWN,
  VITA_BUTTON_LEFT, VITA_BUTTON_CROSS, VITA_BUTTON_CIRCLE, VITA_BUTTON_SQUARE,
  VITA_BUTTON_TRIANGLE, VITA_BUTTON_L, VITA_BUTTON_R, VITA_BUTTON_START,
  VITA_BUTTON_RSTICK_LEFT, VITA_BUTTON_RSTICK_RIGHT,
  VITA_BUTTONS
};

/* Masks of the right stick directions, in bits no SCE_CTRL_* button uses:
 * the input code sets them from the stick's position. */
#define VITA_CTRL_RSTICK_LEFT  0x01000000u
#define VITA_CTRL_RSTICK_RIGHT 0x02000000u

/* The BIOS folder, and the bios setting that picks an image from it. */
#define VITA_BIOS_DIR "ux0:data/yabause-vita/bios"
#define VITA_BIOS_AUTO "auto"

/* The cartridge slot, in the Cartridges setting's order. */
enum {
  VITA_CART_NONE, VITA_CART_BACKUP_4MBIT, VITA_CART_RAM_1MB, VITA_CART_RAM_4MB,
  VITA_CARTS
};
/* Files of the 4 Mbit Backup Memory cartridge, 1..VITA_BACKUP_SLOTS. */
#define VITA_BACKUP_SLOTS 10
/* Backup memory files: the internal 本体RAM and the cartridge slots. */
#define VITA_MEMORY_DIR "ux0:data/yabause-vita/memory"
#define VITA_INTERNAL_BACKUP VITA_MEMORY_DIR "/internal.bin"

typedef struct {
  char bios[256];        /* 512 KiB Saturn BIOS image, or VITA_BIOS_AUTO */
  char games_dir[256];   /* scanned by the game list */
  char last_game[512];   /* selected in the game list on the next launch */
  int region;            /* REGION_* (0 = from the disc) */
  unsigned char pad[VITA_SATURN_BUTTONS]; /* VITA_BUTTON_* per Saturn input */
  int cartridge;         /* VITA_CART_* */
  int backup_slot;       /* the 4 Mbit Backup Memory's file, 1..VITA_BACKUP_SLOTS */
} VitaConfig;

void VitaConfigDefaults(VitaConfig *config);
/* Missing file: defaults, returns 1. Unknown keys and bad values are skipped. */
int VitaConfigLoad(VitaConfig *config, const char *path);
int VitaConfigSave(const VitaConfig *config, const char *path);

/* SCE_CTRL_* bit of a VITA_BUTTON_* value (0 for none). */
unsigned VitaConfigButtonMask(int button);
const char *VitaConfigSaturnName(int saturn);
const char *VitaConfigButtonName(int button);
const char *VitaConfigRegionName(int region);
const char *VitaConfigCartName(int cartridge);
/* The file of a 4 Mbit Backup Memory slot. */
void VitaConfigBackupPath(char *out, size_t size, int slot);

/* Save state slots per game, 1..VITA_STATE_SLOTS. Slot 1 is the file the
 * single-slot menu used. */
#define VITA_STATE_SLOTS 10
void VitaConfigStatePath(char *out, size_t size, const char *game, int slot);

/* Supported regions in menu order (REGION_* values). */
extern const int vita_config_regions[];
extern const unsigned vita_config_region_count;

#ifdef __cplusplus
}
#endif

#endif
