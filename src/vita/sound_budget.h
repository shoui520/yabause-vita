/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITA_SOUND_BUDGET_H
#define VITA_SOUND_BUDGET_H
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <cstdlib>

/* Single producer publishes the existing fractional cycle budget; one sound
 * consumer waits for a changed INTEGER budget. No timing/cycle policy here.
 * Predicate and cancellation are checked under the same mutex as notification.
 * Repeated publications of an unchanged value need no kernel synchronization.
 * Counter.cpp retains a selectable spin baseline for validation. */
class VitaSoundBudget {
  std::atomic<uint64_t> budget{0};
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  bool stopped = false;
  static void checked(int rc) { if (rc) std::abort(); }
public:
  VitaSoundBudget() = default;
  VitaSoundBudget(const VitaSoundBudget&) = delete;
  VitaSoundBudget& operator=(const VitaSoundBudget&) = delete;
  ~VitaSoundBudget() {
    /* Owner must join the consumer before destruction. */
    checked(pthread_cond_destroy(&changed));
    checked(pthread_mutex_destroy(&mutex));
  }
  uint64_t load() const { return budget.load(); }
  /* Only before creating a consumer, after any previous consumer was joined.
   * Preserve the budget already initialized by the emulator's reset path. */
  void start() {
    checked(pthread_mutex_lock(&mutex));
    stopped = false;
    checked(pthread_mutex_unlock(&mutex));
  }
  void publish(uint64_t value) {
    if (budget.exchange(value) == value) return;
    /* If the consumer saw the old value while holding mutex, this lock
     * cannot complete until its cond_wait has atomically begun waiting. */
    checked(pthread_mutex_lock(&mutex));
    checked(pthread_cond_signal(&changed));
    checked(pthread_mutex_unlock(&mutex));
  }
  bool wait_changed(uint64_t previous_integer, unsigned fractional_bits,
                    uint64_t &current_integer) {
    if (fractional_bits >= 64) std::abort();
    checked(pthread_mutex_lock(&mutex));
    while (!stopped && (budget.load() >> fractional_bits) == previous_integer)
      checked(pthread_cond_wait(&changed, &mutex));
    current_integer = budget.load() >> fractional_bits;
    bool running = !stopped;
    checked(pthread_mutex_unlock(&mutex));
    return running;
  }
  void stop() {
    checked(pthread_mutex_lock(&mutex));
    stopped = true;
    checked(pthread_cond_broadcast(&changed));
    checked(pthread_mutex_unlock(&mutex));
  }
};
#endif
