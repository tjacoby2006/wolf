#pragma once

#include <memory>

#include <session/actor.hpp>
#include <session/session_effect.hpp>
#include <session/session_input.hpp>
#include <session/session_model.hpp>

namespace wolf::session {

/**
 * The runtime side of a stream session.
 *
 * See `ActorRuntime` for the lifetime contract: detached threads must hold a `shared_ptr` to the
 * runtime, and `post()` holds only a weak reference to the actor.
 */
using SessionRuntime = ActorRuntime<SessionModel, SessionInput, SessionEffect>;

/**
 * A single-threaded actor that owns one stream session's lifecycle.
 *
 * See `Actor` for the threading model. This is the session-specific instantiation: it wires the
 * pure `step` function and the terminal-state predicate into the generic actor.
 */
class SessionActor : public Actor<SessionModel, SessionInput, SessionEffect> {
public:
  SessionActor(SessionModel initial, std::shared_ptr<SessionRuntime> runtime)
      : Actor<SessionModel, SessionInput, SessionEffect>(
            std::move(initial),
            std::move(runtime),
            Machine<SessionModel, SessionInput, SessionEffect>{
                .step = [](const SessionModel &model, const SessionInput &input) {
                  auto transition = wolf::session::step(model, input);
                  return StateTransition<SessionModel, SessionEffect>{.model = std::move(transition.model),
                                                                      .effects = std::move(transition.effects)};
                },
                .is_terminal = [](const SessionModel &model) { return wolf::session::is_terminal(model.state); }}) {
    // A stop request is the input that wakes the worker when the actor is asked to stop.
    this->set_stop_input(StopRequested{.reason = "actor shutdown"});
  }
};

} // namespace wolf::session