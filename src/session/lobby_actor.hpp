#pragma once

#include <memory>

#include <session/actor.hpp>
#include <session/lobby_effect.hpp>
#include <session/lobby_input.hpp>
#include <session/lobby_model.hpp>

namespace wolf::session {

/**
 * The runtime side of a lobby (shared desktop).
 *
 * See `ActorRuntime` for the lifetime contract: detached threads must hold a `shared_ptr` to the
 * runtime, and `post()` holds only a weak reference to the actor.
 */
using LobbyRuntime = ActorRuntime<LobbyModel, LobbyInput, LobbyEffect>;

/**
 * A single-threaded actor that owns one lobby's lifecycle.
 *
 * See `Actor` for the threading model. This is the lobby-specific instantiation: it wires the pure
 * `step_lobby` function and the terminal-state predicate into the generic actor.
 */
class LobbyActor : public Actor<LobbyModel, LobbyInput, LobbyEffect> {
public:
  LobbyActor(LobbyModel initial, std::shared_ptr<LobbyRuntime> runtime)
      : Actor<LobbyModel, LobbyInput, LobbyEffect>(
            std::move(initial),
            std::move(runtime),
            Machine<LobbyModel, LobbyInput, LobbyEffect>{
                .step = [](const LobbyModel &model, const LobbyInput &input) {
                  auto transition = wolf::session::step_lobby(model, input);
                  return StateTransition<LobbyModel, LobbyEffect>{.model = std::move(transition.model),
                                                                  .effects = std::move(transition.effects)};
                },
                .is_terminal = [](const LobbyModel &model) { return wolf::session::is_terminal(model.state); }}) {
    // A stop request is the input that wakes the worker when the actor is asked to stop.
    this->set_stop_input(StopLobby{.reason = "actor shutdown"});
  }
};

} // namespace wolf::session