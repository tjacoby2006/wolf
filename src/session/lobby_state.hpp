#pragma once

#include <string_view>

namespace wolf::session {

/**
 * The explicit lifecycle of a lobby.
 *
 * A lobby is a *shared desktop*: one compositor, one audio sink, one runner and one GPU, which
 * multiple Moonlight sessions attach to. It used to be driven by seven separate event-bus handlers
 * in `sessions/lobbies.cpp`, each spawning detached threads and promise chains. It is now owned by
 * a single LobbyActor walking this state machine.
 *
 * Happy path:
 *   Created -> DesktopStarting -> RunnerStarting -> RunnerRunning -> Stopping -> Stopped
 *
 * `RunnerRunning` is the steady state where sessions join and leave. `Failed` is terminal and
 * reachable from any non-terminal state.
 */
enum class LobbyState {
  Created,
  DesktopStarting,
  RunnerStarting,
  RunnerRunning,
  Stopping,
  Stopped,
  Failed,
};

/** @return true if no further transitions are possible from this state. */
constexpr bool is_terminal(LobbyState state) {
  return state == LobbyState::Stopped || state == LobbyState::Failed;
}

/** @return true if the lobby is up and accepting sessions. */
constexpr bool is_accepting_sessions(LobbyState state) {
  return state == LobbyState::RunnerRunning;
}

constexpr std::string_view to_string(LobbyState state) {
  switch (state) {
  case LobbyState::Created:
    return "Created";
  case LobbyState::DesktopStarting:
    return "DesktopStarting";
  case LobbyState::RunnerStarting:
    return "RunnerStarting";
  case LobbyState::RunnerRunning:
    return "RunnerRunning";
  case LobbyState::Stopping:
    return "Stopping";
  case LobbyState::Stopped:
    return "Stopped";
  case LobbyState::Failed:
    return "Failed";
  }
  return "Unknown";
}

} // namespace wolf::session