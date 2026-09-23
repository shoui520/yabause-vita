/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/telemetry_owners.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static VitaTelemetryOwners owners;
static pthread_key_t key;
static void revoke(void *cookie) {
  VitaTelemetryOwnerPublish(&owners, (unsigned)(uintptr_t)cookie - 1, 0);
}
static void *worker(void *arg) {
  unsigned slot = (unsigned)(uintptr_t)arg;
  int tid = 100 + (int)slot;
  assert(!pthread_setspecific(key, (void *)(uintptr_t)(slot + 1)));
  VitaTelemetryOwnerPublish(&owners, slot, tid);
  for (unsigned i = 0; i < 100000; ++i) {
    int found = VitaTelemetryOwnerFind(&owners, tid);
    assert(found == (int)slot);
    ++owners.phase[found][VT_M68K].entries;
  }
  return NULL;
}
int main(void) {
  pthread_t threads[8];
  assert(!pthread_key_create(&key, revoke));
  assert(VitaTelemetryOwnerFind(&owners, 0) == -1);
  assert(VitaTelemetryOwnerFind(&owners, 999) == -1);
  for (unsigned i = 0; i < 8; ++i)
    assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
  for (unsigned i = 0; i < 8; ++i) {
    assert(!pthread_join(threads[i], NULL));
    assert(owners.phase[i][VT_M68K].entries == 100000);
    assert(VitaTelemetryOwnerFind(&owners, 100 + (int)i) == -1);
  }
  // A reused kernel identity must bind to a fresh slot, never old counters.
  VitaTelemetryOwnerPublish(&owners, 8, 100);
  assert(VitaTelemetryOwnerFind(&owners, 100) == 8);
  assert(owners.phase[8][VT_M68K].entries == 0);
  VitaTelemetryOwnerPublish(&owners, 8, 0);
  assert(!pthread_key_delete(key));
  puts("telemetry owners: concurrent isolation, exit revocation and identity reuse passed");
}
