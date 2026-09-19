#pragma once

#include <string_view>

namespace wolf::session {

/**
 * The explicit lifecycle of a single stream session.
 *
 * This replaces the implicit, event-handler-driven lifecycle that used to be spread across
 * `sessions/moonlight.cpp` (StreamSession -> StartRunner -> VideoSession/AudioSession handlers,
 * each spawning detached threads). Every session is now owned by one SessionActor that walks
 * this state machine linearly, so the whole lifecycle can be read top-to-bottom.
 *
 * Happy path:
 *   Created -> DesktopStarting -> RunnerStarting -> RunnerRunning -> Streaming
 *           -> Stopping -> Stopped
 *
 * `Paused` is reachable from `Streaming` and returns to `Streaming`.
 * `Failed` is terminal and reachable from any non-terminal state.
 */
enum class SessionState {
  Created,
  DesktopStarting,
  RunnerStarting,
  RunnerRunning,
  Streaming,
  Paused,
  Stopping,
  Stopped,
  Failed,
};

/**
 * @return true if no further transitions are possible from this state.
 */
constexpr bool is_terminal(SessionState state) {
  return state == SessionState::Stopped || state == SessionState::Failed;
}

/**
 * @return true if the session is actively producing a stream.
 */
constexpr bool is_active(SessionState state) {
  return state == SessionState::Streaming || state == SessionState::Paused;
}

constexpr std::string_view to_string(SessionState state) {
  switch (state) {
  case SessionState::Created:
    return "Created";
  case SessionState::DesktopStarting:
    return "DesktopStarting";
  case SessionState::RunnerStarting:
    return "RunnerStarting";
  case SessionState::RunnerRunning:
    return "RunnerRunning";
  case SessionState::Streaming:
    return "Streaming";
  case SessionState::Paused:
    return "Paused";
  case SessionState::Stopping:
    return "Stopping";
  case SessionState::Stopped:
    return "Stopped";
  case SessionState::Failed:
    return "Failed";
  }
  return "Unknown";
}

} // namespace wolf::session
