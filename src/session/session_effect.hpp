#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wolf::session {

/**
 * Effects are the side effects a SessionActor asks the runtime to perform.
 *
 * The state machine itself is pure: `step(state, input) -> (state, effects)`. It never touches
 * GStreamer, Docker, PulseAudio or the network directly. The runtime (see `session_runtime.hpp`)
 * interprets these effects and feeds the resulting inputs back into the actor's inbox.
 *
 * This is what makes the lifecycle unit-testable: a test can drive inputs and assert on the
 * emitted effects without any hardware, containers or event bus.
 */

/** Pick a GPU/render node for this session (load balancer + sticky assignment). */
struct AssignGpu {
  std::uint64_t session_id = 0;
};

/** Start the virtual compositor (gst-wayland-display) on the assigned node. */
struct StartDesktop {
  std::uint64_t session_id = 0;
  std::string render_node;
  int width = 0;
  int height = 0;
  int refresh_rate = 0;
};

/** Start the app runner (container/process) against the ready desktop. */
struct StartRunner {
  std::uint64_t session_id = 0;
  std::string render_node;
};

/** Start the video/audio streaming pipelines once the client pinged. */
struct StartStreaming {
  std::uint64_t session_id = 0;
  std::string client_ip;
  std::uint16_t client_port = 0;
};

/** Pause the streaming pipelines. */
struct PauseStreaming {
  std::uint64_t session_id = 0;
};

/** Resume the streaming pipelines. */
struct ResumeStreaming {
  std::uint64_t session_id = 0;
};

/** Ask the encoder for a keyframe. */
struct RequestIdr {
  std::uint64_t session_id = 0;
};

/** Plug a device into the running runner. */
struct PlugDevice {
  std::uint64_t session_id = 0;
  std::string device_id;
};

/** Unplug a device from the running runner. */
struct UnplugDevice {
  std::uint64_t session_id = 0;
  std::string device_id;
};

/** Tear down everything owned by this session (runner, desktop, sinks, GPU). */
struct Teardown {
  std::uint64_t session_id = 0;
  std::string reason;
};

/** Release the GPU back to the load balancer. */
struct ReleaseGpu {
  std::uint64_t session_id = 0;
};

using SessionEffect = std::variant<AssignGpu,
                                   StartDesktop,
                                   StartRunner,
                                   StartStreaming,
                                   PauseStreaming,
                                   ResumeStreaming,
                                   RequestIdr,
                                   PlugDevice,
                                   UnplugDevice,
                                   Teardown,
                                   ReleaseGpu>;

} // namespace wolf::session
