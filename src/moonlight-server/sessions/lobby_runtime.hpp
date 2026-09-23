#pragma once

#include <memory>
#include <optional>
#include <string>

#include <events/events.hpp>
#include <session/lobby_actor.hpp>
#include <sessions/handlers.hpp>
#include <state/data-structures.hpp>

namespace wolf::core::sessions {

/**
 * Everything the lobby runtime needs to bring a shared desktop up and tear it down.
 *
 * This is the bridge between the pure `wolf::session` lobby state machine and the concrete Wolf
 * runtime (GStreamer, Docker runners, PulseAudio, the GPU balancer). It replaces the scattered
 * event-bus handlers that used to live in `sessions/lobbies.cpp`.
 */
struct LobbyContext {
  immer::box<state::AppState> app_state;
  /** The lobby record (shared desktop handles: wayland display, audio sink, device queue). */
  std::shared_ptr<events::Lobby> lobby;
  /** The settings the lobby was created with (display mode, runner, state folder, ...). */
  std::shared_ptr<events::CreateLobbyEvent> settings;
  std::string runtime_dir;
  std::optional<AudioServer> audio_server;
};

/**
 * The concrete `LobbyRuntime` for a Moonlight lobby (shared desktop).
 *
 * Each effect emitted by the lobby state machine is translated into the corresponding Wolf call,
 * and the result is fed back into the actor as an input:
 *
 *   AssignLobbyGpu    -> gpu_balancer.pick + acquire            -> LobbyGpuAssigned / StopLobby
 *   StartLobbyDesktop -> streaming::start_video_producer        -> LobbyDesktopReady / LobbyDesktopFailed
 *   StartLobbyRunner  -> sessions::start_runner (blocking)      -> LobbyRunnerStarted / LobbyRunnerExited
 *   AttachSession     -> switch input/audio/video to the lobby  -> (no feedback)
 *   DetachSession     -> switch input/audio/video back          -> (no feedback)
 *   TeardownLobby     -> stop producers + runner, drop display  -> LobbyTeardownComplete
 *   ReleaseLobbyGpu   -> gpu_balancer.release                   -> (no feedback)
 *
 * `TeardownLobby` fires a `StopLobbyEvent` (so the lobby's own `<lobby_id>_video`/`_audio` producers
 * and its runner are released) before it drops the compositor; the bus routes that event back in as a
 * `StopLobby`, which the state machine ignores once it is already stopping.
 *
 * Long-running work (compositor startup, the runner itself) happens on detached threads so the
 * actor's own thread is never blocked; the actor stays responsive to stop requests throughout.
 */
class MoonlightLobbyRuntime : public wolf::session::LobbyRuntime {
public:
  explicit MoonlightLobbyRuntime(LobbyContext context);

  void execute(const wolf::session::LobbyEffect &effect) override;

private:
  void assign_gpu();
  void start_desktop(const std::string &render_node);
  void start_runner(const std::string &render_node);
  void attach_session(std::uint64_t session_id);
  void detach_session(std::uint64_t session_id);
  void teardown(const std::string &reason);
  void release_gpu();

  LobbyContext context_;
};

} // namespace wolf::core::sessions