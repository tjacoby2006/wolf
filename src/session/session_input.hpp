#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace wolf::session {

/**
 * Inputs are the only way to drive a SessionActor.
 *
 * Protocol adapters (RTSP, ENet control, RTP ping, REST) translate wire messages into these
 * values and push them onto the actor's inbox. The actor never reaches back into the protocol
 * layers, and the protocol layers never mutate session state directly.
 *
 * This is the boundary that replaces the old pattern of firing `StreamSession`, `StartRunner`,
 * `VideoSession`, `AudioSession`, `PauseStreamEvent`, ... onto the global event bus and letting
 * scattered handlers mutate shared `immer` atoms.
 */

/** A GPU/render node has been selected for this session. */
struct GpuAssigned {
  std::string render_node;
};

/** The virtual compositor (Wayland display) finished starting. */
struct DesktopReady {
  std::string wayland_socket_name;
};

/** The virtual compositor failed to start. */
struct DesktopFailed {
  std::string reason;
};

/** The app runner (container/process) has been launched. */
struct RunnerStarted {};

/** The app runner exited on its own. */
struct RunnerExited {
  int exit_code = 0;
};

/** The client's RTP ping arrived; the stream can start. */
struct RtpPingReceived {
  std::string client_ip;
  std::uint16_t client_port = 0;
};

/** The client paused the stream (ENet disconnect / pause packet). */
struct PauseRequested {};

/** The client resumed the stream. */
struct ResumeRequested {};

/** The client requested a keyframe (IDR). */
struct IdrRequested {};

/** A device (gamepad, pen, ...) should be plugged into the running runner. */
struct DevicePlugged {
  std::string device_id;
};

/** A device should be unplugged from the running runner. */
struct DeviceUnplugged {
  std::string device_id;
};

/** Stop the session. `reason` is for logging/diagnostics only. */
struct StopRequested {
  std::string reason;
};

/** The runtime finished tearing down the session's resources. */
struct TeardownComplete {};

/** A periodic tick, used to drive timeouts (e.g. waiting for the RTP ping). */
struct Tick {};

using SessionInput = std::variant<GpuAssigned,
                                  DesktopReady,
                                  DesktopFailed,
                                  RunnerStarted,
                                  RunnerExited,
                                  RtpPingReceived,
                                  PauseRequested,
                                  ResumeRequested,
                                  IdrRequested,
                                  DevicePlugged,
                                  DeviceUnplugged,
                                  StopRequested,
                                  TeardownComplete,
                                  Tick>;

} // namespace wolf::session
