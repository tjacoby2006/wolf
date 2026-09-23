#pragma once

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <helpers/logger.hpp>
#include <immer/atom.hpp>
#include <platforms/hw.hpp>

namespace state {

/**
 * Per-GPU static configuration (from the [gpus] table + excluded_gpus list).
 */
struct GpuInfo {
  int weight = 1; // Relative performance metric, user supplied. Higher = more apps before spilling over.
  bool excluded = false; // e.g. an iGPU used for the physical display
};

/**
 * Immutable snapshot of GPU load-balancing state.
 *
 * The balancer is deliberately a set of pure functions over this snapshot so it can be unit tested
 * without any GPU hardware, and so it fits Wolf's immer/atom style: callers read a snapshot, compute
 * the next one, and swap it in via `atom->update`.
 */
struct GpuBalancer {
  /** render node path (e.g. /dev/dri/renderD128) -> static info */
  std::map<std::string, GpuInfo> pool;
  /** render node path -> number of containers currently using it */
  std::map<std::string, int> usage;

  bool is_available(const std::string &node) const {
    auto it = pool.find(node);
    return it != pool.end() && !it->second.excluded;
  }

  /**
   * How many nodes a session could actually be placed on: the non-excluded pool.
   *
   * Excluded nodes are never handed out (see `pick`), so they do not make the fleet "multi-GPU" for
   * the purposes of buffer sharing; counting them would needlessly push a single-GPU host onto the
   * system-memory path.
   */
  std::size_t usable_node_count() const {
    return std::count_if(pool.begin(), pool.end(), [](const auto &entry) { return !entry.second.excluded; });
  }

  /**
   * @return true when sessions can be spread over more than one GPU.
   *
   * A shared desktop (a lobby) is consumed by several sessions, and the load balancer may place each
   * of them on a different node. The desktop's producer then cannot emit device-local memory, because
   * a consumer on another GPU could not address it (see `shared_desktop_producer_buffer_caps`).
   */
  bool is_multi_gpu() const { return usable_node_count() > 1; }

  /**
   * Pick a render node for a new container by minimising `(usage[node] + 1) / weight[node]` across
   * the non-excluded pool, so that a higher-weighted GPU (e.g. an RTX 3090 vs a 1660) genuinely
   * takes more apps before we spill over to the others.
   *
   * We score the *result* of placing this container (`usage + 1`) rather than the current load
   * (`usage`): an idle GPU always scores `0 / weight == 0` under the latter, so the second app would
   * abandon a merely-busy 4x GPU for a weak idle one no matter how the weights are set, and the
   * ratio would only emerge over a long burst. Scoring the result makes the very first pick — and
   * every one after it — respect the ratio: with weights 4 and 1 the strong GPU takes three apps
   * before the weak one gets its first, then settles into four for every one.
   *
   * Ties are broken by lowest absolute usage (so on an equal score the least-loaded GPU wins, which
   * is what keeps a just-freed GPU attractive), then by the highest weight, then by node path for
   * determinism.
   */
  std::optional<std::string> pick() const { return pick_avoiding({}); }

  /**
   * Like `pick`, but never returns one of the nodes in `avoid`.
   *
   * Callers use this to retry after a probe (`is_render_node_available`) rejects the best-scoring
   * node — e.g. a GPU that was reset or whose driver was reloaded after the pool was discovered.
   * Nodes are only skipped for the duration of the call, so the caller stays in control of whether a
   * rejected node should also have its usage counters touched (it should not: it was never acquired).
   */
  std::optional<std::string> pick_avoiding(const std::vector<std::string> &avoid) const {
    std::optional<std::string> best;
    double best_score = 0.0;
    int best_usage = 0;
    int best_weight = 0;
    for (const auto &[node, info] : pool) {
      if (info.excluded)
        continue;
      if (std::find(avoid.begin(), avoid.end(), node) != avoid.end())
        continue;
      int weight = std::max(1, info.weight);
      int current = usage.count(node) ? usage.at(node) : 0;
      double score = static_cast<double>(current + 1) / weight;
      bool better = !best.has_value() || score < best_score ||
                    (score == best_score &&
                     (current < best_usage || (current == best_usage && weight > best_weight)));
      if (better) {
        best = node;
        best_score = score;
        best_usage = current;
        best_weight = weight;
      }
    }
    return best;
  }

  /**
   * Like `pick` but treats `preferred_node` as a soft pin: use it when it's in the pool and not
   * excluded, otherwise fall back to load balancing.
   *
   * This is what a lobby uses to stay on the GPU of the session that created it (so the session's
   * encoder can read the lobby's frames) without breaking when that GPU is unavailable. A node in
   * `avoid` (one a caller already probed and rejected) is treated as unavailable, so the walk-down
   * in the runtime does not loop on the same node.
   */
  std::optional<std::string> pick_preferring(const std::optional<std::string> &preferred_node,
                                             const std::vector<std::string> &avoid = {}) const {
    auto avoided = [&avoid](const std::string &node) {
      return std::find(avoid.begin(), avoid.end(), node) != avoid.end();
    };
    if (preferred_node.has_value() && !avoided(*preferred_node)) {
      if (is_available(*preferred_node)) {
        return *preferred_node;
      }
      logs::log(logs::warning,
                "Preferred GPU {} is not in the pool or is excluded, falling back to load balancing",
                *preferred_node);
    }
    return pick_avoiding(avoid);
  }

  /** Increment the active-container count for `node`. Returns a new snapshot. */
  GpuBalancer acquire(const std::string &node) const {
    auto next = *this;
    next.usage[node] += 1;
    return next;
  }

  /** Decrement the active-container count for `node` (floored at 0). Returns a new snapshot. */
  GpuBalancer release(const std::string &node) const {
    auto next = *this;
    if (next.usage.count(node)) {
      next.usage[node] = std::max(0, next.usage[node] - 1);
    }
    return next;
  }

  /**
   * Build the initial balancer from a discovered pool of render nodes plus the user's [gpus]/excluded
   * configuration. `default_node` (WOLF_RENDER_NODE) is always included so single-GPU setups keep working.
   */
  static GpuBalancer from_pool(const std::vector<std::string> &discovered_nodes,
                               const std::map<std::string, int> &weights,
                               const std::vector<std::string> &excluded,
                               const std::string &default_node) {
    GpuBalancer balancer;
    auto add = [&](const std::string &node) {
      if (balancer.pool.count(node))
        return;
      GpuInfo info;
      if (auto w = weights.find(node); w != weights.end())
        info.weight = std::max(1, w->second);
      if (std::find(excluded.begin(), excluded.end(), node) != excluded.end())
        info.excluded = true;
      balancer.pool[node] = info;
    };

    for (const auto &node : discovered_nodes)
      add(node);
    add(default_node); // always keep the default GPU available

    // Sanity-check the [gpus] keys, then add them even when discovery did not find them. Discovery
    // is best-effort (/dev can be only partially populated inside a container) but an explicit
    // weight is user intent we must not silently drop. The runtime still probes every candidate
    // before handing it to the compositor, so a typo'd or dead path falls through to the next GPU
    // instead of failing the session.
    for (const auto &entry : weights) {
      if (!balancer.pool.count(entry.first)) {
        logs::log(logs::warning,
                  "[GPU] GPU {} from the [gpus] table was not discovered, adding it to the pool anyway",
                  entry.first);
      }
      add(entry.first);
    }
    for (const auto &node : excluded)
      add(node); // a node that is unknown-but-excluded stays out of the pool either way

    return balancer;
  }
};

/**
 * Atomically pick a render node, verify it and reserve it, in a single `atom->update`.
 *
 * This is the only entry point runtimes should use to claim a GPU. Picking and acquiring must not be
 * separate steps: `pick()` reads a snapshot and `acquire()` swaps in a new one, so two sessions
 * starting at the same moment could both read the same free GPU and be handed the same node even
 * though there was a second, equally good one available. Doing the whole pick/probe/acquire cycle
 * inside one `update` serialises concurrent callers through immer's atom.
 *
 * The node is also probed before being reserved, because the pool is discovered once at startup and
 * can go stale (GPU reset, driver reload). Handing a dead node to the virtual compositor makes it
 * panic and abort Wolf. A candidate that fails the probe is skipped for the rest of the call and its
 * usage is left untouched (it was never acquired), so a single bad GPU no longer fails the session
 * and, crucially, no longer poisons every later pick by looking permanently idle.
 *
 * @param choose  handed the candidate snapshot and the nodes rejected so far; returns the next
 *                candidate to try, or nullopt to give up (e.g. the pool is exhausted)
 * @param probe   availability check, injectable so the logic can be unit tested without hardware
 * @return the reserved node, or nullopt when no node in the pool is usable
 */
inline std::optional<std::string> pick_and_acquire(
    const std::shared_ptr<immer::atom<GpuBalancer>> &balancer,
    const std::function<std::optional<std::string>(const GpuBalancer &, const std::vector<std::string> &)> &choose,
    const std::function<bool(const std::string &)> &probe = is_render_node_available) {
  std::optional<std::string> chosen;
  balancer->update([&](const GpuBalancer &bal) {
    // `update` may run this more than once (immer's atom is transactional/lock-free), so every
    // attempt starts from a clean slate: a stale value from an aborted pass must not leak out.
    chosen.reset();
    std::vector<std::string> rejected;
    // Bounded by the pool size: every iteration rejects one distinct node.
    for (std::size_t attempt = 0; attempt <= bal.pool.size(); ++attempt) {
      auto candidate = choose(bal, rejected);
      if (!candidate.has_value()) {
        break;
      }
      if (probe(*candidate)) {
        chosen = *candidate;
        return bal.acquire(*candidate);
      }
      logs::log(logs::error, "[GPU] Render node {} is not available, trying the next one", *candidate);
      rejected.push_back(*candidate);
    }
    return bal; // nothing usable: leave the snapshot untouched
  });
  return chosen;
}

/**
 * Discover render nodes available on this system.
 *
 * Three enumeration sources are merged (each may see nodes the others miss inside a container):
 *   1. A scan of /dev/dri for renderD* entries.
 *   2. A scan of /sys/class/dri for cardN entries, mapped to their render node via the
 *      sysfs `device/renderD*` symlink (works when /dev is only partially populated).
 *   3. The kernel DRM subsystem query (libdrm drmGetDevices2).
 *
 * Every candidate is then validated with probe_render_node(), which opens the node and calls
 * drmGetDevice2(). This deliberately does NOT rely on std::filesystem::exists(): inside containers
 * (e.g. Unraid / bind-mounted /dev) a device node can be fully usable via open() while exists()
 * reports false — that mismatch is exactly what made discovery silently return 0 nodes even though
 * the GPU was perfectly streamable. Returns an empty list on non-Linux, in which case callers fall
 * back to the configured default node only.
 */
inline std::vector<std::string> discover_render_nodes() {
  std::vector<std::string> candidates;
  std::error_code ec;

  // 1) Filesystem scan of /dev/dri for renderD* entries. We do NOT gate on exists() here: the
  //    directory_iterator already yields real entries, and probe_render_node() below is the
  //    authoritative check (exists() is unreliable for bind-mounted device nodes in containers).
  auto dri = std::filesystem::path("/dev/dri");
  if (std::filesystem::exists(dri, ec) && !ec) {
    for (const auto &entry : std::filesystem::directory_iterator(dri, ec)) {
      if (ec)
        break;
      auto name = entry.path().filename().string();
      if (name.rfind("renderD", 0) == 0) {
        candidates.push_back(entry.path().string());
      }
    }
  }

  // 2) Sysfs scan of /sys/class/dri for cardN entries, mapped to their render node. The kernel
  //    exposes each GPU's render node as a symlink under the card's device dir (renderD128 etc.).
  //    This finds nodes even when /dev is only partially populated inside a container.
  auto sys_dri = std::filesystem::path("/sys/class/dri");
  if (std::filesystem::exists(sys_dri, ec) && !ec) {
    for (const auto &entry : std::filesystem::directory_iterator(sys_dri, ec)) {
      if (ec)
        break;
      auto name = entry.path().filename().string();
      if (name.rfind("card", 0) != 0)
        continue; // only cardN entries
      auto device_dir = entry.path() / "device";
      for (const auto &render_entry : std::filesystem::directory_iterator(device_dir, ec)) {
        if (ec)
          break;
        auto render_name = render_entry.path().filename().string();
        if (render_name.rfind("renderD", 0) == 0) {
          candidates.push_back("/dev/dri/" + render_name);
        }
      }
    }
  }

  // 3) Kernel DRM subsystem query (libdrm). May surface nodes not visible via the filesystem scans.
  for (const auto &node : discover_dri_render_nodes()) {
    candidates.push_back(node);
  }

  // Validate each unique candidate by actually opening it as a DRM render node. This is the single
  // source of truth for "is this GPU usable right now" and replaces the unreliable exists() gate.
  std::vector<std::string> nodes;
  auto add_if_valid = [&](const std::string &node) {
    if (std::find(nodes.begin(), nodes.end(), node) != nodes.end())
      return;
    if (probe_render_node(node)) {
      nodes.push_back(node);
    } else {
      logs::log(logs::debug, "[GPU] Skipping candidate render node {} (not openable as a DRM device)", node);
    }
  };

  for (const auto &node : candidates) {
    add_if_valid(node);
  }

  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

} // namespace state
