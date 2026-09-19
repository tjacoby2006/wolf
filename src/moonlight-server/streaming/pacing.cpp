#include <streaming/pacing.hpp>

#include <algorithm>

namespace streaming::pacing {

Plan plan_frame(const Config &config,
                const State &state,
                std::size_t num_packets,
                std::chrono::steady_clock::time_point now) {
  Plan plan;
  plan.next_state = state;

  if (num_packets == 0) {
    return plan;
  }

  // Fast path: no pacing, or the whole frame fits in one syscall.
  if (!config.enabled || num_packets <= config.max_batch_size) {
    plan.batches.push_back(PlannedBatch{.offset = 0, .count = num_packets, .due = now});
    return plan;
  }

  // A frame must not start before the previous frame's scheduled end.
  auto frame_start = std::max(state.next_frame_start, now);
  std::size_t packets_sent = 0;
  std::size_t packets_in_window = 0;

  while (packets_sent < num_packets) {
    std::size_t remaining = num_packets - packets_sent;
    std::size_t budget = config.max_packets_per_ms - packets_in_window;

    if (budget == 0) {
      // Window exhausted: wait until the next 1ms window opens.
      auto due = frame_start + std::chrono::nanoseconds(1000000) * packets_sent / config.max_packets_per_ms;
      plan.batches.push_back(PlannedBatch{.offset = packets_sent, .count = 0, .due = due});
      packets_in_window = 0;
      continue;
    }

    std::size_t batch = std::min({budget, config.max_batch_size, remaining});
    auto due = frame_start + std::chrono::nanoseconds(1000000) * packets_sent / config.max_packets_per_ms;
    plan.batches.push_back(PlannedBatch{.offset = packets_sent, .count = batch, .due = due});

    packets_sent += batch;
    packets_in_window += batch;
  }

  plan.next_state.next_frame_start =
      frame_start + std::chrono::nanoseconds(1000000) * packets_sent / config.max_packets_per_ms;
  return plan;
}

} // namespace streaming::pacing
