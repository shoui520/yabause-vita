/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/vita/config.h"
#include "../src/vita/game_list.h"

static char dir[256];

static void write_file(const char *name, const char *text, long size) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *file = fopen(path, "wb");
  assert(file);
  if (text) fputs(text, file);
  if (size > 0) { fseek(file, size - 1, SEEK_SET); fputc(0, file); }
  fclose(file);
}

static void test_config(void) {
  char path[512];
  snprintf(path, sizeof(path), "%s/config.ini", dir);
  VitaConfig config;
  assert(VitaConfigLoad(&config, path) == 1);   /* missing: defaults */
  assert(!strcmp(config.bios, VITA_BIOS_AUTO));
  assert(config.region == 0);
  assert(config.pad[VITA_SATURN_A] == VITA_BUTTON_CROSS);
  assert(config.pad[VITA_SATURN_L] == VITA_BUTTON_RSTICK_LEFT);
  assert(config.cartridge == VITA_CART_BACKUP_4MBIT && config.backup_slot == 1);
  assert(config.pad[VITA_SATURN_R] == VITA_BUTTON_RSTICK_RIGHT);
  assert(VitaConfigButtonMask(VITA_BUTTON_RSTICK_RIGHT) == VITA_CTRL_RSTICK_RIGHT);
  assert(!strcmp(VitaConfigButtonName(VITA_BUTTON_RSTICK_LEFT), "rstick_left"));
  assert(VitaConfigButtonMask(config.pad[VITA_SATURN_Z]) == 0x0100);
  assert(VitaConfigButtonMask(VITA_BUTTON_NONE) == 0);

  write_file("config.ini",
    "# comment\n  bios =  ux0:bios/sega.bin  \nregion = europe\n"
    "pad_l = l\npad_z = none\npad_a = bogus\npad_q = cross\nnoise\n"
    "last_game = ux0:data/yabause-vita/disc/A B/A B.cue\n"
    "cartridge = ram_4mb\nbackup_slot = 11\n", 0);
  assert(VitaConfigLoad(&config, path) == 0);
  assert(!strcmp(config.bios, "ux0:bios/sega.bin"));
  assert(config.region == 12);
  assert(config.pad[VITA_SATURN_L] == VITA_BUTTON_L);
  assert(config.pad[VITA_SATURN_Z] == VITA_BUTTON_NONE);
  assert(config.pad[VITA_SATURN_A] == VITA_BUTTON_CROSS);   /* bad value kept */
  assert(!strcmp(config.last_game, "ux0:data/yabause-vita/disc/A B/A B.cue"));
  assert(config.cartridge == VITA_CART_RAM_4MB);
  assert(config.backup_slot == 1);   /* out of range: kept */

  VitaConfig saved = config;
  saved.region = 4;
  saved.pad[VITA_SATURN_START] = VITA_BUTTON_TRIANGLE;
  saved.cartridge = VITA_CART_NONE;
  saved.backup_slot = 10;
  assert(VitaConfigSave(&saved, path) == 0);
  assert(VitaConfigLoad(&config, path) == 0);
  assert(!memcmp(&config, &saved, sizeof(config)));
  assert(!strcmp(VitaConfigRegionName(4), "north_america"));
  assert(!strcmp(VitaConfigRegionName(99), "auto"));
  char backup[256];
  VitaConfigBackupPath(backup, sizeof(backup), 3);
  assert(!strcmp(backup, "ux0:data/yabause-vita/memory/bkram3.bin"));
}

static void test_games(void) {
  char sub[512], root[512];
  snprintf(root, sizeof(root), "%s/disc", dir);
  mkdir(root, 0700);
  snprintf(sub, sizeof(sub), "%s/disc/Zeta (Japan)", dir);
  mkdir(sub, 0700);
  write_file("disc/Zeta (Japan)/Zeta (Japan).cue", "FILE", 0);
  write_file("disc/Zeta (Japan)/Zeta (Japan) (Track 1).bin", "x", 0);
  write_file("disc/Zeta (Japan)/Zeta (Japan).iso", "x", 0);   /* covered by the sheet */
  write_file("disc/alpha.chd", "x", 0);
  write_file("disc/beta.iso", "x", 0);
  write_file("disc/readme.txt", "x", 0);
  VitaGameList list = {0};
  assert(VitaGameListScan(&list, root) == 0);
  assert(list.count == 3);
  assert(!strcmp(list.entries[0].title, "alpha"));
  assert(!strcmp(list.entries[1].title, "beta"));
  assert(!strcmp(list.entries[2].title, "Zeta (Japan)"));
  assert(strstr(list.entries[2].path, "/disc/Zeta (Japan)/Zeta (Japan).cue"));

  write_file("bios.bin", NULL, 512 * 1024);
  write_file("small.bin", NULL, 1024);
  const char *dirs[] = {dir};
  assert(VitaBiosListScan(&list, dirs, 1) == 0);
  assert(list.count == 1 && !strcmp(list.entries[0].title, "bios"));
  assert(VitaBiosPreferred(&list) == 0);

  /* sega_101.bin is picked over the MPR images, which sort before it. */
  char bioses[512], picked[512], expected[600];
  snprintf(bioses, sizeof(bioses), "%s/bioses", dir);
  mkdir(bioses, 0700);
  write_file("bioses/mpr-17933.bin", NULL, 512 * 1024);
  write_file("bioses/sega_101.bin", NULL, 512 * 1024);
  snprintf(expected, sizeof(expected), "%s/sega_101.bin", bioses);
  assert(VitaBiosResolve(picked, sizeof(picked), VITA_BIOS_AUTO, bioses) == 0);
  assert(!strcmp(picked, expected));
  assert(VitaBiosResolve(picked, sizeof(picked), "/nonexistent/bios.bin", bioses) == 0);
  assert(!strcmp(picked, expected));
  snprintf(expected, sizeof(expected), "%s/mpr-17933.bin", bioses);
  assert(VitaBiosResolve(picked, sizeof(picked), expected, bioses) == 0);
  assert(!strcmp(picked, expected));
  assert(VitaBiosResolve(picked, sizeof(picked), VITA_BIOS_AUTO, "/nonexistent") == -1);
  VitaGameListFree(&list);
  VitaGameList missing = {0};
  assert(VitaGameListScan(&missing, "/nonexistent/yabause") == 0 && missing.count == 0);
  VitaGameListFree(&missing);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  snprintf(dir, sizeof(dir), "%s", argv[1]);
  test_config();
  test_games();
  char state[256];
  VitaConfigStatePath(state, sizeof(state), "Daytona USA (Japan)", 1);
  assert(!strcmp(state, "ux0:data/yabause-vita/states/Daytona USA (Japan).yss"));
  VitaConfigStatePath(state, sizeof(state), "Daytona USA (Japan)", 10);
  assert(!strcmp(state, "ux0:data/yabause-vita/states/Daytona USA (Japan).10.yss"));
  puts("vita config: ok");
  return 0;
}
