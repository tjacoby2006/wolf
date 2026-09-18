#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <helpers/logger.hpp>

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
   * Pick a render node for a new container.
   *
   * If `pinned_node` is set, that node is used (provided it exists and isn't excluded).
   * Otherwise we minimise `usage[node] / weight[node]` across the non-excluded pool so that:
   *  - a now-free GPU (usage dropped to 0) is preferred over round-robin, and
   *  - a higher-weighted GPU (e.g. an RTX 3090 vs a 1660) takes more apps before we spill over.
   * Ties are broken by lowest absolute usage, then by node path for determinism.
   */
  std::optional<std::string> pick(const std::optional<std::string> &pinned_node) const {
    if (pinned_node.has_value()) {
      auto node = *pinned_node;
      if (is_available(node)) {
        return node;
      }
      logs::log(logs::error, "Requested GPU {} is not in the pool or is excluded", node);
      return std::nullopt;
    }

    std::optional<std::string> best;
    double best_score = 0.0;
    int best_usage = 0;
    for (const auto &[node, info] : pool) {
      if (info.excluded)
        continue;
      int current = usage.count(node) ? usage.at(node) : 0;
      double score = static_cast<double>(current) / std::max(1, info.weight);
      bool better = !best.has_value() || score < best_score || (score == best_score && current < best_usage);
      if (better) {
        best = node;
        best_score = score;
        best_usage = current;
      }
    }
    return best;
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

    return balancer;
  }
};

/**
 * Discover render nodes under /dev/dri (renderD*). Returns an empty list on non-Linux or when the
 * directory is missing, in which case callers fall back to the configured default node only.
 */
inline std::vector<std::string> discover_render_nodes() {
  std::vector<std::string> nodes;
  std::error_code ec;
  auto dri = std::filesystem::path("/dev/dri");
  if (!std::filesystem::exists(dri, ec) || ec)
    return nodes;

  for (const auto &entry : std::filesystem::directory_iterator(dri, ec)) {
    if (ec)
      break;
    auto name = entry.path().filename().string();
    if (name.rfind("renderD", 0) == 0 && std::filesystem::is_regular_file(entry.path(), ec) && !ec) {
      nodes.push_back(entry.path().string());
    }
  }
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

} // namespace state
