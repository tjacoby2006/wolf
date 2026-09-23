#include <session/lobby_model.hpp>

#include <algorithm>
#include <utility>

namespace wolf::session {

namespace {

/**
 * Build the effects for a lobby that is going away: every connected session is detached *first*
 * (switching its input/audio/video back to its own desktop) and only then is the shared compositor
 * torn down.
 *
 * The detach has to be part of the teardown itself rather than of one specific input. A lobby can be
 * torn down by an explicit stop, by its runner exiting (the user quitting the app), or by a failed
 * desktop, and any path that drops `wayland_display` without re-binding the sessions leaves them
 * pointing at a destroyed compositor: the client keeps encoding from a dead interpipe source (a
 * permanent black screen) and its mouse/keyboard events are sent to a wayland display that no longer
 * exists (input appears dead until the client reconnects).
 *
 * Always read the sessions off `model`, never off `next`: as documented on `LobbyTransition`, the
 * moved-from `next` is only safe to read for members the transition does not touch.
 */
std::vector<LobbyEffect> teardown_effects(const LobbyModel &model, std::string reason) {
  std::vector<LobbyEffect> effects;
  effects.reserve(model.connected_sessions.size() + 2);
  for (auto session_id : model.connected_sessions) {
    effects.push_back(DetachSession{.lobby_id = model.lobby_id, .session_id = session_id});
  }
  effects.push_back(TeardownLobby{.lobby_id = model.lobby_id, .reason = std::move(reason)});
  effects.push_back(ReleaseLobbyGpu{.lobby_id = model.lobby_id});
  return effects;
}

/** Move to `Failed`, tearing down whatever was already started. */
LobbyTransition fail(const LobbyModel &model, std::string reason) {
  auto next = model;
  next.state = LobbyState::Failed;
  next.failure_reason = reason;
  return LobbyTransition{.model = std::move(next), .effects = teardown_effects(model, std::move(reason))};
}

/** Move to `Stopping`, asking the runtime to tear everything down. */
LobbyTransition stop(const LobbyModel &model, std::string reason) {
  auto next = model;
  next.state = LobbyState::Stopping;
  return LobbyTransition{.model = std::move(next), .effects = teardown_effects(model, std::move(reason))};
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

  // A stop request is valid from any state that is not already stopping, and it wins over every
  // other input. Every connected session is detached first so its input/audio/video is switched back
  // before the desktop goes away.
  //
  // A stop that arrives while we are *already* stopping is ignored. Tearing the lobby down fires a
  // `StopLobbyEvent` so that its own producer pipelines and runner are released (see the runtime's
  // `teardown`), and the bus routes that event straight back here as another `StopLobby`. Without
  // this guard the teardown would re-enter itself, re-emitting every `DetachSession` and re-plugging
  // the sessions' joypads into their own runners.
  if (std::holds_alternative<StopLobby>(input)) {
    if (model.state == LobbyState::Stopping) {
      return ignore(model);
    }
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
      // Read the node off the *input* rather than `next`: `next` is moved into `.model` earlier in
      // the same initializer list, so reading `next.render_node` there would yield "".
      const auto &render_node = std::get<LobbyGpuAssigned>(input).render_node;
      auto next = model;
      next.state = LobbyState::DesktopStarting;
      next.render_node = render_node;
      return LobbyTransition{.model = std::move(next),
                             .effects = {StartLobbyDesktop{.lobby_id = model.lobby_id,
                                                           .render_node = render_node,
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

      // The last session left and the lobby is set to stop when everyone leaves. `next` no longer
      // lists the leaving session, so its detach is added explicitly (input/audio/video switched
      // back) before the teardown the `stop` helper builds.
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
      // The user quit the app, so the runner is gone: detach every joined session back to its own
      // desktop before the shared compositor is dropped (see `teardown_effects`).
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