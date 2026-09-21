#include <atomic>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <session/session_actor.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <sessions/session_runtime.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

using session_devices = immer::map<std::string /* session_id */, std::shared_ptr<events::devices_atom_queue>>;

/**
 * Will stop the execution until an event of type RTPPingType is triggered
 * and the signature is matching the input `sess`.
 * Returns the RTPPingType event
 */
template <typename RTPPingType>
immer::box<RTPPingType> wait_for_ping(std::shared_ptr<events::EventBusType> ev_bus, const auto &sess) {
  auto ping_promise = std::make_shared<std::promise<RTPPingType>>();
  auto ping_future = ping_promise->get_future();
  auto resolved = std::make_shared<std::atomic_bool>(false);

  auto handler = ev_bus->register_handler<immer::box<RTPPingType>>(
      [sess, ping_promise, resolved](const immer::box<RTPPingType> &ping_ev) {
        // Check if this ping is for our session
        if (sess->rtp_secret_payload == ping_ev->payload || // Secret payload matching
            (!ping_ev->payload.has_value() && ping_ev->client_ip == sess->client_ip &&
             ping_ev->client_port == sess->port)) { // Legacy IP+port matching when no payload has been passed
          bool expected = false;
          if (resolved->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // Resolve the promise with the ping event data
            ping_promise->set_value(*ping_ev);
          }
        }
      });

  // Wait for the promise to be fulfilled
  auto ping_ev = ping_future.get();

  // Unregister the handler since we only need it once
  handler.unregister();

  return ping_ev;
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_moonlight_handlers(const immer::box<state::AppState> &app_state,
                         const std::string &runtime_dir,
                         const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  /*
   * A queue of devices that are waiting to be plugged, mapped by session_id
   * This way we can accumulate devices here until the docker container is up and running
   */
  auto plugged_devices_queue = std::make_shared<immer::atom<session_devices>>();
  auto gpu_balancer = app_state->gpu_balancer;
  auto active_actors = app_state->session_actors;

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [&app_state, plugged_devices_queue, gpu_balancer, active_actors](const immer::box<events::StopStreamEvent> &ev) {
        // Ask the session's actor to stop; it owns the teardown (GPU release, session removal,
        // device queue cleanup) and will run it as part of its state machine.
        if (auto actor = active_actors->load()->find(ev->session_id)) {
          (*actor)->post(wolf::session::StopRequested{.reason = "stop stream event"});
          // Drop our reference on a detached thread: destroying the actor joins its worker, and the
          // worker's teardown fires events, so doing it inline here could deadlock the event bus.
          auto actors = active_actors;
          auto session_id = ev->session_id;
          std::thread([actors, session_id]() {
            actors->update([session_id](const auto &map) { return map.erase(session_id); });
          }).detach();
          return;
        }

        // No actor (e.g. a session that never got one): fall back to the legacy cleanup so we
        // never leak a GPU assignment or a stale session entry.
        if (auto session = state::get_session_by_id(app_state->running_sessions->load().get(), ev->session_id)) {
          auto node = session->assigned_render_node;
          if (!node.empty()) {
            gpu_balancer->update([node](const state::GpuBalancer &bal) { return bal.release(node); });
          }
        }

        app_state->running_sessions->update([&ev](const immer::vector<events::StreamSession> &ses_v) {
          return state::remove_session(ses_v, {.session_id = ev->session_id});
        });

        plugged_devices_queue->update([=](const auto map) { return map.erase(std::to_string(ev->session_id)); });
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [plugged_devices_queue, lobbies = app_state->lobbies](const immer::box<events::PlugDeviceEvent> &hotplug_ev) {
        logs::log(logs::debug, "{} received hot-plug device event", hotplug_ev->session_id);

        // If we are currently in a lobby we don't want to plug the device to the original wolf-ui session
        if (!state::get_lobby_by_connected_session(lobbies->load(), hotplug_ev->session_id)) {
          if (auto session_devices_queue = plugged_devices_queue->load()->find(hotplug_ev->session_id)) {
            session_devices_queue->get()->push(hotplug_ev);
          } else {
            logs::log(logs::warning, "Unable to find plugged_devices_queue for session {}", hotplug_ev->session_id);
          }
        } else {
          // This event will be picked up by the lobbies handler
          logs::log(logs::debug, "Session {} is in a lobby, ignoring hot-plug device event", hotplug_ev->session_id);
        }
      }));

  // A new StreamSession is created: hand it to a SessionActor, which owns the whole lifecycle.
  // The actor drives GPU assignment, compositor startup, the runner and teardown as one linear
  // state machine (see src/session/), instead of the old web of detached handlers.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StreamSession>>(
      [&app_state, plugged_devices_queue, runtime_dir, audio_server, active_actors](const immer::box<events::StreamSession> &session) {
        auto stream_session = std::make_shared<events::StreamSession>(*session);

        wolf::session::SessionModel model;
        model.session_id = stream_session->session_id;
        model.width = stream_session->display_mode.width;
        model.height = stream_session->display_mode.height;
        model.refresh_rate = stream_session->display_mode.refreshRate;

        auto context = SessionContext{.app_state = app_state,
                                      .stream_session = stream_session,
                                      .runtime_dir = runtime_dir,
                                      .audio_server = audio_server,
                                      .plugged_devices_queue = plugged_devices_queue};
        auto runtime = std::make_shared<MoonlightSessionRuntime>(std::move(context));

        auto actor = std::make_shared<wolf::session::SessionActor>(std::move(model), runtime);

        // When the actor finishes on its own (e.g. the runner exited), drop our reference so the
        // actor, its runtime and the session are freed. The callback runs on the actor's worker
        // thread, so the erase happens on a detached thread (destroying the actor joins its worker).
        auto actors = active_actors;
        auto session_id = stream_session->session_id;
        actor->set_on_finished([actors, session_id]() {
          std::thread([actors, session_id]() {
            actors->update([session_id](const auto &map) { return map.erase(session_id); });
          }).detach();
        });

        actor->start();

        // Kick off the lifecycle. The runtime feeds the rest of the inputs back as effects complete.
        actor->post(wolf::session::StartSession{});

        // Keep the actor alive for the session's lifetime; it stops itself on a terminal state.
        active_actors->update([id = stream_session->session_id, actor](const auto &actors) {
          return actors.set(id, actor);
        });
      }));

  /*
   * Route the client's RTP ping to the session's actor so it can advance to `Streaming`.
   *
   * The ping is matched against the session exactly as the old `wait_for_ping` did: the client
   * echoes back the per-session secret payload exchanged over RTSP, or (for older clients that
   * send no payload) the ping's IP and port match the session's.
   */
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::RTPVideoPingEvent>>(
      [&app_state, active_actors](const immer::box<events::RTPVideoPingEvent> &ping) {
        for (const auto &session : app_state->running_sessions->load().get()) {
          // Mirrors the legacy `wait_for_ping` matching: payload match when the client echoes the
          // secret, otherwise the IP/port fallback (which compared against the session's video
          // stream port).
          bool matches = ping->payload.has_value()
                             ? session.rtp_secret_payload == *ping->payload
                             : (ping->client_ip == session.ip && ping->client_port == session.video_stream_port);
          if (!matches) {
            continue;
          }
          if (auto actor = active_actors->load()->find(session.session_id)) {
            (*actor)->post(wolf::session::RtpPingReceived{.client_ip = ping->client_ip,
                                                          .client_port = ping->client_port});
          }
          return;
        }
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::VideoSession>>(
      [ev_bus = app_state->event_bus,
       gst_contexts = app_state->gst_contexts](const immer::box<events::VideoSession> &sess) {
        // Start a thread that will wait for the RTP ping event
        std::thread([sess, ev_bus, gst_contexts]() {
          auto ping_ev = wait_for_ping<events::RTPVideoPingEvent>(ev_bus, sess);

          // Start streaming
          streaming::start_streaming_video(sess,
                                           ev_bus,
                                           ping_ev->client_ip,
                                           ping_ev->client_port,
                                           gst_contexts,
                                           ping_ev->video_socket.get());
        }).detach();
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::AudioSession>>(
      [ev_bus = app_state->event_bus, audio_server](const immer::box<events::AudioSession> &sess) {
        // Start a thread that will wait for the RTP ping event
        std::thread([sess, ev_bus, audio_server]() {
          auto ping_ev = wait_for_ping<events::RTPAudioPingEvent>(ev_bus, sess);

          // Start streaming
          auto audio_server_name = audio_server ? audio::get_server_name(audio_server->server)
                                                : std::optional<std::string>();
          auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, sess->session_id);
          auto server_name = audio_server_name ? audio_server_name.value() : "";

          streaming::start_streaming_audio(sess,
                                           ev_bus,
                                           ping_ev->client_ip,
                                           ping_ev->client_port,
                                           ping_ev->audio_socket.get(),
                                           sink_name,
                                           server_name);
        }).detach();
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
