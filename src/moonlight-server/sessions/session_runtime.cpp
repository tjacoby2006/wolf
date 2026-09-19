#include <sessions/session_runtime.hpp>

#include <helpers/logger.hpp>
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
        if constexpr (std::is_same_v<T, AssignGpu>) {
          assign_gpu(e.session_id);
        } else if constexpr (std::is_same_v<T, StartDesktop>) {
          start_desktop(e.session_id, e.render_node);
        } else if constexpr (std::is_same_v<T, StartRunner>) {
          start_runner(e.session_id, e.render_node);
        } else if constexpr (std::is_same_v<T, StartStreaming>) {
          start_streaming(e.session_id, e.client_ip, e.client_port);
        } else if constexpr (std::is_same_v<T, Teardown>) {
          teardown(e.session_id, e.reason);
        } else if constexpr (std::is_same_v<T, ReleaseGpu>) {
          release_gpu(e.session_id);
        } else {
          // PauseStreaming / ResumeStreaming / RequestIdr / PlugDevice / UnplugDevice are handled
          // by the streaming pipelines' own event-bus handlers for now; the actor still owns the
          // lifecycle, these are just forwarded.
          logs::log(logs::debug, "[SESSION] Effect {} not yet wired to the runtime", typeid(T).name());
        }
      },
      effect);
}

void MoonlightSessionRuntime::assign_gpu(std::uint64_t session_id) {
  auto session = context_.stream_session;
  auto gpu_balancer = context_.app_state->gpu_balancer;

  auto chosen = gpu_balancer->load()->pick(session->app->gpu_pin);
  logs::log(logs::info,
            "[SESSION] Picked GPU {} for session {} (pin={})",
            chosen.has_value() ? *chosen : "<none>",
            session_id,
            session->app->gpu_pin.has_value() ? *session->app->gpu_pin : "<none>");

  // The pool is discovered at startup and can go stale (GPU reset, driver reload, ...).
  // Handing a dead node to the virtual compositor makes it panic and abort Wolf, so probe first.
  if (chosen.has_value() && !is_render_node_available(*chosen)) {
    logs::log(logs::error, "[SESSION] Assigned GPU {} is not available for session {}", *chosen, session_id);
    gpu_balancer->update([node = *chosen](const state::GpuBalancer &bal) { return bal.release(node); });
    chosen.reset();
  }

  if (!chosen.has_value()) {
    logs::log(logs::error, "[SESSION] No available GPU for session {}", session_id);
    post(StopRequested{.reason = "no available GPU"});
    return;
  }

  gpu_balancer->update([node = *chosen](const state::GpuBalancer &bal) { return bal.acquire(node); });
  post(GpuAssigned{.render_node = *chosen});
}

void MoonlightSessionRuntime::start_desktop(std::uint64_t session_id, const std::string &render_node) {
  auto session = context_.stream_session;

  if (!session->app->start_virtual_compositor) {
    // No compositor: the desktop is trivially "ready" (virtual devices are created elsewhere).
    post(DesktopReady{.wayland_socket_name = ""});
    return;
  }

  auto on_ready = std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();
  auto gst_contexts = context_.app_state->gst_contexts;
  auto event_bus = session->event_bus;
  auto runtime_dir = context_.runtime_dir;
  auto display_mode = session->display_mode;
  auto buffer_caps = session->app->video_producer_buffer_caps;

  // The compositor startup blocks, so run it off the actor's thread.
  std::thread([this, session_id, render_node, buffer_caps, display_mode, gst_contexts, on_ready, event_bus, runtime_dir]() {
    streaming::start_video_producer(std::to_string(session_id),
                                    buffer_caps,
                                    render_node,
                                    {.width = display_mode.width,
                                     .height = display_mode.height,
                                     .refreshRate = display_mode.refreshRate},
                                    gst_contexts,
                                    on_ready,
                                    event_bus);

    auto ready = on_ready->get_future().get();
    if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name)) {
      post(DesktopFailed{.reason = "wayland socket was not ready"});
      return;
    }
    post(DesktopReady{.wayland_socket_name = ready.wayland_socket_name});
  }).detach();
}

void MoonlightSessionRuntime::start_runner(std::uint64_t session_id, const std::string &render_node) {
  auto session = context_.stream_session;
  auto app_state = context_.app_state;
  auto audio_server = context_.audio_server;
  auto runtime_dir = context_.runtime_dir;

  // The runner blocks for the container's lifetime, so run it off the actor's thread.
  std::thread([this, session, app_state, audio_server, runtime_dir, session_id, render_node]() {
    auto devices_q = std::make_shared<events::devices_atom_queue>();
    post(RunnerStarted{});

    // Qualify the free function: the member `start_runner` would otherwise shadow it.
    wolf::core::sessions::start_runner(session->app->runner,
                                       devices_q,
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
    post(RunnerExited{});
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
