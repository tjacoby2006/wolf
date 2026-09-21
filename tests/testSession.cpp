#include <catch2/catch_test_macros.hpp>

#include <session/session_actor.hpp>
#include <session/session_model.hpp>

using namespace wolf::session;

namespace {

/** A fake runtime that records the effects it was asked to perform. */
class RecordingRuntime : public SessionRuntime {
public:
  void execute(const SessionEffect &effect) override { effects.push_back(effect); }

  std::vector<SessionEffect> effects;
};

/**
 * A runtime that reacts to effects by feeding inputs back, like the real runtime does. Used to
 * prove the actor <-> runtime feedback loop drives the session to completion on its own.
 */
class AutoPilotRuntime : public SessionRuntime {
public:
  void execute(const SessionEffect &effect) override {
    std::visit(
        [this](const auto &e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, AssignGpu>) {
            post(GpuAssigned{.render_node = "/dev/dri/renderD128"});
          } else if constexpr (std::is_same_v<T, StartDesktop>) {
            post(DesktopReady{.wayland_socket_name = "wayland-1"});
          } else if constexpr (std::is_same_v<T, StartRunner>) {
            post(RunnerStarted{});
          } else if constexpr (std::is_same_v<T, Teardown>) {
            post(TeardownComplete{});
          }
        },
        effect);
  }
};

SessionModel new_session(std::uint64_t id = 1) {
  SessionModel model;
  model.session_id = id;
  model.width = 1920;
  model.height = 1080;
  model.refresh_rate = 60;
  return model;
}

template <typename T> bool has(const std::vector<SessionEffect> &effects) {
  for (const auto &effect : effects) {
    if (std::holds_alternative<T>(effect)) {
      return true;
    }
  }
  return false;
}

/** @return the first effect of type `T`, so tests can assert on its payload. */
template <typename T> const T *find(const std::vector<SessionEffect> &effects) {
  for (const auto &effect : effects) {
    if (std::holds_alternative<T>(effect)) {
      return &std::get<T>(effect);
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("session walks the happy path in order", "[session]") {
  auto model = new_session();

  // Starting the session asks the runtime to pick a GPU.
  auto t = step(model, StartSession{});
  REQUIRE(t.model.state == SessionState::Created);
  REQUIRE(has<AssignGpu>(t.effects));

  t = step(t.model, GpuAssigned{.render_node = "/dev/dri/renderD128"});
  REQUIRE(t.model.state == SessionState::DesktopStarting);
  REQUIRE(t.model.render_node == "/dev/dri/renderD128");
  REQUIRE(has<StartDesktop>(t.effects));

  t = step(t.model, DesktopReady{.wayland_socket_name = "wayland-1"});
  REQUIRE(t.model.state == SessionState::RunnerStarting);
  REQUIRE(t.model.wayland_socket_name == "wayland-1");
  REQUIRE(has<StartRunner>(t.effects));

  t = step(t.model, RunnerStarted{});
  REQUIRE(t.model.state == SessionState::RunnerRunning);

  t = step(t.model, RtpPingReceived{.client_ip = "10.0.0.2", .client_port = 48000});
  REQUIRE(t.model.state == SessionState::Streaming);
  REQUIRE(t.model.client_ip == "10.0.0.2");
  REQUIRE(has<StartStreaming>(t.effects));

  t = step(t.model, StopRequested{.reason = "client left"});
  REQUIRE(t.model.state == SessionState::Stopping);
  REQUIRE(has<Teardown>(t.effects));
  REQUIRE(has<ReleaseGpu>(t.effects));

  t = step(t.model, TeardownComplete{});
  REQUIRE(t.model.state == SessionState::Stopped);
  REQUIRE(is_terminal(t.model.state));
}

TEST_CASE("effects carry their payload past the model move", "[session]") {
  // Regression: the assignment and ping transitions build `Transition{model = std::move(next),
  // effects = {...}}`. `Transition::model` is declared before `Transition::effects`, so the move
  // runs first and any field read off `next` while building the effects is moved-from ("" for a
  // std::string). That silently produced `waylanddisplaysrc ... render_node=` (empty), which made
  // gst-wayland-display abort with "Failed to open drm node !". Assert the payloads are intact.
  auto model = new_session();

  auto t = step(model, StartSession{});
  t = step(t.model, GpuAssigned{.render_node = "/dev/dri/renderD129"});

  auto desktop = find<StartDesktop>(t.effects);
  REQUIRE(desktop != nullptr);
  REQUIRE(desktop->render_node == "/dev/dri/renderD129");
  REQUIRE(desktop->width == 1920);
  REQUIRE(desktop->height == 1080);
  REQUIRE(desktop->refresh_rate == 60);

  t = step(t.model, DesktopReady{.wayland_socket_name = "wayland-1"});
  auto runner = find<StartRunner>(t.effects);
  REQUIRE(runner != nullptr);
  REQUIRE(runner->render_node == "/dev/dri/renderD129");

  t = step(t.model, RunnerStarted{});
  t = step(t.model, RtpPingReceived{.client_ip = "10.0.0.2", .client_port = 48000});
  auto streaming = find<StartStreaming>(t.effects);
  REQUIRE(streaming != nullptr);
  REQUIRE(streaming->client_ip == "10.0.0.2");
  REQUIRE(streaming->client_port == 48000);
}

TEST_CASE("pause and resume round-trip", "[session]") {
  auto model = new_session();
  model.state = SessionState::Streaming;

  auto t = step(model, PauseRequested{});
  REQUIRE(t.model.state == SessionState::Paused);
  REQUIRE(has<PauseStreaming>(t.effects));

  t = step(t.model, ResumeRequested{});
  REQUIRE(t.model.state == SessionState::Streaming);
  REQUIRE(has<ResumeStreaming>(t.effects));
}

TEST_CASE("desktop failure is fatal and tears down", "[session]") {
  auto model = new_session();
  model.state = SessionState::DesktopStarting;

  auto t = step(model, DesktopFailed{.reason = "compositor panicked"});
  REQUIRE(t.model.state == SessionState::Failed);
  REQUIRE(t.model.failure_reason == "compositor panicked");
  REQUIRE(has<Teardown>(t.effects));
  REQUIRE(has<ReleaseGpu>(t.effects));
}

TEST_CASE("runner exit stops the session", "[session]") {
  auto model = new_session();
  model.state = SessionState::Streaming;

  auto t = step(model, RunnerExited{.exit_code = 0});
  REQUIRE(t.model.state == SessionState::Stopping);
  REQUIRE(has<Teardown>(t.effects));
}

TEST_CASE("out-of-order inputs are ignored, not fatal", "[session]") {
  auto model = new_session(); // Created

  // A ping before the runner is up must not advance the state.
  auto t = step(model, RtpPingReceived{.client_ip = "10.0.0.2", .client_port = 1});
  REQUIRE(t.model.state == SessionState::Created);
  REQUIRE(t.effects.empty());

  // A desktop-ready before a GPU is assigned must not advance the state.
  t = step(model, DesktopReady{.wayland_socket_name = "wayland-1"});
  REQUIRE(t.model.state == SessionState::Created);
  REQUIRE(t.effects.empty());
}

TEST_CASE("terminal states never transition again", "[session]") {
  auto model = new_session();
  model.state = SessionState::Stopped;

  auto t = step(model, GpuAssigned{.render_node = "/dev/dri/renderD128"});
  REQUIRE(t.model.state == SessionState::Stopped);
  REQUIRE(t.effects.empty());

  model.state = SessionState::Failed;
  t = step(model, StopRequested{.reason = "again"});
  REQUIRE(t.model.state == SessionState::Failed);
  REQUIRE(t.effects.empty());
}

TEST_CASE("idr and device hotplug emit effects without changing state", "[session]") {
  auto model = new_session();
  model.state = SessionState::Streaming;

  auto t = step(model, IdrRequested{});
  REQUIRE(t.model.state == SessionState::Streaming);
  REQUIRE(has<RequestIdr>(t.effects));

  t = step(t.model, DevicePlugged{.device_id = "gamepad-1"});
  REQUIRE(t.model.state == SessionState::Streaming);
  REQUIRE(has<PlugDevice>(t.effects));

  t = step(t.model, DeviceUnplugged{.device_id = "gamepad-1"});
  REQUIRE(t.model.state == SessionState::Streaming);
  REQUIRE(has<UnplugDevice>(t.effects));
}

TEST_CASE("actor drives the machine on its own thread", "[session]") {
  auto runtime = std::make_shared<RecordingRuntime>();
  // The actor must be owned by a shared_ptr: it hands the runtime a weak reference to itself.
  auto actor = std::make_shared<SessionActor>(new_session(42), runtime);
  actor->start();

  actor->post(GpuAssigned{.render_node = "/dev/dri/renderD128"});
  actor->post(DesktopReady{.wayland_socket_name = "wayland-1"});
  actor->post(RunnerStarted{});
  actor->post(RtpPingReceived{.client_ip = "10.0.0.2", .client_port = 48000});
  // Wait for the actor to catch up.
  for (int i = 0; i < 100 && actor->snapshot().state != SessionState::Streaming; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().state == SessionState::Streaming);

  actor->post(StopRequested{.reason = "done"});
  // The runtime performs the teardown and reports back when it is done.
  actor->post(TeardownComplete{});
  for (int i = 0; i < 100 && actor->snapshot().state != SessionState::Stopped; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().state == SessionState::Stopped);

  actor->stop();
  REQUIRE(has<StartDesktop>(runtime->effects));
  REQUIRE(has<StartRunner>(runtime->effects));
  REQUIRE(has<StartStreaming>(runtime->effects));
  REQUIRE(has<Teardown>(runtime->effects));
}

TEST_CASE("the actor reports when it finishes on its own", "[session]") {
  auto runtime = std::make_shared<AutoPilotRuntime>();
  auto actor = std::make_shared<SessionActor>(new_session(9), runtime);

  auto finished = std::make_shared<std::atomic_bool>(false);
  actor->set_on_finished([finished]() { finished->store(true); });

  actor->start();
  actor->post(StartSession{});

  // Drive the session to Streaming, then stop it: the actor reaches a terminal state and reports.
  for (int i = 0; i < 100 && actor->snapshot().state != SessionState::RunnerRunning; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  actor->post(StopRequested{.reason = "done"});

  for (int i = 0; i < 100 && !finished->load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(finished->load());

  actor->stop();
}

TEST_CASE("runtime feedback drives the session to completion", "[session]") {
  // The runtime reacts to each effect by posting the matching input, so the actor should walk the
  // whole lifecycle without any external input beyond the initial GPU assignment.
  auto runtime = std::make_shared<AutoPilotRuntime>();
  auto actor = std::make_shared<SessionActor>(new_session(7), runtime);
  actor->start();

  actor->post(StartSession{});

  for (int i = 0; i < 100 && actor->snapshot().state != SessionState::RunnerRunning; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().state == SessionState::RunnerRunning);

  actor->post(StopRequested{.reason = "done"});
  for (int i = 0; i < 100 && actor->snapshot().state != SessionState::Stopped; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(actor->snapshot().state == SessionState::Stopped);

  actor->stop();
}
