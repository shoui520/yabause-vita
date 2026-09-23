/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_SOUND_BUDGET_API_H
#define VITA_SOUND_BUDGET_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void VitaSoundBudgetConfigure(int blocking); /* Before emulator initialization. */
int VitaSoundBudgetBlocking(void);
void VitaSoundBudgetStart(void);
void VitaSoundBudgetStop(void);
int VitaSoundBudgetWait(uint64_t previous_integer, uint64_t *current_integer);
#ifdef __cplusplus
}
#endif
#endif
