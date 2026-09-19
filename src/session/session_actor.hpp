#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <helpers/tsqueue.hpp>
#include <session/session_effect.hpp>
#include <session/session_input.hpp>
#include <session/session_model.hpp>

namespace wolf::session {

/**
 * The runtime side of a session: it interprets the effects emitted by the state machine and
 * feeds the resulting inputs back into the actor.
 *
 * The state machine never touches hardware, containers or the network. Everything with a side
 * effect goes through this interface, which is what makes the lifecycle testable (a fake runtime
 * can be driven in a unit test) and keeps the actor free of GStreamer/Docker/PulseAudio deps.
 */
class SessionRuntime {
public:
  virtual ~SessionRuntime() = default;

  /** Perform an effect. Implementations must eventually push a matching input back. */
  virtual void execute(const SessionEffect &effect) = 0;

  /**
   * Feed an input back into the owning actor.
   *
   * Runtimes call this when an asynchronous effect completes (e.g. the compositor is ready, the
   * runner exited). It is a no-op until the actor binds its sink in `start()`.
   */
  void post(SessionInput input) {
    if (sink_) {
      sink_(std::move(input));
    }
  }

  /**
   * Called by the actor to give the runtime a way to feed inputs back. Not intended to be called
   * by anything else.
   */
  void bind_input_sink(std::function<void(SessionInput)> sink) { sink_ = std::move(sink); }

private:
  std::function<void(SessionInput)> sink_;
};

/**
 * A single-threaded actor that owns one session's lifecycle.
 *
 * All state transitions happen on the actor's own thread, so `SessionModel` needs no locks and
 * there is exactly one owner of the lifecycle. Protocol adapters only ever call `post()`, which
 * is the sole thread-safe entry point.
 *
 * This replaces the old design where a session was mutated from many detached threads and
 * event-bus handlers with no single owner.
 */
class SessionActor {
public:
  SessionActor(SessionModel initial, std::shared_ptr<SessionRuntime> runtime)
      : model_(std::move(initial)), runtime_(std::move(runtime)) {}

  SessionActor(const SessionActor &) = delete;
  SessionActor &operator=(const SessionActor &) = delete;

  ~SessionActor() { stop(); }

  /** Start the actor's worker thread. */
  void start() {
    if (running_.exchange(true)) {
      return;
    }
    // Let the runtime feed inputs back into this actor.
    if (runtime_) {
      runtime_->bind_input_sink([this](SessionInput input) { post(std::move(input)); });
    }
    worker_ = std::thread([this] { run(); });
  }

  /**
   * Post an input to the actor. Thread-safe; the only way to drive the session from outside.
   */
  void post(SessionInput input) { inbox_.push(std::move(input)); }

  /**
   * Ask the actor to stop and join its thread. Idempotent.
   */
  void stop() {
    // Only enqueue a stop request if the worker is still running; if it already reached a
    // terminal state it has set `running_` to false on its own.
    if (running_.exchange(false)) {
      inbox_.push(StopRequested{.reason = "actor shutdown"});
    }
    // Always join: the worker may have exited on its own (terminal state) without us noticing.
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  /** @return a snapshot of the current model. Safe to call from any thread. */
  SessionModel snapshot() const {
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
  }

  std::uint64_t session_id() const { return model_.session_id; }

private:
  void run() {
    {
      std::lock_guard lock(snapshot_mutex_);
      snapshot_ = model_;
    }

    while (running_.load()) {
      auto input = inbox_.pop();
      if (!input.has_value()) {
        continue; // timeout: loop so we can observe `running_`
      }

      auto transition = step(model_, *input);
      model_ = std::move(transition.model);

      {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = model_;
      }

      for (const auto &effect : transition.effects) {
        runtime_->execute(effect);
      }

      if (is_terminal(model_.state)) {
        running_.store(false);
      }
    }
  }

  SessionModel model_;
  std::shared_ptr<SessionRuntime> runtime_;

  TSQueue<SessionInput> inbox_;
  std::thread worker_;
  std::atomic_bool running_{false};

  mutable std::mutex snapshot_mutex_;
  SessionModel snapshot_;
};

} // namespace wolf::session
