#include <catch2/catch_test_macros.hpp>

#include <streaming/pacing.hpp>

using namespace streaming::pacing;
using clock_t_ = std::chrono::steady_clock;

namespace {
const auto T0 = clock_t_::time_point{};
}

TEST_CASE("pacing disabled sends the whole frame in one batch", "[pacing]") {
  Config config{.enabled = false, .max_packets_per_ms = 10, .max_batch_size = 4};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 100, T0);

  REQUIRE(plan.batches.size() == 1);
  REQUIRE(plan.batches[0].offset == 0);
  REQUIRE(plan.batches[0].count == 100);
}

TEST_CASE("a frame that fits in one batch is not paced", "[pacing]") {
  Config config{.enabled = true, .max_packets_per_ms = 10, .max_batch_size = 16};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 16, T0);

  REQUIRE(plan.batches.size() == 1);
  REQUIRE(plan.batches[0].count == 16);
}

TEST_CASE("paced frames account for every packet and respect the batch cap", "[pacing]") {
  Config config{.enabled = true, .max_packets_per_ms = 10, .max_batch_size = 4};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 25, T0);

  std::size_t total = 0;
  for (const auto &batch : plan.batches) {
    REQUIRE(batch.count <= config.max_batch_size);
    total += batch.count;
  }
  REQUIRE(total == 25);
}

TEST_CASE("pacing never exceeds the per-millisecond budget", "[pacing]") {
  Config config{.enabled = true, .max_packets_per_ms = 5, .max_batch_size = 16};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 50, T0);

  // Group batches by their due time (one window per millisecond) and check each window's budget.
  std::map<clock_t_::time_point, std::size_t> per_window;
  for (const auto &batch : plan.batches) {
    per_window[batch.due] += batch.count;
  }
  for (const auto &[due, count] : per_window) {
    REQUIRE(count <= config.max_packets_per_ms);
  }
}

TEST_CASE("zero packets produces no batches", "[pacing]") {
  Config config{.enabled = true, .max_packets_per_ms = 10, .max_batch_size = 4};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 0, T0);
  REQUIRE(plan.batches.empty());
}

TEST_CASE("the next frame is scheduled after the current one", "[pacing]") {
  Config config{.enabled = true, .max_packets_per_ms = 10, .max_batch_size = 4};
  auto plan = plan_frame(config, State{.next_frame_start = T0}, 25, T0);
  REQUIRE(plan.next_state.next_frame_start > T0);
}
