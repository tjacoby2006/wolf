#pragma once

#include <rest/endpoints/common.hpp>

/**
 * The HTTPS streaming workflow: listing apps and their assets, and the launch/resume/cancel
 * lifecycle. `launch` registers the session and hands it to its actor; `resume` re-uses the
 * already-running session's display and devices, so it only swap the entry in the registry.
 */
namespace endpoints {

using namespace control;
using namespace wolf::core;

namespace https {

void applist(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
             const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
             const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTPS>(request);

  immer::vector<immer::box<events::App>> moonlight_apps =
      state::get_moonlight_profile(state->config).value()->apps->load();
  auto base_apps = moonlight_apps                                                        //
                   | ranges::views::transform([](const auto &app) { return app->base; }) //
                   | ranges::to<immer::vector<moonlight::App>>();
  auto xml = moonlight::applist(base_apps);

  send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::success_ok, xml);
}

void appasset(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
              const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
              const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTPS>(request);

  SimpleWeb::CaseInsensitiveMultimap headers = request->parse_query_string();
  auto app_id = get_header(headers, "appid");
  if (!app_id) {
    logs::log(logs::warning, "[HTTP] Wrong request, missing app_id");
    server_error<SimpleWeb::HTTPS>(response);
    return;
  }

  auto app = state::get_moonlight_app_by_id(state->config, app_id.value());
  if (!app || !app.value()->base.icon_png_path) {
    logs::log(logs::trace, "[HTTP] Can't find icon_png_path for app with id: {}", app_id.value());
    server_error<SimpleWeb::HTTPS>(response);
    return;
  }

  auto icon_path = app.value()->base.icon_png_path.value();
  if (auto icon = utils::get_icon(state->host->local_base_state_folder, icon_path)) {
    SimpleWeb::CaseInsensitiveMultimap asset_headers;
    asset_headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, icon.value(), asset_headers);
    response->close_connection_after_response = true;
  } else {
    response->write(SimpleWeb::StatusCode::client_error_not_found, "asset not found");
  }
}

auto create_run_session(const SimpleWeb::CaseInsensitiveMultimap &headers,
                        const std::string &client_ip,
                        const state::PairedClient &current_client,
                        immer::box<state::AppState> state,
                        const events::App &run_app) {
  auto display_mode_str = utils::split(get_header(headers, "mode").value_or("1920x1080x60"), 'x');
  moonlight::DisplayMode display_mode = {std::stoi(display_mode_str[0].data()),
                                         std::stoi(display_mode_str[1].data()),
                                         std::stoi(display_mode_str[2].data()),
                                         state->config->support_hevc,
                                         state->config->support_av1};

  auto surround_info = std::stoi(get_header(headers, "surroundAudioInfo").value_or("196610"));
  int channelCount = surround_info & (0xffff /* last 16 bits */);

  auto base_session = create_stream_session(state,
                                            run_app,
                                            current_client,
                                            display_mode,
                                            channelCount,
                                            get_header(headers, "rikey").value(),
                                            get_header(headers, "rikeyid").value());

  base_session->ip = client_ip;
  return std::move(base_session);
}

std::string get_rtsp_ip_string(const std::string &local_ip, const events::StreamSession &session) {
  auto use_fake_ip = utils::get_env("WOLF_USE_RTSP_FAKE_IP", "TRUE") == "TRUE"s;
  return use_fake_ip ? session.rtsp_fake_ip : local_ip;
}

// Forward declaration so launch() can delegate to resume() when a session is already running.
void resume(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
            const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
            const state::PairedClient &current_client,
            const immer::box<state::AppState> &state);

void launch(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
            const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
            const state::PairedClient &current_client,
            const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTPS>(request);

  SimpleWeb::CaseInsensitiveMultimap headers = request->parse_query_string();
  auto app = state::get_moonlight_app_by_id(state->config, get_header(headers, "appid").value());
  if (!app) {
    logs::log(logs::warning, "[HTTP] Requested wrong app_id: not found");
    server_error<SimpleWeb::HTTPS>(response);
    return;
  }

  // Re-launching while a session is already running would start a second runner that removes the
  // live container out from under the first one. Resume the existing session instead.
  if (state::get_session_by_client(state->running_sessions->load(), current_client)) {
    logs::log(logs::info, "[HTTP] Client already has a running session, resuming instead of relaunching");
    resume(response, request, current_client, state);
    return;
  }

  auto client_ip = get_client_ip<SimpleWeb::HTTPS>(request);
  auto new_session = create_run_session(request->parse_query_string(), client_ip, current_client, state, app.value());

  // Register the session *before* the client is told the launch succeeded: the RTSP handshake and
  // the control channel look it up (by client id, and later by RTP secret) and would not find it
  // otherwise. Upserting (rather than appending) means the session's actor can also register it
  // when it adopts the session, without producing a duplicate.
  state->running_sessions->update([new_session](const immer::vector<events::StreamSession> &ses_v) {
    return state::add_session(ses_v, *new_session);
  });

  // Hand the session to its actor, which owns the rest of the lifecycle from here on.
  state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

  auto rtsp_ip = get_rtsp_ip_string(get_host_ip<SimpleWeb::HTTPS>(request, state), *new_session);
  auto xml = moonlight::launch_success(rtsp_ip, std::to_string(get_port(state::RTSP_SETUP_PORT)));
  send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::success_ok, xml);
}

void resume(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
            const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
            const state::PairedClient &current_client,
            const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTPS>(request);

  auto client_ip = get_client_ip<SimpleWeb::HTTPS>(request);
  auto old_session = state::get_session_by_client(state->running_sessions->load(), current_client);
  if (old_session) {
    auto new_session =
        create_run_session(request->parse_query_string(), client_ip, current_client, state, *old_session->app);
    // The compositor, the input devices and — crucially — the GPU the load balancer assigned all
    // belong to the client's stream rather than to this RTSP session, so they survive the swap.
    state::carry_over_resumed_session(*old_session, *new_session);

    state->running_sessions->update([&old_session, new_session](const immer::vector<events::StreamSession> ses_v) {
      return state::remove_session(ses_v, old_session.value()).push_back(*new_session);
    });

    auto rtsp_ip = get_rtsp_ip_string(get_host_ip<SimpleWeb::HTTPS>(request, state), *new_session);
    auto xml = moonlight::launch_resume(rtsp_ip, std::to_string(get_port(state::RTSP_SETUP_PORT)));
    send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::success_ok, xml);
    return;
  }

  logs::log(logs::warning, "[HTTPS] Received resume event from an unregistered session, ip: {}", client_ip);
  server_error<SimpleWeb::HTTPS>(response);
}

void cancel(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Response> &response,
            const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTPS>::Request> &request,
            const state::PairedClient &current_client,
            const immer::box<state::AppState> &state) {
  log_req<SimpleWeb::HTTPS>(request);

  auto client_session = state::get_session_by_client(state->running_sessions->load(), current_client);
  if (client_session) {
    state->event_bus->fire_event(
        immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = client_session->session_id}));

    state->running_sessions->update([&client_session](const immer::vector<events::StreamSession> &ses_v) {
      return state::remove_session(ses_v, client_session.value());
    });
  } else {
    auto client_ip = get_client_ip<SimpleWeb::HTTPS>(request);
    logs::log(logs::warning, "[HTTPS] Received resume event from an unregistered session, ip: {}", client_ip);
  }

  XML xml;
  xml.put("root.<xmlattr>.status_code", 200);
  xml.put("root.cancel", 1);
  send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::success_ok, xml);
}

} // namespace https

} // namespace endpoints