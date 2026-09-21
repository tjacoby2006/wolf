#include <session/session_model.hpp>

#include <utility>

namespace wolf::session {

namespace {

/** Build a `Teardown` + `ReleaseGpu` pair for a session that is going away. */
std::vector<SessionEffect> teardown_effects(std::uint64_t session_id, std::string reason) {
  return {Teardown{.session_id = session_id, .reason = std::move(reason)},
          ReleaseGpu{.session_id = session_id}};
}

/** Move to `Failed`, tearing down whatever was already started. */
Transition fail(const SessionModel &model, std::string reason) {
  auto next = model;
  next.state = SessionState::Failed;
  next.failure_reason = reason;
  return Transition{.model = std::move(next), .effects = teardown_effects(model.session_id, std::move(reason))};
}

/** Move to `Stopping`, asking the runtime to tear everything down. */
Transition stop(const SessionModel &model, std::string reason) {
  auto next = model;
  next.state = SessionState::Stopping;
  return Transition{.model = std::move(next), .effects = teardown_effects(model.session_id, std::move(reason))};
}

/**
 * Build the effects for adopting a session and asking the runtime to pick a GPU.
 *
 * Registration happens *first* so that the session is already visible in `running_sessions` when
 * the GPU is assigned: a viewer can hit RTSP setup as soon as the runner reports ready.
 */
Transition adopt(const SessionModel &model) {
  return Transition{.model = model,
                    .effects = {AdoptSession{.session_id = model.session_id},
                                AssignGpu{.session_id = model.session_id}}};
}

/** No-op transition: ignore an input that is not valid in the current state. */
Transition ignore(const SessionModel &model) {
  return Transition{.model = model, .effects = {}};
}

/**
 * Emit effects without changing state.
 *
 * Beware of reading from `next` in the initializer lists below. `Transition::model` is declared
 * before `Transition::effects`, and the initializer-clauses of a braced-init-list are evaluated in
 * declaration order, so `std::move(next)` runs *before* the effects are built. Any field read off
 * `next` therefore observes a moved-from value: for a `std::string` that is the empty string.
 *
 * That is exactly how an empty `render_node` reached the compositor: the effect carried the
 * moved-from string, the Wayland source was launched with `render_node=` (empty) and
 * gst-wayland-display aborted with "Failed to open drm node !", then panicked. Always read such
 * fields off `model` (which is a reference to the original, un-moved object) or off the input.
 */
Transition effect_only(const SessionModel &model, SessionEffect effect) {
  return Transition{.model = model, .effects = {std::move(effect)}};
}

} // namespace

Transition step(const SessionModel &model, const SessionInput &input) {
  // Terminal states never transition again.
  if (is_terminal(model.state)) {
    return ignore(model);
  }

  // A stop request is valid from any non-terminal state and always wins.
  if (std::holds_alternative<StopRequested>(input)) {
    return stop(model, std::get<StopRequested>(input).reason);
  }

  // A desktop failure is fatal from any state that is still waiting on the compositor.
  if (std::holds_alternative<DesktopFailed>(input)) {
    return fail(model, std::get<DesktopFailed>(input).reason);
  }

  switch (model.state) {
  case SessionState::Created:
    if (std::holds_alternative<StartSession>(input)) {
      return adopt(model);
    }
    if (std::holds_alternative<GpuAssigned>(input)) {
      // Read the node off the *input* rather than `next`: `next` is moved into `.model` earlier in
      // the same initializer list, so reading `next.render_node` there would yield "".
      const auto &render_node = std::get<GpuAssigned>(input).render_node;
      auto next = model;
      next.state = SessionState::DesktopStarting;
      next.render_node = render_node;
      return Transition{.model = std::move(next),
                        .effects = {StartDesktop{.session_id = model.session_id,
                                                 .render_node = render_node,
                                                 .width = model.width,
                                                 .height = model.height,
                                                 .refresh_rate = model.refresh_rate}}};
    }
    return ignore(model);

  case SessionState::DesktopStarting:
    if (std::holds_alternative<DesktopReady>(input)) {
      auto next = model;
      next.state = SessionState::RunnerStarting;
      next.wayland_socket_name = std::get<DesktopReady>(input).wayland_socket_name;
      // `model.render_node` is untouched by this transition, so it is safe to read after the move.
      return Transition{.model = std::move(next),
                        .effects = {StartRunner{.session_id = model.session_id, .render_node = model.render_node}}};
    }
    return ignore(model);

  case SessionState::RunnerStarting:
    if (std::holds_alternative<RunnerStarted>(input)) {
      auto next = model;
      next.state = SessionState::RunnerRunning;
      return Transition{.model = std::move(next), .effects = {}};
    }
    return ignore(model);

  case SessionState::RunnerRunning:
    if (std::holds_alternative<RtpPingReceived>(input)) {
      // Read the client off the *input*: `next` is moved into `.model` earlier in the same
      // initializer list, so reading `next.client_ip` there would yield "".
      const auto &ping = std::get<RtpPingReceived>(input);
      auto next = model;
      next.state = SessionState::Streaming;
      next.client_ip = ping.client_ip;
      next.client_port = ping.client_port;
      return Transition{.model = std::move(next),
                        .effects = {StartStreaming{.session_id = model.session_id,
                                                   .client_ip = ping.client_ip,
                                                   .client_port = ping.client_port}}};
    }
    if (std::holds_alternative<RunnerExited>(input)) {
      return stop(model, "runner exited");
    }
    if (std::holds_alternative<DevicePlugged>(input)) {
      return effect_only(model,
                         PlugDevice{.session_id = model.session_id,
                                    .device_id = std::get<DevicePlugged>(input).device_id});
    }
    if (std::holds_alternative<DeviceUnplugged>(input)) {
      return effect_only(model,
                         UnplugDevice{.session_id = model.session_id,
                                      .device_id = std::get<DeviceUnplugged>(input).device_id});
    }
    return ignore(model);

  case SessionState::Streaming:
    if (std::holds_alternative<PauseRequested>(input)) {
      auto next = model;
      next.state = SessionState::Paused;
      return Transition{.model = std::move(next), .effects = {PauseStreaming{.session_id = model.session_id}}};
    }
    if (std::holds_alternative<IdrRequested>(input)) {
      return effect_only(model, RequestIdr{.session_id = model.session_id});
    }
    if (std::holds_alternative<DevicePlugged>(input)) {
      return effect_only(model,
                         PlugDevice{.session_id = model.session_id,
                                    .device_id = std::get<DevicePlugged>(input).device_id});
    }
    if (std::holds_alternative<DeviceUnplugged>(input)) {
      return effect_only(model,
                         UnplugDevice{.session_id = model.session_id,
                                      .device_id = std::get<DeviceUnplugged>(input).device_id});
    }
    if (std::holds_alternative<RunnerExited>(input)) {
      return stop(model, "runner exited");
    }
    return ignore(model);

  case SessionState::Paused:
    if (std::holds_alternative<ResumeRequested>(input)) {
      auto next = model;
      next.state = SessionState::Streaming;
      return Transition{.model = std::move(next), .effects = {ResumeStreaming{.session_id = model.session_id}}};
    }
    if (std::holds_alternative<RunnerExited>(input)) {
      return stop(model, "runner exited");
    }
    return ignore(model);

  case SessionState::Stopping:
    if (std::holds_alternative<TeardownComplete>(input)) {
      auto next = model;
      next.state = SessionState::Stopped;
      return Transition{.model = std::move(next), .effects = {}};
    }
    return ignore(model);

  case SessionState::Stopped:
  case SessionState::Failed:
    return ignore(model);
  }

  return ignore(model);
}

} // namespace wolf::session
