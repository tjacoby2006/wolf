#include <api/api.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

/*
 * Lobby endpoints.
 *
 * Split out of the single `endpoints.cpp` so each API domain lives in its own translation unit.
 */

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();

    // The lobby's frames are consumed by the creating session's encoder, whose GPU is fixed at RTSP
    // PLAY. Resolve that node here so the lobby renders on the same GPU (see `CreateLobbyEvent`).
    //
    // Clients like wolf-ui don't send an explicit `session_id`, but they create the lobby from inside
    // the launcher session (the one running the virtual compositor) that will then join it. Falling back
    // to that session keeps render and encode on one GPU for those clients too.
    std::optional<std::string> preferred_render_node = std::nullopt;
    auto running_sessions = this->state_->app_state->running_sessions->load();
    std::optional<std::size_t> creating_session_id;
    if (auto session_id = event.value().session_id.get(); session_id.has_value() && !session_id->empty()) {
      creating_session_id = std::stoul(*session_id);
    } else {
      creating_session_id = state::get_launcher_session_id(running_sessions);
      if (creating_session_id) {
        logs::log(logs::debug, "[API] No session_id for lobby creation, using launcher session {}",
                  *creating_session_id);
      }
    }
    if (creating_session_id) {
      auto session = state::get_session_by_id(running_sessions.get(), *creating_session_id);
      if (!session) {
        logs::log(logs::warning, "[API] Invalid session_id for lobby creation: {}", *creating_session_id);
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
        return;
      }
      // Mirror the encode-side fallback: an unassigned session still has an app render node.
      preferred_render_node =
          session->assigned_render_node.empty() ? session->app->render_node : session->assigned_render_node;
    }

    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get(),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = event.value().video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration),
                .motion_controller_override = client_settings.motion_controller_override.value_or(
                    default_client_settings.motion_controller_override)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus),
        .preferred_render_node = preferred_render_node};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(20));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

namespace {

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

} // namespace

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session = state::get_session_by_id(this->state_->app_state->running_sessions->load(),
                                            std::stoul(event.value().session_id));
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

} // namespace wolf::api
