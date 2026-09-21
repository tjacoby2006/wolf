#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <helpers/tsqueue.hpp>

namespace wolf::session {

/**
 * The result of a single state-machine transition: the new model plus the effects to perform.
 *
 * Generic over the model and effect types so the session and lobby machines can share it.
 */
template <typename Model, typename Effect> struct StateTransition {
  Model model;
  std::vector<Effect> effects;
};

/**
 * A pure state machine: a total `step` function plus a predicate telling the actor when the model
 * has reached a terminal state (at which point the actor stops itself).
 */
template <typename Model, typename Input, typename Effect> struct Machine {
  std::function<StateTransition<Model, Effect>(const Model &, const Input &)> step;
  std::function<bool(const Model &)> is_terminal;
};

template <typename Model, typename Input, typename Effect> class Actor;

/**
 * The runtime side of an actor: it interprets the effects emitted by the state machine and feeds
 * the resulting inputs back into the actor.
 *
 * The state machine never touches hardware, containers or the network. Everything with a side
 * effect goes through this interface, which is what makes the lifecycle testable (a fake runtime
 * can be driven in a unit test) and keeps the actor free of GStreamer/Docker/PulseAudio deps.
 *
 * Lifetime: a runtime may spawn detached threads that outlive the actor (e.g. the compositor
 * startup or the runner, which blocks for the container's lifetime). Those threads must hold a
 * `shared_ptr` to the runtime (via `shared_from_this()`) so it stays alive, and `post()` holds
 * only a *weak* reference to the actor, so a late completion after the actor was torn down is
 * safely dropped instead of dereferencing a destroyed actor.
 */
template <typename Model, typename Input, typename Effect>
class ActorRuntime : public std::enable_shared_from_this<ActorRuntime<Model, Input, Effect>> {
public:
  virtual ~ActorRuntime() = default;

  /** Perform an effect. Implementations must eventually push a matching input back. */
  virtual void execute(const Effect &effect) = 0;

  /**
   * Feed an input back into the owning actor.
   *
   * Safe to call from any thread, including after the actor has been destroyed (in which case it
   * is a no-op). Runtimes call this when an asynchronous effect completes.
   */
  void post(Input input) {
    if (auto actor = actor_.lock()) {
      actor->post(std::move(input));
    }
  }

  /**
   * Called by the actor to give the runtime a way to feed inputs back. Not intended to be called
   * by anything else.
   */
  void bind_actor(std::weak_ptr<Actor<Model, Input, Effect>> actor) { actor_ = std::move(actor); }

private:
  std::weak_ptr<Actor<Model, Input, Effect>> actor_;
};

/**
 * A single-threaded actor that owns one lifecycle (a stream session, a lobby, ...).
 *
 * All state transitions happen on the actor's own thread, so the model needs no locks and there is
 * exactly one owner of the lifecycle. Protocol adapters only ever call `post()`, which is the sole
 * thread-safe entry point.
 *
 * This replaces the old design where a session/lobby was mutated from many detached threads and
 * event-bus handlers with no single owner.
 */
template <typename Model, typename Input, typename Effect>
class Actor : public std::enable_shared_from_this<Actor<Model, Input, Effect>> {
public:
  Actor(Model initial, std::shared_ptr<ActorRuntime<Model, Input, Effect>> runtime, Machine<Model, Input, Effect> machine)
      : model_(std::move(initial)), runtime_(std::move(runtime)), machine_(std::move(machine)) {}

  Actor(const Actor &) = delete;
  Actor &operator=(const Actor &) = delete;

  ~Actor() { stop(); }

  /** Start the actor's worker thread. */
  void start() {
    if (running_.exchange(true)) {
      return;
    }
    // Let the runtime feed inputs back into this actor. The runtime holds a weak reference, so a
    // late completion after this actor is destroyed is safely dropped.
    if (runtime_) {
      runtime_->bind_actor(this->weak_from_this());
    }
    worker_ = std::thread([this] { run(); });
  }

  /** Post an input to the actor. Thread-safe; the only way to drive the lifecycle from outside. */
  void post(Input input) { inbox_.push(std::move(input)); }

  /** Ask the actor to stop and join its thread. Idempotent. */
  void stop() {
    // Only enqueue a stop request if the worker is still running; if it already reached a
    // terminal state it has set `running_` to false on its own.
    if (running_.exchange(false)) {
      inbox_.push(stop_input_);
    }
    // Always join: the worker may have exited on its own (terminal state) without us noticing.
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  /** @return a snapshot of the current model. Safe to call from any thread. */
  Model snapshot() const {
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
  }

  /**
   * The input posted by `stop()` to wake the worker. Defaults to a default-constructed `Input`;
   * callers whose `Input` has no meaningful default can override it.
   */
  void set_stop_input(Input input) { stop_input_ = std::move(input); }

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

      auto transition = machine_.step(model_, *input);
      model_ = std::move(transition.model);

      {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = model_;
      }

      for (const auto &effect : transition.effects) {
        runtime_->execute(effect);
      }

      if (machine_.is_terminal(model_)) {
        running_.store(false);
      }
    }
  }

  Model model_;
  std::shared_ptr<ActorRuntime<Model, Input, Effect>> runtime_;
  Machine<Model, Input, Effect> machine_;

  TSQueue<Input> inbox_;
  std::thread worker_;
  std::atomic_bool running_{false};
  Input stop_input_{};

  mutable std::mutex snapshot_mutex_;
  Model snapshot_;
};

} // namespace wolf::session