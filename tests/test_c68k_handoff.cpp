/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/core/c68k/a9_handoff.h"
#include <cassert>
#include <thread>
#include <cstdio>
int main() {
  m68ka9::Handoff gate;
  assert(!gate.Running() && !gate.Park() && !gate.Resume() && !gate.BeginPublish());
  assert(gate.Start() && gate.Running() && !gate.Start());
  assert(!gate.BeginPublish() && !gate.EndPublish());
  assert(gate.Park() && !gate.Running() && !gate.Park());
  assert(gate.BeginPublish() && !gate.Resume() && !gate.Running());
  assert(gate.EndPublish() && gate.Resume() && gate.Running());
  gate.Stop();
  assert(!gate.Running());
  // Model the existing finish/frame-start queues with release/acquire events.
  // Non-atomic cache state can migrate only after the peer relinquishes it.
  std::atomic<unsigned> finished{0}, resumed{0};
  unsigned shared = 0;
  std::thread worker([&] {
    assert(gate.Start());
    for (unsigned frame = 1; frame <= 10000; ++frame) {
      assert(gate.Running() && shared == (frame - 1) * 2);
      ++shared;
      assert(gate.Park());
      finished.store(frame, std::memory_order_release);
      while (resumed.load(std::memory_order_acquire) != frame) std::this_thread::yield();
      assert(gate.Resume());
    }
    gate.Stop();
  });
  for (unsigned frame = 1; frame <= 10000; ++frame) {
    while (finished.load(std::memory_order_acquire) != frame) std::this_thread::yield();
    assert(gate.BeginPublish() && shared == frame * 2 - 1);
    ++shared;
    assert(gate.EndPublish());
    resumed.store(frame, std::memory_order_release);
  }
  worker.join();
  assert(shared == 20000 && !gate.Running());
  puts("68K handoff: invalid transitions and 10,000 exclusive ownership transfers passed");
}
