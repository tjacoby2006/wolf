#include <session/lobby_model.hpp>

#include <algorithm>
#include <utility>

namespace wolf::session {

namespace {

/** Build a `TeardownLobby` + `ReleaseLobbyGpu` pair for a lobby that is going away. */
std::vector<LobbyEffect> teardown_effects(const std::string &lobby_id, std::string reason) {
  return {
      TeardownLobby{.lobby_id = lobby_id, .reason = std::move(reason)},
      ReleaseLobbyGpu{.lobby_id = lobby_id},
  };
}

/** Move to `Failed`, tearing down whatever was already started. */
LobbyTransition fail(const LobbyModel &model, std::string reason) {
  auto next = model;
  next.state = LobbyState::Failed;
  next.failure_reason = reason;
  return LobbyTransition{.model = std::move(next),
                         .effects = teardown_effects(model.lobby_id, std::move(reason))};
}

/** Move to `Stopping`, asking the runtime to tear everything down. */
LobbyTransition stop(const LobbyModel &model, std::string reason) {
  auto next = model;
  next.state = LobbyState::Stopping;
  return LobbyTransition{.model = std::move(next),
                         .effects = teardown_effects(model.lobby_id, std::move(reason))};
}

/** No-op transition: ignore an input that is not valid in the current state. */
LobbyTransition ignore(const LobbyModel &model) {
  return LobbyTransition{.model = model, .effects = {}};
}

/** Emit a single effect without changing state. */
LobbyTransition effect_only(const LobbyModel &model, LobbyEffect effect) {
  return LobbyTransition{.model = model, .effects = {std::move(effect)}};
}

/** @return true if `session_id` is already attached. */
bool is_attached(const LobbyModel &model, std::uint64_t session_id) {
  return std::find(model.connected_sessions.begin(), model.connected_sessions.end(), session_id) !=
         model.connected_sessions.end();
}

} // namespace

LobbyTransition step_lobby(const LobbyModel &model, const LobbyInput &input) {
  // Terminal states never transition again.
  if (is_terminal(model.state)) {
    return ignore(model);
  }

  // A stop request is valid from any non-terminal state and always wins.
  if (std::holds_alternative<StopLobby>(input)) {
    return stop(model, std::get<StopLobby>(input).reason);
  }

  // A desktop failure is fatal from any state that is still waiting on the compositor.
  if (std::holds_alternative<LobbyDesktopFailed>(input)) {
    return fail(model, std::get<LobbyDesktopFailed>(input).reason);
  }

  switch (model.state) {
  case LobbyState::Created:
    if (std::holds_alternative<StartLobby>(input)) {
      return effect_only(model, AssignLobbyGpu{.lobby_id = model.lobby_id});
    }
    if (std::holds_alternative<LobbyGpuAssigned>(input)) {
      auto next = model;
      next.state = LobbyState::DesktopStarting;
      next.render_node = std::get<LobbyGpuAssigned>(input).render_node;
      return LobbyTransition{.model = std::move(next),
                             .effects = {StartLobbyDesktop{.lobby_id = model.lobby_id,
                                                           .render_node = next.render_node,
                                                           .width = model.width,
                                                           .height = model.height,
                                                           .refresh_rate = model.refresh_rate}}};
    }
    return ignore(model);

  case LobbyState::DesktopStarting:
    if (std::holds_alternative<LobbyDesktopReady>(input)) {
      auto next = model;
      next.state = LobbyState::RunnerStarting;
      next.wayland_socket_name = std::get<LobbyDesktopReady>(input).wayland_socket_name;
      return LobbyTransition{.model = std::move(next),
                             .effects = {StartLobbyRunner{.lobby_id = model.lobby_id,
                                                          .render_node = model.render_node}}};
    }
    return ignore(model);

  case LobbyState::RunnerStarting:
    if (std::holds_alternative<LobbyRunnerStarted>(input)) {
      auto next = model;
      next.state = LobbyState::RunnerRunning;
      return LobbyTransition{.model = std::move(next), .effects = {}};
    }
    return ignore(model);

  case LobbyState::RunnerRunning:
    if (std::holds_alternative<SessionJoined>(input)) {
      auto session_id = std::get<SessionJoined>(input).session_id;
      // Ignore a duplicate join, and refuse a second session in a single-user lobby.
      if (is_attached(model, session_id) || (!model.multi_user && !model.connected_sessions.empty())) {
        return ignore(model);
      }
      auto next = model;
      next.connected_sessions.push_back(session_id);
      return LobbyTransition{.model = std::move(next),
                             .effects = {AttachSession{.lobby_id = model.lobby_id, .session_id = session_id}}};
    }
    if (std::holds_alternative<SessionLeft>(input)) {
      auto session_id = std::get<SessionLeft>(input).session_id;
      if (!is_attached(model, session_id)) {
        return ignore(model);
      }
      auto next = model;
      next.connected_sessions.erase(
          std::remove(next.connected_sessions.begin(), next.connected_sessions.end(), session_id),
          next.connected_sessions.end());

      // The last session left and the lobby is set to stop when everyone leaves. The session is
      // still detached first (input/audio/video switched back), then the lobby tears down.
      if (next.connected_sessions.empty() && model.stop_when_everyone_leaves) {
        auto transition = stop(next, "last session left");
        transition.effects.insert(transition.effects.begin(),
                                  DetachSession{.lobby_id = model.lobby_id, .session_id = session_id});
        return transition;
      }
      return LobbyTransition{.model = std::move(next),
                             .effects = {DetachSession{.lobby_id = model.lobby_id, .session_id = session_id}}};
    }
    if (std::holds_alternative<LobbyRunnerExited>(input)) {
      return stop(model, "runner exited");
    }
    return ignore(model);

  case LobbyState::Stopping:
    if (std::holds_alternative<LobbyTeardownComplete>(input)) {
      auto next = model;
      next.state = LobbyState::Stopped;
      return LobbyTransition{.model = std::move(next), .effects = {}};
    }
    return ignore(model);

  case LobbyState::Stopped:
  case LobbyState::Failed:
    return ignore(model);
  }

  return ignore(model);
}

} // namespace wolf::session