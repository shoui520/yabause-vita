/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/vita/sound_budget.h"
#include <cassert>
#include <thread>
#include <cstdio>

int main() {
  { // Publication before the wait must not require another notification.
    VitaSoundBudget budget;
    budget.publish(9 << 8);
    uint64_t received;
    assert(budget.wait_changed(0, 8, received) && received == 9);
    budget.publish(0); // Frame budget reset is a change too.
    assert(budget.wait_changed(9, 8, received) && received == 0);
  }
  { // Alternate publication/wait ordering without sleeps or timing assumptions.
    VitaSoundBudget budget, acknowledged;
    std::thread consumer([&] {
      uint64_t last = 0, value;
      for (unsigned i = 1; i <= 10000; ++i) {
        assert(budget.wait_changed(last, 8, value));
        assert(value == i);
        last = value;
        acknowledged.publish(i);
      }
    });
    for (unsigned i = 1; i <= 10000; ++i) {
      budget.publish((uint64_t(i - 1) << 8) | 1); // Fractional-only change.
      budget.publish(uint64_t(i) << 8);
      uint64_t value;
      assert(acknowledged.wait_changed(i - 1, 0, value) && value == i);
    }
    consumer.join();
  }
  for (unsigned i = 0; i < 100; ++i) {
    VitaSoundBudget budget;
    std::thread consumer([&] {
      uint64_t value;
      assert(!budget.wait_changed(0, 8, value));
    });
    budget.stop(); // Correct both before and during the consumer's wait.
    consumer.join();
    budget.publish(4 << 8);
    uint64_t value;
    assert(!budget.wait_changed(0, 8, value)); // Publication cannot cancel stop.
    budget.start(); // Previous consumer is joined before re-arming.
    assert(budget.wait_changed(0, 8, value) && value == 4);
  }
  puts("sound budget: 10000 handoffs, fractional updates, reset and cancellation passed");
}
