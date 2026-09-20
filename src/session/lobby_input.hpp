#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wolf::session {

/**
 * Inputs that drive a LobbyActor.
 *
 * Protocol adapters (the Wolf API, the Moonlight REST/control paths) translate their requests into
 * these values and post them to the lobby's actor. The actor never reaches back into those layers.
 */

/** Kick off the lobby lifecycle: pick a GPU and start the shared desktop. */
struct StartLobby {};

/** A GPU/render node has been selected for this lobby. */
struct LobbyGpuAssigned {
  std::string render_node;
};

/** The shared compositor finished starting. */
struct LobbyDesktopReady {
  std::string wayland_socket_name;
};

/** The shared compositor failed to start. */
struct LobbyDesktopFailed {
  std::string reason;
};

/** The shared runner has been launched. */
struct LobbyRunnerStarted {};

/** The shared runner exited on its own. */
struct LobbyRunnerExited {
  int exit_code = 0;
};

/** A Moonlight session wants to attach to this lobby. */
struct SessionJoined {
  std::uint64_t session_id = 0;
};

/** A Moonlight session detached from this lobby. */
struct SessionLeft {
  std::uint64_t session_id = 0;
};

/** Stop the lobby. `reason` is for logging/diagnostics only. */
struct StopLobby {
  std::string reason;
};

/** The runtime finished tearing down the lobby's resources. */
struct LobbyTeardownComplete {};

using LobbyInput = std::variant<StartLobby,
                                LobbyGpuAssigned,
                                LobbyDesktopReady,
                                LobbyDesktopFailed,
                                LobbyRunnerStarted,
                                LobbyRunnerExited,
                                SessionJoined,
                                SessionLeft,
                                StopLobby,
                                LobbyTeardownComplete>;

} // namespace wolf::session