#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wolf::session {

/**
 * Effects a LobbyActor asks the runtime to perform.
 *
 * The state machine is pure: `step_lobby(state, input) -> (state, effects)`. The runtime interprets
 * these and feeds the resulting inputs back, exactly like the session runtime.
 */

/** Pick a GPU/render node for this lobby (load balancer). */
struct AssignLobbyGpu {
  std::string lobby_id;
};

/** Start the shared compositor on the assigned node. */
struct StartLobbyDesktop {
  std::string lobby_id;
  std::string render_node;
  int width = 0;
  int height = 0;
  int refresh_rate = 0;
};

/** Start the shared runner against the ready desktop. */
struct StartLobbyRunner {
  std::string lobby_id;
  std::string render_node;
};

/** Attach a session to the shared desktop (input/audio/video switch). */
struct AttachSession {
  std::string lobby_id;
  std::uint64_t session_id = 0;
};

/** Detach a session from the shared desktop (input/audio/video switch back). */
struct DetachSession {
  std::string lobby_id;
  std::uint64_t session_id = 0;
};

/** Tear down everything owned by this lobby (runner, desktop, sinks, GPU). */
struct TeardownLobby {
  std::string lobby_id;
  std::string reason;
};

/** Release the lobby's GPU back to the load balancer. */
struct ReleaseLobbyGpu {
  std::string lobby_id;
};

using LobbyEffect = std::variant<AssignLobbyGpu,
                                 StartLobbyDesktop,
                                 StartLobbyRunner,
                                 AttachSession,
                                 DetachSession,
                                 TeardownLobby,
                                 ReleaseLobbyGpu>;

} // namespace wolf::session