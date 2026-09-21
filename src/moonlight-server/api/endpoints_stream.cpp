#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

/*
 * Stream-session endpoints.
 *
 * Split out of the single `endpoints.cpp` so each API domain lives in its own translation unit.
 */

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    res.sessions.push_back(rfl::Reflector<events::StreamSession>::from(session));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    immer::box<events::App> choosen_app;
    auto ss = session.value();
    if (auto app_id = ss.app_id) {
      auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, *app_id);
      if (!app) {
        logs::log(logs::warning, "[API] Invalid app_id: {}", *app_id);
        auto res = GenericErrorResponse{.error = "Invalid app_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_app = *app;
    } else {
      auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
      if (!moonlight_profile) {
        logs::log(logs::warning, "[API] No moonlight profile found, unable to automatically create an app.");
        auto res = GenericErrorResponse{.error = "No moonlight profile found"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
      immer::box<events::App> sample_app = apps.front();
      choosen_app = events::App{
          .base = {.title = "dummy", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

          .video_producer_buffer_caps = sample_app->video_producer_buffer_caps,

          .h264_gst_pipeline = sample_app->h264_gst_pipeline,
          .hevc_gst_pipeline = sample_app->hevc_gst_pipeline,
          .av1_gst_pipeline = sample_app->av1_gst_pipeline,

          .render_node = sample_app->render_node,
          .opus_gst_pipeline = sample_app->opus_gst_pipeline,
          .start_virtual_compositor = true,
          .start_audio_server = true,

          .runner = std::make_shared<process::RunProcess>(state_->app_state->event_bus,
                                                          "sh -c \"while :; do echo 'running...'; sleep 10; done\"")};
    }

    config::PairedClient choosen_client;
    if (auto client_id = ss.client_id) {
      auto client = state::get_client_by_id(this->state_->app_state->config, *client_id);
      if (!client) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        auto res = GenericErrorResponse{.error = "Invalid client_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_client = *client;
    } else {
      // Create a dummy client
      choosen_client = {.client_cert = "", .app_state_folder = state::gen_uuid(), .settings = {}};
    }

    choosen_client.settings = ss.client_settings.value_or(config::ClientSettings{});

    auto new_session = state::create_stream_session( //
        state_->app_state,
        choosen_app,
        choosen_client,
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv);
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;

    state_->app_state->running_sessions->update(
        [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = std::to_string(new_session->session_id)};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(start_req.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      video_session.session_id = session_id; // Can't be JSON encoded
      if (video_session.render_node.empty()) {
        video_session.render_node =
            session->assigned_render_node.empty() ? session->app->render_node : session->assigned_render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id; // Can't be JSON encoded
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionPauseRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(input_request.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto pkt_parsed = crypto::hex_to_str(hex_pkt);
      control::INPUT_PKT *input_pkt = reinterpret_cast<control::INPUT_PKT *>(pkt_parsed.data());
      control::handle_input(session.value(), {}, input_pkt);

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

} // namespace wolf::api
