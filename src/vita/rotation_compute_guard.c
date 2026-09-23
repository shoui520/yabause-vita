/* SPDX-License-Identifier: GPL-2.0-or-later
 * The Vita product selects Yabause's CPU rotation renderer. These entry points
 * must never be reached: fail visibly instead of pretending GL compute ran.
 */
#include <stdlib.h>
#include "ygl.h"
#include "yui.h"
extern void YuiMsg(const char *format, ...);
static void unavailable(void) {
  YuiMsg("fatal: GL compute rotation selected on Vita; CPU rotation is required");
  abort();
}
void RBGGenerator_init(int width, int height) {
  (void)width; (void)height; unavailable();
}
void RBGGenerator_update(RBGDrawInfo *info) {
  (void)info; unavailable();
}
GLuint RBGGenerator_getTexture(int id) {
  (void)id; unavailable(); return 0;
}
void RBGGenerator_onFinish(void) { unavailable(); }
