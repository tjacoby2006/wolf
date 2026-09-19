#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace streaming::pacing {

/**
 * Pacing configuration for the custom UDP sink.
 *
 * Extracted from the anonymous `custom_sink::PacingConfig` so the scheduling logic can be unit
 * tested without GStreamer, sockets or real time.
 */
struct Config {
  bool enabled = false;
  /** Target packets per millisecond (derived from bitrate / packet size). */
  std::size_t max_packets_per_ms = 0;
  /**
   * Maximum number of packets per sendmmsg() syscall.
   * Caps batch size to stay under 64KB per call, following Sunshine's pattern.
   * Computed at runtime as min(16, 65536 / packet_size).
   * Does not affect pacing rate — only syscall granularity.
   */
  std::size_t max_batch_size = 16;
};

/** Mutable pacing state carried between frames. */
struct State {
  std::chrono::steady_clock::time_point next_frame_start = std::chrono::steady_clock::now();
};

/** A single sendmmsg() call: send `count` packets starting at `offset`, not before `due`. */
struct PlannedBatch {
  std::size_t offset = 0;
  std::size_t count = 0;
  std::chrono::steady_clock::time_point due;
};

/** The result of planning one frame: the batches to send and the updated pacing state. */
struct Plan {
  std::vector<PlannedBatch> batches;
  State next_state;
};

/**
 * Plan how to send `num_packets` for one frame.
 *
 * This is a pure function: it computes the batches and their due times but performs no I/O and no
 * sleeping. The caller sends each batch and sleeps until its `due` time. Keeping the schedule
 * computation separate from the syscalls is what makes the pacing logic testable.
 *
 * Behaviour (preserved from the original inline implementation):
 *  - If pacing is disabled, or the frame fits in a single batch, emit one batch with no delay.
 *  - Otherwise spread the packets across 1ms windows, never exceeding `max_packets_per_ms` per
 *    window nor `max_batch_size` per syscall, and never sending a frame faster than its budget.
 */
Plan plan_frame(const Config &config,
                const State &state,
                std::size_t num_packets,
                std::chrono::steady_clock::time_point now);

} // namespace streaming::pacing
