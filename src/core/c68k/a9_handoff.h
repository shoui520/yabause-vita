/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
namespace m68ka9 {
// Scheduler ownership, not a general lock. Only the sound worker starts,
// parks and resumes; main publishes after receiving its finish event and ends
// publication before sending the resume event. No simultaneous code mutation.
class Handoff {
  enum Phase { Disabled, Executing, Parked, Publishing };
  std::atomic<Phase> phase{Disabled};
  bool Move(Phase from, Phase to) {
    return phase.compare_exchange_strong(from, to, std::memory_order_acq_rel);
  }
public:
  bool Start() { return Move(Disabled, Executing); }
  bool Park() { return Move(Executing, Parked); }
  bool BeginPublish() { return Move(Parked, Publishing); }
  bool EndPublish() { return Move(Publishing, Parked); }
  bool Resume() { return Move(Parked, Executing); }
  // Hot query is made only by the worker after Start/Resume's acquire; avoid
  // an ARMv7 barrier on every guest instruction. Transitions carry ordering.
  bool Running() const { return phase.load(std::memory_order_relaxed) == Executing; }
  void Stop() { phase.store(Disabled, std::memory_order_release); }
};
}
