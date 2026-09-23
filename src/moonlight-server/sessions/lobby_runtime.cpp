#include <sessions/lobby_runtime.hpp>

#include <core/audio.hpp>
#include <core/virtual-display.hpp>
#include <helpers/logger.hpp>
#include <immer/vector_transient.hpp>
#include <platforms/hw.hpp>
#include <range/v3/view.hpp>
#include <sessions/common.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

using namespace wolf::session;

MoonlightLobbyRuntime::MoonlightLobbyRuntime(LobbyContext context) : context_(std::move(context)) {}

void MoonlightLobbyRuntime::execute(const LobbyEffect &effect) {
  std::visit(
      [this](const auto &e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, AssignLobbyGpu>) {
          assign_gpu();
        } else if constexpr (std::is_same_v<T, StartLobbyDesktop>) {
          start_desktop(e.render_node);
        } else if constexpr (std::is_same_v<T, StartLobbyRunner>) {
          start_runner(e.render_node);
        } else if constexpr (std::is_same_v<T, AttachSession>) {
          attach_session(e.session_id);
        } else if constexpr (std::is_same_v<T, DetachSession>) {
          detach_session(e.session_id);
        } else if constexpr (std::is_same_v<T, TeardownLobby>) {
          teardown(e.reason);
        } else if constexpr (std::is_same_v<T, ReleaseLobbyGpu>) {
          release_gpu();
        }
      },
      effect);
}

void MoonlightLobbyRuntime::assign_gpu() {
  auto lobby = context_.lobby;
  auto gpu_balancer = context_.app_state->gpu_balancer;

  // The lobby's frames are consumed by the encoder of the session that created it, and that encoder's
  // GPU is fixed when the session goes to RTSP PLAY. Rendering on a different node would hand the
  // encoder GPU memory it can't address, so inherit the creating session's node when we have it and
  // only load-balance as a fallback (`pick` returns nullopt when a pinned node isn't in the pool).
  auto preferred = context_.settings->preferred_render_node;

  // Pick, probe and reserve in one atomic step, so a stale node is skipped and never left looking
  // idle (which would make it win every later pick).
  auto chosen = state::pick_and_acquire(
      gpu_balancer,
      [&preferred](const state::GpuBalancer &bal, const std::vector<std::string> &avoid) {
        return bal.pick_preferring(preferred, avoid);
      });

  if (!chosen.has_value()) {
    logs::log(logs::error, "[LOBBY] No available GPU for lobby {}", lobby->id);
    post(StopLobby{.reason = "no available GPU"});
    return;
  }

  logs::log(logs::info,
            "[LOBBY] Assigned GPU {} to lobby {} (preferred={})",
            *chosen,
            lobby->id,
            preferred.has_value() ? *preferred : "<none>");

  lobby->assigned_render_node = *chosen;
  post(LobbyGpuAssigned{.render_node = *chosen});
}

void MoonlightLobbyRuntime::start_desktop(const std::string &render_node) {
  auto lobby = context_.lobby;
  auto settings = context_.settings;
  auto app_state = context_.app_state;
  auto runtime_dir = context_.runtime_dir;

  auto on_ready = std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();
  auto gst_contexts = app_state->gst_contexts;
  auto event_bus = app_state->event_bus;
  auto video_settings = settings->video_settings;

  // `start_video_producer` calls `run_pipeline`, which *blocks* until the pipeline reaches EOS or
  // errors out, and it fulfils `on_ready` from the pipeline's own GStreamer bus thread when the
  // Wayland socket is up. The producer and the readiness-waiter therefore have to be two separate
  // threads: waiting on the future from the thread that is running the pipeline would deadlock
  // before the pipeline ever signalled readiness (no `LobbyDesktopReady`, so the runner never
  // starts, and no compositor wired into the lobby).
  //
  // The waiter holds a shared_ptr to this runtime so it stays alive even if the actor is torn down
  // first.
  std::thread([self = shared_from_this(), lobby, on_ready, runtime_dir]() {
    auto ready = on_ready->get_future().get();

    auto wl_state = virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
    lobby->wayland_display->store(wl_state);

    if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name)) {
      self->post(LobbyDesktopFailed{.reason = "wayland socket was not ready"});
      return;
    }
    self->post(LobbyDesktopReady{.wayland_socket_name = ready.wayland_socket_name});
  }).detach();

  // This thread blocks for the lifetime of the compositor: `run_pipeline` runs a GLib main loop
  // until the pipeline reaches EOS (StopLobbyEvent) or errors out.
  std::thread([lobby, video_settings, render_node, gst_contexts, on_ready, event_bus]() {
    streaming::start_video_producer(lobby->id,
                                    video_settings.video_producer_buffer_caps,
                                    render_node,
                                    {.width = video_settings.width,
                                     .height = video_settings.height,
                                     .refreshRate = video_settings.refresh_rate},
                                    gst_contexts,
                                    on_ready,
                                    event_bus);
  }).detach();
}

void MoonlightLobbyRuntime::start_runner(const std::string &render_node) {
  auto lobby = context_.lobby;
  auto settings = context_.settings;
  auto app_state = context_.app_state;
  auto audio_server = context_.audio_server;
  auto runtime_dir = context_.runtime_dir;

  // Create the shared audio virtual sink and start the audio producer before the runner comes up.
  if (audio_server && audio_server->server) {
    auto channel_count = settings->audio_settings.channel_count;
    auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, lobby->id);
    auto v_device = audio::create_virtual_sink(
        audio_server->server,
        audio::AudioDevice{.sink_name = pulse_sink_name, .mode = state::get_audio_mode(channel_count, true)});
    lobby->audio_sink->store(v_device);

    std::thread([lobby, audio_server = audio_server->server, channel_count, event_bus = app_state->event_bus]() {
      auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, lobby->id);
      streaming::start_audio_producer(lobby->id,
                                      event_bus,
                                      channel_count,
                                      sink_name,
                                      audio::get_server_name(audio_server));
    }).detach();
  }

  // The runner blocks for the container's lifetime, so run it off the actor's thread.
  std::thread([self = shared_from_this(), lobby, settings, app_state, audio_server, runtime_dir, render_node]() {
    auto host = app_state->host;
    auto full_path = std::filesystem::path(host->local_base_state_folder) / settings->runner_state_folder;
    std::filesystem::create_directories(full_path);

    self->post(LobbyRunnerStarted{});

    // The lobby is now up: fulfil the promise the API waits on so lobby creation returns.
    // `on_setup_over` is an rfl::Skip wrapper, so unwrap it with `.get()`.
    if (settings->on_setup_over.get()) {
      settings->on_setup_over.get()->set_value(true);
    }

    wolf::core::sessions::start_runner(
        lobby->runner,
        lobby->plugged_devices_queue,
        immer::box<RunnerArgs>{RunnerArgs{
            .session_id = lobby->id,
            .video_settings = events::VideoSettings{.width = settings->video_settings.width,
                                                    .height = settings->video_settings.height,
                                                    .refresh_rate = settings->video_settings.refresh_rate,
                                                    .wayland_render_node = render_node,
                                                    .runner_render_node = render_node,
                                                    .video_producer_buffer_caps =
                                                        settings->video_settings.video_producer_buffer_caps},
            .wayland_display = lobby->wayland_display->load(),
            .audio_server = audio_server,
            .audio_sink = lobby->audio_sink->load(),
            .host = host,
            .app_local_state_folder = full_path.string(),
            .app_host_state_folder =
                std::filesystem::path(host->host_base_state_folder) / settings->runner_state_folder,
            .xdg_runtime_dir = runtime_dir,
            .client_settings = settings->client_settings}});

    // The runner process ended.
    self->post(LobbyRunnerExited{});
  }).detach();
}

void MoonlightLobbyRuntime::attach_session(std::uint64_t session_id) {
  auto lobby = context_.lobby;
  auto event_bus = context_.app_state->event_bus;

  auto sessions = context_.app_state->running_sessions->load();
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    logs::log(logs::warning, "[LOBBY] Cannot attach unknown session {} to lobby {}", session_id, lobby->id);
    return;
  }

  // Migrate the session's joypads into the lobby's runner. This must happen before the session is
  // recorded as connected, or the relayed unplug races the queued plug.
  events::JoypadList joypads = session->joypads->load();
  for (auto [_joypad_nr, joypad] : joypads) {
    events::PlugDeviceEvent plug_ev{.session_id = lobby->id};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    // Unplug it from the session's own runner.
    event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
        events::UnplugDeviceEvent{.session_id = std::to_string(session_id),
                                  .udev_events = plug_ev.udev_events,
                                  .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
    // Add it to the lobby runner's device queue.
    lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{plug_ev});
  }
  // TODO: hotplug pen_tablet

  // Record the session on the lobby record: other code (e.g. the device relay) reads this atom.
  lobby->connected_sessions->update([session_id](const immer::vector<immer::box<std::string>> &connected) {
    return connected.push_back({std::to_string(session_id)});
  });

  // Switch mouse/keyboard/touch to the lobby's wayland server.
  auto wl_state = lobby->wayland_display->load();
  session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
  session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
  session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

  // Switch the session's audio/video producers to the lobby's shared desktop.
  event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
      events::SwitchStreamProducerEvents{.session_id = session_id, .interpipe_src_id = lobby->id}});
}

void MoonlightLobbyRuntime::detach_session(std::uint64_t session_id) {
  auto lobby = context_.lobby;
  auto event_bus = context_.app_state->event_bus;

  auto sessions = context_.app_state->running_sessions->load();
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    logs::log(logs::warning, "[LOBBY] Cannot detach unknown session {} from lobby {}", session_id, lobby->id);
    return;
  }

  // Drop the session from the lobby record.
  lobby->connected_sessions->update([session_id](const immer::vector<immer::box<std::string>> &connected) {
    return connected | //
           ranges::views::filter([session_id](const immer::box<std::string> &id) {
             return *id != std::to_string(session_id);
           }) | //
           ranges::to<immer::vector<immer::box<std::string>>>();
  });

  // Switch mouse/keyboard/touch back to the session's own wayland server.
  auto wl_state = session->wayland_display->load();
  session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
  session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
  session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

  // Migrate the session's joypads back out of the lobby's runner.
  events::JoypadList joypads = session->joypads->load();
  for (auto [_joypad_nr, joypad] : joypads) {
    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session_id)};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    // Plug them back into the session's own runner.
    event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    // Unplug them from the lobby's runner.
    event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
        events::UnplugDeviceEvent{.session_id = lobby->id,
                                  .udev_events = plug_ev.udev_events,
                                  .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
  }
  // TODO: hotplug pen_tablet and touch_screen

  // Switch the session's audio/video producers back to its own desktop.
  event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
      events::SwitchStreamProducerEvents{.session_id = session_id, .interpipe_src_id = std::to_string(session_id)}});
}

void MoonlightLobbyRuntime::teardown(const std::string &reason) {
  auto lobby = context_.lobby;
  logs::log(logs::debug, "[LOBBY] Tearing down lobby {}: {}", lobby->id, reason);

  // Stop the lobby's own pipelines and runner *before* dropping the compositor.
  //
  // The lobby's video/audio producers are registered under the lobby id (`<lobby_id>_video` and
  // `<lobby_id>_audio`), and they only ever listen for `StopLobbyEvent` (the other handler,
  // `StopStreamEvent`, carries a *session* id and can never match). Dropping `wayland_display`
  // destroys the compositor behind `waylanddisplaysrc`, but without an EOS the pipeline itself — and
  // the GPU render node it holds — keeps running for the rest of the process lifetime. The same event
  // also reaches the runner's stop handler, releasing the container/process (and its GPU memory).
  //
  // The fire is guarded on the lobby still being published, which is exactly the condition "this
  // teardown was started from inside Wolf rather than by a `StopLobbyEvent`". `wolf.cpp`'s
  // `stop_all_sessions` fires `StopLobbyEvent` for every lobby at shutdown, and the bus routes that
  // straight back here as a `StopLobby`; an unguarded fire would re-enter teardown and emit a second
  // `ReleaseLobbyGpu`, double-releasing the render node from the balancer.
  auto lobbies = context_.app_state->lobbies->load();
  if (state::get_lobby_by_id(lobbies.get(), lobby->id).has_value()) {
    context_.app_state->event_bus->fire_event(
        immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
  }

  // Drop the shared wayland display so the compositor is destroyed.
  lobby->wayland_display->store(nullptr);

  // Remove the lobby from app state.
  context_.app_state->lobbies->update([id = lobby->id](const immer::vector<events::Lobby> &lobbies) {
    return lobbies | //
           ranges::views::filter([id](const events::Lobby &lobby) { return lobby.id != id; }) | //
           ranges::to<immer::vector<events::Lobby>>();
  });

  post(LobbyTeardownComplete{});
}

void MoonlightLobbyRuntime::release_gpu() {
  auto lobby = context_.lobby;
  if (lobby->assigned_render_node.empty()) {
    return;
  }
  context_.app_state->gpu_balancer->update(
      [node = lobby->assigned_render_node](const state::GpuBalancer &bal) { return bal.release(node); });
}

} // namespace wolf::core::sessions