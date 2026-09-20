#include <catch2/catch_test_macros.hpp>

#include <session/lobby_model.hpp>

using namespace wolf::session;

namespace {

LobbyModel new_lobby(std::string id = "lobby-1", bool multi_user = true, bool stop_when_empty = true) {
  LobbyModel model;
  model.lobby_id = std::move(id);
  model.width = 1920;
  model.height = 1080;
  model.refresh_rate = 60;
  model.multi_user = multi_user;
  model.stop_when_everyone_leaves = stop_when_empty;
  return model;
}

template <typename T> bool has(const std::vector<LobbyEffect> &effects) {
  for (const auto &effect : effects) {
    if (std::holds_alternative<T>(effect)) {
      return true;
    }
  }
  return false;
}

/** Drive a lobby to the steady `RunnerRunning` state. */
LobbyModel running_lobby(bool multi_user = true, bool stop_when_empty = true) {
  auto model = new_lobby("lobby-1", multi_user, stop_when_empty);
  model = advance_lobby(model, StartLobby{});
  model = advance_lobby(model, LobbyGpuAssigned{.render_node = "/dev/dri/renderD128"});
  model = advance_lobby(model, LobbyDesktopReady{.wayland_socket_name = "wayland-1"});
  model = advance_lobby(model, LobbyRunnerStarted{});
  return model;
}

} // namespace

TEST_CASE("lobby walks the happy path in order", "[lobby]") {
  auto model = new_lobby();

  auto t = step_lobby(model, StartLobby{});
  REQUIRE(t.model.state == LobbyState::Created);
  REQUIRE(has<AssignLobbyGpu>(t.effects));

  t = step_lobby(t.model, LobbyGpuAssigned{.render_node = "/dev/dri/renderD128"});
  REQUIRE(t.model.state == LobbyState::DesktopStarting);
  REQUIRE(t.model.render_node == "/dev/dri/renderD128");
  REQUIRE(has<StartLobbyDesktop>(t.effects));

  t = step_lobby(t.model, LobbyDesktopReady{.wayland_socket_name = "wayland-1"});
  REQUIRE(t.model.state == LobbyState::RunnerStarting);
  REQUIRE(has<StartLobbyRunner>(t.effects));

  t = step_lobby(t.model, LobbyRunnerStarted{});
  REQUIRE(t.model.state == LobbyState::RunnerRunning);
  REQUIRE(is_accepting_sessions(t.model.state));

  t = step_lobby(t.model, StopLobby{.reason = "done"});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));
  REQUIRE(has<ReleaseLobbyGpu>(t.effects));

  t = step_lobby(t.model, LobbyTeardownComplete{});
  REQUIRE(t.model.state == LobbyState::Stopped);
  REQUIRE(is_terminal(t.model.state));
}

TEST_CASE("sessions attach and detach from a running lobby", "[lobby]") {
  auto model = running_lobby();

  auto t = step_lobby(model, SessionJoined{.session_id = 10});
  REQUIRE(t.model.state == LobbyState::RunnerRunning);
  REQUIRE(t.model.connected_sessions.size() == 1);
  REQUIRE(has<AttachSession>(t.effects));

  t = step_lobby(t.model, SessionJoined{.session_id = 20});
  REQUIRE(t.model.connected_sessions.size() == 2);

  t = step_lobby(t.model, SessionLeft{.session_id = 10});
  REQUIRE(t.model.connected_sessions.size() == 1);
  REQUIRE(t.model.connected_sessions[0] == 20);
  REQUIRE(has<DetachSession>(t.effects));
}

TEST_CASE("a duplicate join is ignored", "[lobby]") {
  auto model = running_lobby();
  model = advance_lobby(model, SessionJoined{.session_id = 10});

  auto t = step_lobby(model, SessionJoined{.session_id = 10});
  REQUIRE(t.model.connected_sessions.size() == 1);
  REQUIRE(t.effects.empty());
}

TEST_CASE("a single-user lobby refuses a second session", "[lobby]") {
  auto model = running_lobby(/*multi_user=*/false);
  model = advance_lobby(model, SessionJoined{.session_id = 10});

  auto t = step_lobby(model, SessionJoined{.session_id = 20});
  REQUIRE(t.model.connected_sessions.size() == 1);
  REQUIRE(t.effects.empty());
}

TEST_CASE("the lobby stops when the last session leaves", "[lobby]") {
  auto model = running_lobby(/*multi_user=*/true, /*stop_when_empty=*/true);
  model = advance_lobby(model, SessionJoined{.session_id = 10});

  auto t = step_lobby(model, SessionLeft{.session_id = 10});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));
  // The leaving session is still detached (input/audio switched back) before the teardown.
  REQUIRE(has<DetachSession>(t.effects));
  REQUIRE(std::holds_alternative<DetachSession>(t.effects.front()));
}

TEST_CASE("the lobby survives an empty room when configured to", "[lobby]") {
  auto model = running_lobby(/*multi_user=*/true, /*stop_when_empty=*/false);
  model = advance_lobby(model, SessionJoined{.session_id = 10});

  auto t = step_lobby(model, SessionLeft{.session_id = 10});
  REQUIRE(t.model.state == LobbyState::RunnerRunning);
  REQUIRE(t.model.connected_sessions.empty());
  REQUIRE(has<DetachSession>(t.effects));
}

TEST_CASE("runner exit stops the lobby", "[lobby]") {
  auto model = running_lobby();

  auto t = step_lobby(model, LobbyRunnerExited{.exit_code = 0});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));
}

TEST_CASE("desktop failure is fatal and tears down", "[lobby]") {
  auto model = new_lobby();
  model.state = LobbyState::DesktopStarting;

  auto t = step_lobby(model, LobbyDesktopFailed{.reason = "compositor panicked"});
  REQUIRE(t.model.state == LobbyState::Failed);
  REQUIRE(t.model.failure_reason == "compositor panicked");
  REQUIRE(has<TeardownLobby>(t.effects));
  REQUIRE(has<ReleaseLobbyGpu>(t.effects));
}

TEST_CASE("out-of-order lobby inputs are ignored, not fatal", "[lobby]") {
  auto model = new_lobby(); // Created

  // A join before the lobby is running must not advance the state.
  auto t = step_lobby(model, SessionJoined{.session_id = 10});
  REQUIRE(t.model.state == LobbyState::Created);
  REQUIRE(t.effects.empty());

  // A desktop-ready before a GPU is assigned must not advance the state.
  t = step_lobby(model, LobbyDesktopReady{.wayland_socket_name = "wayland-1"});
  REQUIRE(t.model.state == LobbyState::Created);
  REQUIRE(t.effects.empty());
}

TEST_CASE("terminal lobby states never transition again", "[lobby]") {
  auto model = new_lobby();
  model.state = LobbyState::Stopped;

  auto t = step_lobby(model, StartLobby{});
  REQUIRE(t.model.state == LobbyState::Stopped);
  REQUIRE(t.effects.empty());

  model.state = LobbyState::Failed;
  t = step_lobby(model, StopLobby{.reason = "again"});
  REQUIRE(t.model.state == LobbyState::Failed);
  REQUIRE(t.effects.empty());
}