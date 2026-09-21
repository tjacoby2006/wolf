#pragma once

#include <memory>
#include <optional>
#include <string>

#include <events/events.hpp>
#include <immer/atom.hpp>
#include <immer/map.hpp>
#include <session/session_actor.hpp>
#include <sessions/handlers.hpp>
#include <state/data-structures.hpp>

namespace wolf::core::sessions {

/** Per-session queue of devices waiting to be plugged into the runner. */
using session_devices = immer::map<std::string /* session_id */, std::shared_ptr<events::devices_atom_queue>>;

/**
 * Everything the runtime needs to bring a session up and tear it down.
 *
 * This is the bridge between the pure `wolf::session` state machine and the concrete Wolf runtime
 * (GStreamer, Docker runners, PulseAudio, the GPU balancer). It replaces the scattered event-bus
 * handlers that used to live in `sessions/moonlight.cpp`.
 */
struct SessionContext {
  immer::box<state::AppState> app_state;
  std::shared_ptr<events::StreamSession> stream_session;
  std::string runtime_dir;
  std::optional<AudioServer> audio_server;
  /** Shared across sessions; the runtime registers/removes this session's device queue here. */
  std::shared_ptr<immer::atom<session_devices>> plugged_devices_queue;
};

/**
 * The concrete `SessionRuntime` for a Moonlight stream session.
 *
 * Each effect emitted by the state machine is translated into the corresponding Wolf call, and the
 * result is fed back into the actor as an input:
 *
 *   AssignGpu      -> gpu_balancer.pick + acquire_sticky        -> GpuAssigned / StopRequested
 *   AdoptSession   -> running_sessions upsert                   -> (no feedback)
 *   StartDesktop   -> streaming::start_video_producer           -> DesktopReady / DesktopFailed
 *   StartRunner    -> sessions::start_runner (blocking)         -> RunnerStarted / RunnerExited
 *   StartStreaming -> streaming::start_streaming_video/audio    -> (no feedback)
 *   Teardown       -> release GPU, drop session, stop pipelines -> TeardownComplete
 *
 * Long-running work (compositor startup, the runner itself) happens on detached threads so the
 * actor's own thread is never blocked; the actor stays responsive to stop requests throughout.
 */
class MoonlightSessionRuntime : public wolf::session::SessionRuntime {
public:
  explicit MoonlightSessionRuntime(SessionContext context);

  void execute(const wolf::session::SessionEffect &effect) override;

private:
  /** Register this session in `running_sessions` (idempotent; see `state::add_session`). */
  void adopt_session(std::uint64_t session_id);
  void assign_gpu(std::uint64_t session_id);
  void start_desktop(std::uint64_t session_id, const std::string &render_node);
  void start_runner(std::uint64_t session_id, const std::string &render_node);
  void start_streaming(std::uint64_t session_id, const std::string &client_ip, std::uint16_t client_port);
  void forward_pause(std::uint64_t session_id);
  void forward_resume(std::uint64_t session_id);
  void forward_idr(std::uint64_t session_id);
  void teardown(std::uint64_t session_id, const std::string &reason);
  void release_gpu(std::uint64_t session_id);

  SessionContext context_;
};

} // namespace wolf::core::sessions
