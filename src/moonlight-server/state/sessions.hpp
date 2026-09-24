#pragma once

#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <immer/vector.hpp>
#include <optional>
#include <range/v3/view.hpp>
#include <utility>
#include <state/config.hpp>
#include <state/serialised_config.hpp>

namespace state {

using namespace wolf::core;

inline std::optional<events::StreamSession> get_session_by_id(const immer::vector<events::StreamSession> &sessions,
                                                              const std::size_t id) {
  auto results =
      sessions |                                                                                             //
      ranges::views::filter([id](const events::StreamSession &session) { return session.session_id == id; }) //
      | ranges::views::take(1)                                                                               //
      | ranges::to_vector;                                                                                   //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple sessions for a given ID: {}", id);
    return {};
  }
}

inline std::optional<events::StreamSession> get_session_by_client(const immer::vector<events::StreamSession> &sessions,
                                                                  const wolf::config::PairedClient &client) {
  auto client_id = get_client_id(client);
  return get_session_by_id(sessions, client_id);
}

/**
 * Resolve the session that is driving a lobby when the caller did not say which one it is.
 *
 * A lobby's frames are consumed by the encoder of the session that created it, and that encoder's GPU is
 * frozen at RTSP PLAY. Clients such as wolf-ui create a lobby from inside a *launcher session* (the one
 * running the virtual compositor, i.e. `start_virtual_compositor`), so that session's node is the one the
 * lobby must render on. We can only do this unambiguously when exactly one such session is running:
 * otherwise the request is genuinely ambiguous and we return `nullopt` so the caller falls back to
 * normal load balancing. An explicit `session_id` always takes precedence over this heuristic.
 */
inline std::optional<std::size_t> get_launcher_session_id(const immer::vector<events::StreamSession> &sessions) {
  std::optional<std::size_t> launcher;
  for (const events::StreamSession &session : sessions) {
    if (session.app && session.app->start_virtual_compositor) {
      if (launcher.has_value()) {
        // More than one launcher session: don't guess.
        return std::nullopt;
      }
      launcher = session.session_id;
    }
  }
  return launcher;
}

inline std::optional<events::Lobby> get_lobby_by_id(const immer::vector<events::Lobby> &lobbies,
                                                    std::string_view lobby_id) {
  auto results = lobbies |                                                                                      //
                 ranges::views::filter([lobby_id](const events::Lobby &lobby) { return lobby.id == lobby_id; }) //
                 | ranges::views::take(1)                                                                       //
                 | ranges::to_vector;                                                                           //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple lobbies for a given ID: {}", lobby_id);
    return {};
  }
}

inline std::optional<events::Lobby> get_lobby_by_connected_session(const immer::vector<events::Lobby> &lobbies,
                                                                   std::string_view session_id) {
  for (const events::Lobby &lobby : lobbies) {
    immer::vector<immer::box<std::string>> sessions = lobby.connected_sessions->load();
    auto session = std::find_if(sessions.begin(), sessions.end(), [session_id](const auto &session) {
      return session == session_id;
    });
    if (session == sessions.end()) {
      continue;
    }
    return lobby;
  }
  return {};
}

inline std::shared_ptr<events::StreamSession> create_stream_session(immer::box<state::AppState> state,
                                                                    const events::App &run_app,
                                                                    const wolf::config::PairedClient &current_client,
                                                                    const moonlight::DisplayMode &display_mode,
                                                                    int audio_channel_count,
                                                                    const std::string &aes_key,
                                                                    const std::string &aes_iv) {
  auto full_path = std::filesystem::path(state->host->local_base_state_folder) / current_client.app_state_folder /
                   run_app.base.title;
  logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
  std::filesystem::create_directories(full_path);

  std::random_device rd;
  std::mt19937 generator(rd());

  std::uniform_int_distribution<> chars(33, 126); // ASCII values for printable character
  std::array<char, 16> rtp_secret_payload;
  for (auto &c : rtp_secret_payload) {
    c = static_cast<char>(chars(generator));
  }

  std::uniform_int_distribution<u_int32_t> uints(0, UINT32_MAX);

  std::uniform_int_distribution<> ints(0, 255);
  auto rtsp_fake_ip = fmt::format("{}.{}.{}.{}", ints(generator), ints(generator), ints(generator), ints(generator));

  auto session = events::StreamSession{
      .display_mode = display_mode,
      .audio_channel_count = audio_channel_count,
      .event_bus = state->event_bus,
      .client_settings = current_client.settings,
      .app = std::make_shared<events::App>(run_app),
      .app_local_state_folder = full_path.string(),
      .app_host_state_folder = std::filesystem::path(state->host->host_base_state_folder) /
                               current_client.app_state_folder / run_app.base.title,

      .aes_key = aes_key,
      .aes_iv = aes_iv,

      // Moonlight protocol extension to support IP-less connections
      .rtp_secret_payload = rtp_secret_payload,
      .enet_secret_payload = uints(generator),
      .rtsp_fake_ip = rtsp_fake_ip,

      // client info
      .session_id = get_client_id(current_client),
      .video_stream_port = static_cast<unsigned short>(get_port(VIDEO_PING_PORT)),
      .audio_stream_port = static_cast<unsigned short>(get_port(AUDIO_PING_PORT)),
      .control_stream_port = static_cast<unsigned short>(get_port(CONTROL_PORT))};

  return std::make_shared<events::StreamSession>(session);
}

inline immer::vector<events::StreamSession> remove_session(const immer::vector<events::StreamSession> &sessions,
                                                           const events::StreamSession &session) {
  return sessions                                                                                           //
         | ranges::views::filter([remove_hash = session.session_id](const events::StreamSession &cur_ses) { //
             return cur_ses.session_id != remove_hash;                                                      //
           })                                                                                               //
         | ranges::to<immer::vector<events::StreamSession>>();                                              //
}

/**
 * Insert a session into the registry, replacing any existing entry for the same `session_id`.
 *
 * `session_id` is derived from the client, so a relaunch/resume of the same client reuses it.
 * Appending blindly would leave two entries for one client, which made `/sessions` list the
 * session twice, made `/cancel` remove only one copy, and made the RTP-ping fan-out post the
 * same input to every duplicate. Upserting keeps "one client, one session" true whether the
 * caller is a protocol adapter (before the client is told the launch succeeded) or the session's
 * own actor (which owns the lifecycle).
 */
inline immer::vector<events::StreamSession> add_session(const immer::vector<events::StreamSession> &sessions,
                                                        const events::StreamSession &session) {
  return remove_session(sessions, session).push_back(session);
}

/**
 * Carry the state that belongs to the *client's stream* — rather than to a single RTSP session —
 * from the session being replaced onto the fresh one a resume builds.
 *
 * A resume rebuilds the session from scratch (new RTP secrets, new ports), but the GPU the load
 * balancer assigned, the compositor and the virtual input devices all live as long as the stream and
 * must survive the swap. Forgetting `assigned_render_node` is the dangerous one: the encoder is
 * re-pointed at the app's default node on the next RTSP PLAY while the compositor keeps rendering on
 * the assigned node, so nvenc is handed `CUDAMemory` from a GPU it cannot address. That doesn't just
 * black out the session — it corrupts the process-wide CUDA context and cascades
 * `CUDA_ERROR_UNKNOWN` / `CUDA_ERROR_ILLEGAL_ADDRESS` (and Rust compositor panics) into every other
 * running session.
 */
inline void carry_over_resumed_session(events::StreamSession &from, events::StreamSession &to) {
  // The GPU assignment lives with the stream, not the RTSP session: the compositor keeps rendering
  // there across a pause/resume, so the encoder must follow it.
  to.assigned_render_node = from.assigned_render_node;
  // The compositor and the virtual input devices are already wired into the running container;
  // reusing them is the whole point of resume.
  to.wayland_display = std::move(from.wayland_display);
  to.mouse = std::move(from.mouse);
  to.keyboard = std::move(from.keyboard);
  to.joypads = std::move(from.joypads);
  to.pen_tablet = std::move(from.pen_tablet);
  to.touch_screen = std::move(from.touch_screen);
  // The PulseAudio virtual sink is created once with the runner and removed when the session ends.
  // Dropping it here would leak the sink, since teardown deletes it through this handle.
  to.audio_sink = std::move(from.audio_sink);
}

} // namespace state