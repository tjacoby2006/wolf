#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <session/session_effect.hpp>
#include <session/session_input.hpp>
#include <session/session_state.hpp>

namespace wolf::session {

/**
 * The immutable, actor-owned state of a single stream session.
 *
 * Unlike the old `events::StreamSession`, this holds no `shared_ptr<immer::atom<...>>` handles
 * and no `shared_ptr<std::optional<...>>` device slots. Those existed only because the session
 * was shared across threads. In the actor model the session is owned by exactly one thread, so
 * plain values are enough.
 */
struct SessionModel {
  std::uint64_t session_id = 0;
  SessionState state = SessionState::Created;

  /** The render node assigned by the load balancer. Empty until `GpuAssigned`. */
  std::string render_node;

  /** The Wayland socket name once the compositor is up. */
  std::string wayland_socket_name;

  /** Client networking, filled in when the RTP ping arrives. */
  std::string client_ip;
  std::uint16_t client_port = 0;

  /** Display mode, used when starting the compositor. */
  int width = 0;
  int height = 0;
  int refresh_rate = 0;

  /** Devices currently plugged into the runner. */
  std::vector<std::string> plugged_devices;

  /** Human-readable failure reason when `state == Failed`. */
  std::string failure_reason;
};

/**
 * The result of a single transition: the new state plus the effects to perform.
 */
struct Transition {
  SessionModel model;
  std::vector<SessionEffect> effects;
};

/**
 * The pure session state machine.
 *
 * `step` is a total function: every (state, input) pair is handled. Inputs that are not valid
 * for the current state are ignored (no state change, no effects) rather than throwing, so a
 * late or duplicate message from a protocol adapter can never corrupt the lifecycle.
 *
 * This is the heart of the rewrite: the entire session lifecycle is now one readable function
 * instead of a web of event-bus handlers and detached threads.
 */
Transition step(const SessionModel &model, const SessionInput &input);

/**
 * Convenience: apply an input and return only the new model (dropping effects).
 * Useful in tests that only care about state sequencing.
 */
inline SessionModel advance(const SessionModel &model, const SessionInput &input) {
  return step(model, input).model;
}

} // namespace wolf::session
