#include <session/session_model.hpp>

#include <utility>

namespace wolf::session {

namespace {

/** Build a `Teardown` + `ReleaseGpu` pair for a session that is going away. */
std::vector<SessionEffect> teardown_effects(std::uint64_t session_id, std::string reason) {
  return {
      Teardown{.session_id = session_id, .reason = std::move(reason)},
      ReleaseGpu{.session_id = session_id},
  };
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

/** No-op transition: ignore an input that is not valid in the current state. */
Transition ignore(const SessionModel &model) {
  return Transition{.model = model, .effects = {}};
}

/** Emit a single effect without changing state. */
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
    if (std::holds_alternative<GpuAssigned>(input)) {
      auto next = model;
      next.state = SessionState::DesktopStarting;
      next.render_node = std::get<GpuAssigned>(input).render_node;
      return Transition{.model = std::move(next),
                        .effects = {StartDesktop{.session_id = model.session_id,
                                                 .render_node = next.render_node,
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
      auto next = model;
      next.state = SessionState::Streaming;
      next.client_ip = std::get<RtpPingReceived>(input).client_ip;
      next.client_port = std::get<RtpPingReceived>(input).client_port;
      return Transition{.model = std::move(next),
                        .effects = {StartStreaming{.session_id = model.session_id,
                                                   .client_ip = next.client_ip,
                                                   .client_port = next.client_port}}};
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
