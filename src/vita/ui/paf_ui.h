/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_PAF_UI_H
#define VITA_PAF_UI_H

#include "../config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VITA_UI_NONE,
  VITA_UI_RESUME,
  VITA_UI_SAVE_STATE,
  VITA_UI_LOAD_STATE,
  VITA_UI_RESET,
  VITA_UI_QUIT
} VitaUiAction;

/* The game list, drawn by PAF on its own display (no vitaGL). Choosing a
 * game restarts the application with "--game <path>"; returns only when
 * PAF cannot start or the restart fails. */
int VitaUiRunLauncher(VitaConfig *config, const char *config_path);

/* The in-game menu, drawn by PAF into vitaGL's frames. Initialized once
 * vitaGL is running; config is updated (and saved) by the settings pages.
 * title is the disc name, which also names the game's state files. */
int VitaUiMenuInit(VitaConfig *config, const char *config_path, const char *title);
int VitaUiMenuAvailable(void);
void VitaUiMenuOpen(void);
/* Presents one menu frame over the game frame shown when the menu opened.
 * Called on the graphics thread in place of the game's swap. */
void VitaUiMenuFrame(void);
/* The action chosen since the last call (RESUME: Resume or Back). */
VitaUiAction VitaUiMenuPoll(void);
/* The slot (1..VITA_STATE_SLOTS) of the last SAVE_STATE or LOAD_STATE. */
int VitaUiMenuSlot(void);
void VitaUiMenuClose(void);
/* The settings pages are shown (SELECT does not close the menu then). */
int VitaUiMenuSettingsShown(void);
void VitaUiMenuSetStatus(const char *text);
/* Nonzero once after the settings pages changed the configuration. */
int VitaUiMenuTakeConfigChange(void);

#ifdef __cplusplus
}
#endif

#endif
