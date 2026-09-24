#pragma once

#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <immer/vector.hpp>
#include <optional>
#include <range/v3/view.hpp>
#include <state/config.hpp>
#include <state/gpu.hpp>
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

/**
 * Picks a GPU from the pool using the load balancer and increments its assignment counter.
 * Returns the render node that the caller should pin the session to.
 *
 * The pick happens inside the atom's update function so that the assignment is atomic: immer may
 * retry `update` under contention, and the value captured from the final invocation is the one that
 * corresponds to the stored state.
 */
inline std::string acquire_gpu(immer::box<state::AppState> state) {
  const auto &pool = state->config->gpus;
  std::string chosen;
  int weight = 1;

  state->gpu_assignments->update([&](const GpuAssignments &assignments) {
    auto gpu = pick_gpu(pool, assignments);
    if (gpu == nullptr) {
      return assignments;
    }
    chosen = gpu->render_node;
    weight = gpu->weight;
    return state::assign_gpu(assignments, chosen);
  });

  if (chosen.empty()) {
    logs::log(logs::warning, "No GPU available in the pool, the session won't be pinned to a specific device");
    return chosen;
  }
  logs::log(logs::info, "Assigned GPU {} (weight {}) to new session", chosen, weight);
  return chosen;
}

/**
 * Releases a GPU when a session ends, decrementing its assignment counter.
 */
inline void release_session_gpu(immer::box<state::AppState> state, const std::string &render_node) {
  state->gpu_assignments->update(
      [&render_node](const GpuAssignments &assignments) { return state::release_gpu(assignments, render_node); });
}

/**
 * Assigns a GPU to an app and re-resolves its video pipelines, buffer caps and codec support
 * against the assigned GPU's defaults. Returns the chosen render node.
 */
inline std::string assign_gpu_to_app(immer::box<state::AppState> state, events::App &app) {
  auto render_node = acquire_gpu(state);
  auto gpu = state->config->gpus.find(render_node);
  if (gpu == nullptr) {
    // No GPU could be assigned (empty pool); leave the app's settings untouched.
    return render_node;
  }

  auto resolved = resolve_video_settings(app.video_override, *gpu);
  app.render_node = render_node;
  app.video_producer_buffer_caps = resolved.video_producer_buffer_caps;
  app.h264_gst_pipeline = resolved.h264_gst_pipeline;
  app.hevc_gst_pipeline = resolved.hevc_gst_pipeline;
  app.av1_gst_pipeline = resolved.av1_gst_pipeline;
  app.support_hevc = resolved.support_hevc;
  app.support_av1 = resolved.support_av1;
  return render_node;
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

  // Pick a GPU and pin the session to it, re-resolving the video pipelines against that GPU's defaults
  auto app = std::make_shared<events::App>(run_app);
  auto render_node = assign_gpu_to_app(state, *app);

  auto session = events::StreamSession{
      .display_mode = display_mode,
      .audio_channel_count = audio_channel_count,
      .event_bus = state->event_bus,
      .client_settings = current_client.settings,
      .app = std::move(app),
      .app_local_state_folder = full_path.string(),
      .app_host_state_folder = std::filesystem::path(state->host->host_base_state_folder) /
                               current_client.app_state_folder / run_app.base.title,

      .render_node = render_node,

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

  // Advertise codec support based on the assigned GPU, not the global config
  session.display_mode.hevc_supported = session.display_mode.hevc_supported && session.app->support_hevc;
  session.display_mode.av1_supported = session.display_mode.av1_supported && session.app->support_av1;

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
} // namespace state