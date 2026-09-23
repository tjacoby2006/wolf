#include <catch2/catch_test_macros.hpp>

#include <session/lobby_actor.hpp>
#include <session/lobby_model.hpp>

using namespace wolf::session;

namespace {

/** A runtime that reacts to effects by feeding inputs back, like the real runtime does. */
class AutoPilotLobbyRuntime : public LobbyRuntime {
public:
  void execute(const LobbyEffect &effect) override {
    std::visit(
        [this](const auto &e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, AssignLobbyGpu>) {
            post(LobbyGpuAssigned{.render_node = "/dev/dri/renderD128"});
          } else if constexpr (std::is_same_v<T, StartLobbyDesktop>) {
            post(LobbyDesktopReady{.wayland_socket_name = "wayland-1"});
          } else if constexpr (std::is_same_v<T, StartLobbyRunner>) {
            post(LobbyRunnerStarted{});
          } else if constexpr (std::is_same_v<T, TeardownLobby>) {
            post(LobbyTeardownComplete{});
          }
        },
        effect);
  }
};

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

/** @return the first effect of type `T`, so tests can assert on its payload. */
template <typename T> const T *find(const std::vector<LobbyEffect> &effects) {
  for (const auto &effect : effects) {
    if (std::holds_alternative<T>(effect)) {
      return &std::get<T>(effect);
    }
  }
  return nullptr;
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

TEST_CASE("lobby effects carry their payload past the model move", "[lobby]") {
  // Same regression as the session machine: reading `next.render_node` while moving `next` into
  // `LobbyTransition::model` yields "" because the move is evaluated first.
  auto model = new_lobby();

  auto t = step_lobby(model, StartLobby{});
  t = step_lobby(t.model, LobbyGpuAssigned{.render_node = "/dev/dri/renderD129"});

  auto desktop = find<StartLobbyDesktop>(t.effects);
  REQUIRE(desktop != nullptr);
  REQUIRE(desktop->render_node == "/dev/dri/renderD129");
  REQUIRE(desktop->width == 1920);
  REQUIRE(desktop->height == 1080);
  REQUIRE(desktop->refresh_rate == 60);

  t = step_lobby(t.model, LobbyDesktopReady{.wayland_socket_name = "wayland-1"});
  auto runner = find<StartLobbyRunner>(t.effects);
  REQUIRE(runner != nullptr);
  REQUIRE(runner->render_node == "/dev/dri/renderD129");
}

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

TEST_CASE("stopping a lobby detaches every connected session first", "[lobby]") {
  auto model = running_lobby();
  model = advance_lobby(model, SessionJoined{.session_id = 10});
  model = advance_lobby(model, SessionJoined{.session_id = 20});

  auto t = step_lobby(model, StopLobby{.reason = "shutdown"});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));
  REQUIRE(has<ReleaseLobbyGpu>(t.effects));

  // Both sessions are detached before the teardown.
  std::size_t detaches = 0;
  for (const auto &effect : t.effects) {
    if (std::holds_alternative<DetachSession>(effect)) {
      ++detaches;
    }
  }
  REQUIRE(detaches == 2);
  REQUIRE(std::holds_alternative<DetachSession>(t.effects.front()));
}

TEST_CASE("runner exit stops the lobby", "[lobby]") {
  auto model = running_lobby();

  auto t = step_lobby(model, LobbyRunnerExited{.exit_code = 0});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));
  REQUIRE(has<ReleaseLobbyGpu>(t.effects));

  t = step_lobby(t.model, LobbyTeardownComplete{});
  REQUIRE(t.model.state == LobbyState::Stopped);
}

/*
 * The user quitting the app makes the lobby's runner exit. Any session that joined has its
 * mouse/keyboard/touch and its audio/video producers pointed at the lobby's compositor, so it must be
 * detached *before* the compositor is dropped: otherwise the client keeps encoding from a dead
 * interpipe source (a black screen) and sends input to a wayland display that no longer exists (the
 * UI stops reacting to mouse and keyboard).
 */
TEST_CASE("runner exit detaches connected sessions before tearing down", "[lobby]") {
  auto model = running_lobby();
  model = advance_lobby(model, SessionJoined{.session_id = 10});
  model = advance_lobby(model, SessionJoined{.session_id = 20});

  auto t = step_lobby(model, LobbyRunnerExited{.exit_code = 0});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<DetachSession>(t.effects));
  REQUIRE(has<TeardownLobby>(t.effects));

  // Both sessions are detached, and every detach comes before the teardown so the sessions are
  // switched back while the shared compositor is still alive.
  std::size_t detaches = 0;
  for (const auto &effect : t.effects) {
    if (std::holds_alternative<DetachSession>(effect)) {
      ++detaches;
    }
  }
  REQUIRE(detaches == 2);
  REQUIRE(std::holds_alternative<DetachSession>(t.effects[0]));
  REQUIRE(std::holds_alternative<DetachSession>(t.effects[1]));
  REQUIRE(std::holds_alternative<TeardownLobby>(t.effects[2]));
}

TEST_CASE("every teardown path detaches the connected sessions first", "[lobby]") {
  auto with_two_sessions = [] {
    auto model = running_lobby();
    model = advance_lobby(model, SessionJoined{.session_id = 10});
    return advance_lobby(model, SessionJoined{.session_id = 20});
  };

  // An explicit stop, the runner exiting (the app quit) and a desktop failure all end the lobby, and
  // all of them have to hand the sessions back to their own desktop before the shared one goes away.
  std::vector<LobbyTransition> transitions;
  transitions.push_back(step_lobby(with_two_sessions(), StopLobby{.reason = "shutdown"}));
  transitions.push_back(step_lobby(with_two_sessions(), LobbyRunnerExited{.exit_code = 0}));
  transitions.push_back(step_lobby(with_two_sessions(), LobbyDesktopFailed{.reason = "compositor panicked"}));

  for (const auto &t : transitions) {
    // Detaches are emitted first, then the teardown, so the switch back happens against a live display.
    REQUIRE(t.effects.size() == 4);
    REQUIRE(std::holds_alternative<DetachSession>(t.effects[0]));
    REQUIRE(std::holds_alternative<DetachSession>(t.effects[1]));
    REQUIRE(std::holds_alternative<TeardownLobby>(t.effects[2]));
    REQUIRE(std::holds_alternative<ReleaseLobbyGpu>(t.effects[3]));
  }
}

/*
 * Tearing a lobby down fires a `StopLobbyEvent` so that its own video/audio producers and its runner
 * are released (they are registered under the lobby id and only listen for that event). The event bus
 * then routes the event straight back into the actor as another `StopLobby`, so the second stop has to
 * be a no-op: re-entering teardown would re-emit every `DetachSession` — re-plugging the sessions'
 * joypads into their own runners — and a second `ReleaseLobbyGpu`, double-releasing the render node.
 */
TEST_CASE("a stop that races the teardown is ignored", "[lobby]") {
  auto model = running_lobby();
  model = advance_lobby(model, SessionJoined{.session_id = 10});

  auto t = step_lobby(model, StopLobby{.reason = "stop lobby event"});
  REQUIRE(t.model.state == LobbyState::Stopping);
  REQUIRE(has<TeardownLobby>(t.effects));

  // The teardown's own `StopLobbyEvent` comes back as a second stop: it must not re-emit anything.
  auto again = step_lobby(t.model, StopLobby{.reason = "stop lobby event"});
  REQUIRE(again.model.state == LobbyState::Stopping);
  REQUIRE(again.effects.empty());

  // The teardown still completes normally.
  auto done = step_lobby(again.model, LobbyTeardownComplete{});
  REQUIRE(done.model.state == LobbyState::Stopped);
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

TEST_CASE("lobby actor drives the machine on its own thread", "[lobby]") {
  auto runtime = std::make_shared<AutoPilotLobbyRuntime>();
  // The actor must be owned by a shared_ptr: it hands the runtime a weak reference to itself.
  auto actor = std::make_shared<LobbyActor>(new_lobby(), runtime);
  actor->start();

  // Only StartLobby is posted externally; the runtime drives the rest.
  actor->post(StartLobby{});

  for (int i = 0; i < 100 && actor->snapshot().state != LobbyState::RunnerRunning; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().state == LobbyState::RunnerRunning);

  // A session joins and leaves the running lobby.
  actor->post(SessionJoined{.session_id = 10});
  for (int i = 0; i < 100 && actor->snapshot().connected_sessions.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().connected_sessions.size() == 1);

  actor->post(SessionLeft{.session_id = 10});
  for (int i = 0; i < 100 && actor->snapshot().state != LobbyState::Stopped; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // stop_when_everyone_leaves defaults to true, so the lobby stops itself.
  REQUIRE(actor->snapshot().state == LobbyState::Stopped);

  actor->stop();
}