#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <session/lobby_effect.hpp>
#include <session/lobby_input.hpp>
#include <session/lobby_state.hpp>

namespace wolf::session {

/**
 * The immutable, actor-owned state of a lobby (shared desktop).
 *
 * Like `SessionModel`, this holds plain values only: the lobby is owned by exactly one thread, so
 * no `shared_ptr<immer::atom<...>>` handles are needed.
 */
struct LobbyModel {
  std::string lobby_id;
  LobbyState state = LobbyState::Created;

  /** The render node assigned by the load balancer. Empty until `LobbyGpuAssigned`. */
  std::string render_node;

  /** The Wayland socket name once the shared compositor is up. */
  std::string wayland_socket_name;

  /** Display mode, used when starting the compositor. */
  int width = 0;
  int height = 0;
  int refresh_rate = 0;

  /** Whether more than one session may attach. */
  bool multi_user = false;

  /** Stop the lobby once the last session leaves. */
  bool stop_when_everyone_leaves = false;

  /** Sessions currently attached to this lobby. */
  std::vector<std::uint64_t> connected_sessions;

  /** Human-readable failure reason when `state == Failed`. */
  std::string failure_reason;
};

/** The result of a single lobby transition: the new state plus the effects to perform. */
struct LobbyTransition {
  LobbyModel model;
  std::vector<LobbyEffect> effects;
};

/**
 * The pure lobby state machine.
 *
 * Like `step` for sessions, this is a total function: inputs that are not valid for the current
 * state are ignored rather than throwing, so a late or duplicate message can never corrupt the
 * lobby lifecycle.
 */
LobbyTransition step_lobby(const LobbyModel &model, const LobbyInput &input);

/** Convenience: apply an input and return only the new model (dropping effects). */
inline LobbyModel advance_lobby(const LobbyModel &model, const LobbyInput &input) {
  return step_lobby(model, input).model;
}

} // namespace wolf::session