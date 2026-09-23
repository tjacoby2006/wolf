#include <sessions/session_runtime.hpp>

#include <core/audio.hpp>
#include <core/input.hpp>
#include <core/virtual-display.hpp>
#include <helpers/logger.hpp>
#include <immer/vector_transient.hpp>
#include <platforms/hw.hpp>
#include <sessions/common.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

using namespace wolf::session;

MoonlightSessionRuntime::MoonlightSessionRuntime(SessionContext context) : context_(std::move(context)) {}

void MoonlightSessionRuntime::execute(const SessionEffect &effect) {
  std::visit(
      [this](const auto &e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, AdoptSession>) {
          adopt_session(e.session_id);
        } else if constexpr (std::is_same_v<T, AssignGpu>) {
          assign_gpu(e.session_id);
        } else if constexpr (std::is_same_v<T, StartDesktop>) {
          start_desktop(e.session_id, e.render_node);
        } else if constexpr (std::is_same_v<T, StartRunner>) {
          start_runner(e.session_id, e.render_node);
        } else if constexpr (std::is_same_v<T, StartStreaming>) {
          start_streaming(e.session_id, e.client_ip, e.client_port);
        } else if constexpr (std::is_same_v<T, PauseStreaming>) {
          forward_pause(e.session_id);
        } else if constexpr (std::is_same_v<T, ResumeStreaming>) {
          forward_resume(e.session_id);
        } else if constexpr (std::is_same_v<T, RequestIdr>) {
          forward_idr(e.session_id);
        } else if constexpr (std::is_same_v<T, Teardown>) {
          teardown(e.session_id, e.reason);
        } else if constexpr (std::is_same_v<T, ReleaseGpu>) {
          release_gpu(e.session_id);
        } else {
          // PlugDevice / UnplugDevice are part of the state machine's vocabulary but are not yet
          // driven by any input: device hotplug still goes straight through the event bus, where
          // the Docker runner consumes it.
          logs::log(logs::debug, "[SESSION] Effect {} is not yet driven by the actor", typeid(T).name());
        }
      },
      effect);
}

/*
 * The streaming pipelines subscribe to these events on the bus. The actor is the single entry
 * point for the lifecycle, so it forwards the corresponding effects onto the bus rather than the
 * protocol adapters firing events directly. This keeps the pipelines unchanged while giving the
 * session one owner.
 */
void MoonlightSessionRuntime::forward_pause(std::uint64_t session_id) {
  context_.stream_session->event_bus->fire_event(
      immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
}

void MoonlightSessionRuntime::forward_resume(std::uint64_t session_id) {
  context_.stream_session->event_bus->fire_event(
      immer::box<events::ResumeStreamEvent>(events::ResumeStreamEvent{.session_id = session_id}));
}

void MoonlightSessionRuntime::forward_idr(std::uint64_t session_id) {
  context_.stream_session->event_bus->fire_event(
      immer::box<events::IDRRequestEvent>(events::IDRRequestEvent{.session_id = session_id}));
}

void MoonlightSessionRuntime::adopt_session(std::uint64_t session_id) {
  auto session = context_.stream_session;

  // The protocol adapters (REST `/launch`, the wolf-ui API) register the session before they reply
  // to the client, because the RTSP handshake and the control channel look it up immediately. The
  // actor adopts the session as the first step of its lifecycle, so it (re-)asserts that entry:
  // `add_session` upserts by `session_id` (which is derived from the client), so re-registering is
  // idempotent and never leaves a duplicate for `/sessions`, `/cancel` or the RTP-ping fan-out.
  context_.app_state->running_sessions->update(
      [session](const immer::vector<events::StreamSession> &sessions) {
        return state::add_session(sessions, *session);
      });

  logs::log(logs::debug, "[SESSION] Adopted session {}", session_id);
}

void MoonlightSessionRuntime::assign_gpu(std::uint64_t session_id) {
  auto session = context_.stream_session;
  auto gpu_balancer = context_.app_state->gpu_balancer;

  // Pick, probe and reserve in one atomic step. Picking without reserving would let two sessions
  // starting at the same instant read the same free GPU; probing afterwards (as a separate step)
  // would leave a dead node looking perpetually idle and therefore winning every subsequent pick.
  auto chosen = state::pick_and_acquire(gpu_balancer, [](const state::GpuBalancer &bal, const std::vector<std::string> &avoid) {
    return bal.pick_avoiding(avoid);
  });

  if (!chosen.has_value()) {
    logs::log(logs::error, "[SESSION] No available GPU for session {}", session_id);
    post(StopRequested{.reason = "no available GPU"});
    return;
  }

  logs::log(logs::info, "[SESSION] Assigned GPU {} to session {}", *chosen, session_id);

  // Record the assignment on the session so the runner and teardown can see it.
  session->assigned_render_node = *chosen;
  context_.app_state->running_sessions->update(
      [node = *chosen, id = session_id](const immer::vector<events::StreamSession> &sessions) {
        auto v = sessions.transient();
        for (std::size_t i = 0; i < v.size(); ++i) {
          if (v[i].session_id == id) {
            auto updated = v[i];
            updated.assigned_render_node = node;
            v.set(i, updated);
          }
        }
        return v.persistent();
      });

  post(GpuAssigned{.render_node = *chosen});
}

void MoonlightSessionRuntime::start_desktop(std::uint64_t session_id, const std::string &render_node) {
  auto session = context_.stream_session;

  // Register this session's device queue so hotplug events have somewhere to go.
  auto devices_q = std::make_shared<events::devices_atom_queue>();
  context_.plugged_devices_queue->update(
      [id = std::to_string(session_id), devices_q](const session_devices map) { return map.set(id, devices_q); });

  if (!session->app->start_virtual_compositor) {
    // No compositor: create the virtual input devices directly and report the desktop ready.
    auto mouse = input::Mouse::create();
    if (!mouse) {
      logs::log(logs::error, "Failed to create mouse: {}", mouse.getErrorMessage());
    } else {
      auto mouse_ptr = input::Mouse(std::move(*mouse));
      devices_q->push(immer::box<events::PlugDeviceEvent>(
          events::PlugDeviceEvent{.session_id = std::to_string(session_id),
                                  .udev_events = mouse_ptr.get_udev_events(),
                                  .udev_hw_db_entries = mouse_ptr.get_udev_hw_db_entries()}));
      session->mouse->emplace(std::move(mouse_ptr));
    }

    auto keyboard = input::Keyboard::create();
    if (!keyboard) {
      logs::log(logs::error, "Failed to create keyboard: {}", keyboard.getErrorMessage());
    } else {
      auto keyboard_ptr = input::Keyboard(std::move(*keyboard));
      devices_q->push(immer::box<events::PlugDeviceEvent>(
          events::PlugDeviceEvent{.session_id = std::to_string(session_id),
                                  .udev_events = keyboard_ptr.get_udev_events(),
                                  .udev_hw_db_entries = keyboard_ptr.get_udev_hw_db_entries()}));
      session->keyboard->emplace(std::move(keyboard_ptr));
    }
    post(DesktopReady{.wayland_socket_name = ""});
    return;
  }

  auto on_ready = std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();
  auto gst_contexts = context_.app_state->gst_contexts;
  auto event_bus = session->event_bus;
  auto runtime_dir = context_.runtime_dir;
  auto display_mode = session->display_mode;
  auto buffer_caps = session->app->video_producer_buffer_caps;

  // `start_video_producer` calls `run_pipeline`, which *blocks* until the pipeline reaches EOS or
  // errors out, and it fulfils `on_ready` from the pipeline's own GStreamer bus thread when the
  // Wayland socket is up. The producer and the readiness-waiter therefore have to be two separate
  // threads: waiting on the future from the thread that is running the pipeline would deadlock
  // before the pipeline ever signalled readiness (no `DesktopReady`, so the runner never starts,
  // and no compositor wired into the session, so no virtual input devices exist).
  //
  // The waiter holds a shared_ptr to this runtime so it stays alive even if the actor (and its
  // owning session) is torn down first; `post()` then safely drops the completion.
  std::thread([self = shared_from_this(), session, on_ready, runtime_dir]() {
    auto ready = on_ready->get_future().get();

    // Wire the compositor up as the session's virtual display and input devices.
    auto wl_state = virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
    session->wayland_display->store(wl_state);
    session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
    session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
    session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

    if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name)) {
      self->post(DesktopFailed{.reason = "wayland socket was not ready"});
      return;
    }
    self->post(DesktopReady{.wayland_socket_name = ready.wayland_socket_name});
  }).detach();

  // This thread blocks for the lifetime of the compositor: `run_pipeline` runs a GLib main loop
  // until the pipeline reaches EOS (StopStreamEvent) or errors out.
  std::thread([session_id, render_node, buffer_caps, display_mode, gst_contexts, on_ready, event_bus]() {
    streaming::start_video_producer(std::to_string(session_id),
                                    buffer_caps,
                                    render_node,
                                    {.width = display_mode.width,
                                     .height = display_mode.height,
                                     .refreshRate = display_mode.refreshRate},
                                    gst_contexts,
                                    on_ready,
                                    event_bus);
  }).detach();
}

void MoonlightSessionRuntime::start_runner(std::uint64_t session_id, const std::string &render_node) {
  auto session = context_.stream_session;
  auto app_state = context_.app_state;
  auto audio_server = context_.audio_server;
  auto runtime_dir = context_.runtime_dir;

  // Create the audio virtual sink and start the audio producer before the runner comes up.
  if (session->app->start_audio_server && audio_server && audio_server->server) {
    auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, session_id);
    auto v_device = audio::create_virtual_sink(
        audio_server->server,
        audio::AudioDevice{.sink_name = pulse_sink_name,
                           .mode = state::get_audio_mode(session->audio_channel_count, true)});
    session->audio_sink->store(v_device);

    std::thread([session, audio_server = audio_server->server, session_id]() {
      auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, session_id);
      streaming::start_audio_producer(std::to_string(session_id),
                                      session->event_bus,
                                      session->audio_channel_count,
                                      sink_name,
                                      audio::get_server_name(audio_server));
    }).detach();
  }

  auto devices_q = context_.plugged_devices_queue->load()->find(std::to_string(session_id));
  if (!devices_q) {
    logs::log(logs::warning, "[SESSION] No devices queue found for session {}", session_id);
    post(StopRequested{.reason = "no devices queue"});
    return;
  }

  // The runner blocks for the container's lifetime, so run it off the actor's thread. The thread
  // holds a shared_ptr to this runtime so it stays alive even if the actor is torn down first.
  std::thread([self = shared_from_this(), session, app_state, audio_server, runtime_dir, session_id, render_node, devices_q]() {
    self->post(RunnerStarted{});

    // Qualify the free function: the member `start_runner` would otherwise shadow it.
    wolf::core::sessions::start_runner(session->app->runner,
                                       *devices_q,
                                       immer::box<RunnerArgs>{RunnerArgs{
                                           .session_id = std::to_string(session_id),
                                           .video_settings =
                                               {
                                                   .width = session->display_mode.width,
                                                   .height = session->display_mode.height,
                                                   .refresh_rate = session->display_mode.refreshRate,
                                                   .wayland_render_node = render_node,
                                                   .runner_render_node = render_node,
                                                   .video_producer_buffer_caps = session->app->video_producer_buffer_caps,
                                               },
                                           .wayland_display = session->wayland_display->load(),
                                           .audio_server = audio_server,
                                           .audio_sink = session->audio_sink->load(),
                                           .host = app_state->host,
                                           .app_local_state_folder = session->app_local_state_folder,
                                           .app_host_state_folder = session->app_host_state_folder,
                                           .xdg_runtime_dir = runtime_dir,
                                           .client_settings = session->client_settings}});

    // The runner process ended.
    self->post(RunnerExited{});
  }).detach();
}

void MoonlightSessionRuntime::start_streaming(std::uint64_t session_id,
                                              const std::string &client_ip,
                                              std::uint16_t client_port) {
  // Streaming pipelines are started by the existing VideoSession/AudioSession event handlers once
  // the RTSP handshake completes; the actor only records that the session is live.
  logs::log(logs::debug, "[SESSION] Session {} streaming to {}:{}", session_id, client_ip, client_port);
}

void MoonlightSessionRuntime::teardown(std::uint64_t session_id, const std::string &reason) {
  logs::log(logs::debug, "[SESSION] Tearing down session {}: {}", session_id, reason);

  auto app_state = context_.app_state;
  auto session = context_.stream_session;

  // Drop the Wayland display so the compositor is destroyed.
  session->wayland_display->store(nullptr);

  // Remove the session from app state so the app list is updated.
  app_state->running_sessions->update([session_id](const immer::vector<events::StreamSession> &sessions) {
    return state::remove_session(sessions, {.session_id = session_id});
  });

  // Drop this session's device queue.
  context_.plugged_devices_queue->update(
      [id = std::to_string(session_id)](const session_devices map) { return map.erase(id); });

  post(TeardownComplete{});
}

void MoonlightSessionRuntime::release_gpu(std::uint64_t session_id) {
  auto session = context_.stream_session;
  auto node = session->assigned_render_node;
  if (node.empty()) {
    return;
  }
  context_.app_state->gpu_balancer->update(
      [node](const state::GpuBalancer &bal) { return bal.release(node); });
}

} // namespace wolf::core::sessions
