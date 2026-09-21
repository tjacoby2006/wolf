#include <immer/vector_transient.hpp>
#include <session/lobby_actor.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <sessions/lobby_runtime.hpp>
#include <state/config.hpp>
#include <state/data-structures.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

/** Live lobby actors, keyed by lobby id. Keeps them alive for the lobby's lifetime. */
using lobby_actor_map = immer::map<std::string, std::shared_ptr<wolf::session::LobbyActor>>;

immer::vector<immer::box<events::EventBusHandlers>>
setup_lobbies_handlers(const immer::box<state::AppState> &app_state,
                       const std::string &runtime_dir,
                       const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  auto active_lobby_actors = std::make_shared<immer::atom<lobby_actor_map>>();

  // On create lobby event: hand the lobby to a LobbyActor, which owns the whole lifecycle.
  // The actor drives GPU assignment, the shared compositor, the runner and teardown as one linear
  // state machine (see src/session/lobby_*), instead of the old web of detached handlers.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [=](const immer::box<events::CreateLobbyEvent> &lobby_settings) {
        logs::log(logs::info, "[LOBBY] Creating new lobby {}", lobby_settings->id);

        auto lobby = std::make_shared<events::Lobby>(events::Lobby{.id = lobby_settings->id,
                                                                   .name = lobby_settings->name,
                                                                   .started_by_profile_id = lobby_settings->profile_id,
                                                                   .icon_png_path = lobby_settings->icon_png_path,
                                                                   .multi_user = lobby_settings->multi_user,
                                                                   .pin = lobby_settings->pin,
                                                                   .stop_when_everyone_leaves =
                                                                       lobby_settings->stop_when_everyone_leaves,
                                                                   .runner = lobby_settings->runner});

        // Publish the lobby immediately so joins can find it while it is still starting up.
        app_state->lobbies->update(
            [lobby](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(*lobby); });

        wolf::session::LobbyModel model;
        model.lobby_id = lobby->id;
        model.width = lobby_settings->video_settings.width;
        model.height = lobby_settings->video_settings.height;
        model.refresh_rate = lobby_settings->video_settings.refresh_rate;
        model.multi_user = lobby->multi_user;
        model.stop_when_everyone_leaves = lobby->stop_when_everyone_leaves;

        auto context = LobbyContext{.app_state = app_state,
                                    .lobby = lobby,
                                    .settings = std::make_shared<events::CreateLobbyEvent>(*lobby_settings),
                                    .runtime_dir = runtime_dir,
                                    .audio_server = audio_server};
        auto runtime = std::make_shared<MoonlightLobbyRuntime>(std::move(context));

        auto actor = std::make_shared<wolf::session::LobbyActor>(std::move(model), runtime);

        // When the actor finishes on its own (e.g. the runner exited, or the last session left),
        // drop our reference so the actor, its runtime and the lobby are freed. The callback runs on
        // the actor's worker thread, so the erase happens on a detached thread.
        auto actors = active_lobby_actors;
        auto lobby_id = lobby->id;
        actor->set_on_finished([actors, lobby_id]() {
          std::thread([actors, lobby_id]() {
            actors->update([lobby_id](const auto &map) { return map.erase(lobby_id); });
          }).detach();
        });

        actor->start();

        // Kick off the lifecycle. The runtime feeds the rest of the inputs back as effects complete.
        actor->post(wolf::session::StartLobby{});

        // Keep the actor alive for the lobby's lifetime; it stops itself on a terminal state.
        active_lobby_actors->update([id = lobby->id, actor](const auto &actors) { return actors.set(id, actor); });
      }));

  // When a Moonlight client joins a lobby: route the join to the lobby's actor, which owns the
  // attach logic (device migration, input/audio/video switch) as part of its state machine.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [=](const immer::box<events::JoinLobbyEvent> &join_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), join_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), join_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to join lobby: lobby {} or session {} not found",
                    join_lobby_event->lobby_id,
                    join_lobby_event->moonlight_session_id);
          join_lobby_event->error_message.get()->set_value("Lobby or session not found");
          return;
        }

        auto actor = active_lobby_actors->load()->find(lobby->id);
        if (!actor) {
          logs::log(logs::error, "[LOBBY] No actor for lobby {}", lobby->id);
          join_lobby_event->error_message.get()->set_value("Lobby is not ready");
          return;
        }

        // The actor decides whether the join is allowed (single-user lobby, duplicate join, ...).
        // We report success optimistically; the actor ignores an invalid join.
        logs::log(logs::info, "[LOBBY] Session {} joining lobby {}", session->session_id, lobby->id);
        (*actor)->post(wolf::session::SessionJoined{.session_id = session->session_id});
        join_lobby_event->error_message.get()->set_value("");
      }));

  // When a Moonlight session leaves the lobby: route the leave to the lobby's actor.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [=](const immer::box<events::LeaveLobbyEvent> &leave_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), leave_lobby_event->lobby_id);
        if (!lobby) {
          logs::log(logs::error, "[LOBBY] Failed to leave lobby: lobby {} not found", leave_lobby_event->lobby_id);
          return;
        }
        if (auto actor = active_lobby_actors->load()->find(lobby->id)) {
          (*actor)->post(wolf::session::SessionLeft{.session_id = leave_lobby_event->moonlight_session_id});
        }
      }));

  // Stopping a lobby: route the stop to the lobby's actor, which detaches every connected session
  // and tears the shared desktop down as part of its state machine.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [=](const immer::box<events::StopLobbyEvent> &stop_lobby_event) {
        auto actor = active_lobby_actors->load()->find(stop_lobby_event->lobby_id);
        if (!actor) {
          logs::log(logs::warning, "[LOBBY] No actor for lobby {}", stop_lobby_event->lobby_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] stopping lobby {}", stop_lobby_event->lobby_id);
        (*actor)->post(wolf::session::StopLobby{.reason = "stop lobby event"});
        // Drop our reference on a detached thread: destroying the actor joins its worker, and the
        // worker's teardown fires events, so doing it inline here could deadlock the event bus.
        auto actors = active_lobby_actors;
        auto lobby_id = stop_lobby_event->lobby_id;
        std::thread([actors, lobby_id]() {
          actors->update([lobby_id](const auto &map) { return map.erase(lobby_id); });
        }).detach();
      }));

  // On a PlugDeviceEvent, we have to add the device to the lobby queue so that the runner will pick it up
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [=](const immer::box<events::PlugDeviceEvent> &plug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, plug_device_event->session_id)) {
          logs::log(logs::info,
                    "[LOBBY] adding device to session {} in lobby {}",
                    plug_device_event->session_id,
                    lobby->id);

          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{
              events::PlugDeviceEvent{.session_id = lobby->id,
                                      .udev_events = plug_device_event->udev_events,
                                      .udev_hw_db_entries = plug_device_event->udev_hw_db_entries}});
        }
      }));

  // When a device is unplugged from a Moonlight session, we have to re-fire the event on our lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [=](const immer::box<events::UnplugDeviceEvent> &unplug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, unplug_device_event->session_id)) {
          logs::log(logs::debug, "[LOBBY] Unplug device for session {}", unplug_device_event->session_id);
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = unplug_device_event->udev_events,
                                        .udev_hw_db_entries = unplug_device_event->udev_hw_db_entries}});
        }
      }));

  auto on_moonlight_session_over = [app_state](std::size_t moonlight_session_id) {
    immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
    if (auto lobby = state::get_lobby_by_connected_session(lobbies, std::to_string(moonlight_session_id))) {
      logs::log(logs::info, "[LOBBY] Moonlight stream {} over, leaving lobby {}", moonlight_session_id, lobby->id);
      // Fire the LeaveLobbyEvent so that it can also be picked up by WolfUI via SSE
      app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
          events::LeaveLobbyEvent{.lobby_id = lobby->id, .moonlight_session_id = moonlight_session_id}});
    }
  };

  // When a Moonlight client Pauses a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
      [=](const immer::box<events::PauseStreamEvent> &pause_stream_event) {
        on_moonlight_session_over(pause_stream_event->session_id);
      }));

  // When a Moonlight client Stops a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [=](const immer::box<events::StopStreamEvent> &stop_stream_event) {
        on_moonlight_session_over(stop_stream_event->session_id);
      }));

  // When a client presses the WolfUI special combo, we get the user out of the lobby (and back to WolfUI)
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::ClientWolfUIComboEvent>>(
      [=](const immer::box<events::ClientWolfUIComboEvent> &event) {
        logs::log(logs::info, "Detected WolfUI combo for session {}", event->session_id);
        on_moonlight_session_over(event->session_id);
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
