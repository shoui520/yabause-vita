/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <psp2/gxm.h>
#include "telemetry.h"

/* Link-time wrappers observe EXISTING waits in the application and static
 * vitaGL archive. They never add a finish, change ordering or claim GPU time.
 * Match VitaSDK's declarations (including its void sceGxmFinish signature). */
void __real_sceGxmFinish(SceGxmContext *context);
int __real_sceGxmNotificationWait(const SceGxmNotification *notification);
int __real_sceGxmDisplayQueueFinish(void);

void __wrap_sceGxmFinish(SceGxmContext *context) {
  VT_SCOPE(VT_GPU_WAIT);
  __real_sceGxmFinish(context);
}
int __wrap_sceGxmNotificationWait(const SceGxmNotification *notification) {
  VT_SCOPE(VT_GPU_NOTIFICATION_WAIT);
  return __real_sceGxmNotificationWait(notification);
}
int __wrap_sceGxmDisplayQueueFinish(void) {
  VT_SCOPE(VT_DISPLAY_QUEUE_WAIT);
  return __real_sceGxmDisplayQueueFinish();
}
