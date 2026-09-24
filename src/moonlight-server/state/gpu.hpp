#pragma once

#include <algorithm>
#include <fmt/format.h>
#include <immer/map.hpp>
#include <platforms/hw.hpp>
#include <state/serialised_config.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace state {

using namespace wolf::config;

/**
 * A single GPU that Wolf can use.
 *
 * It bundles everything that is GPU/vendor specific: the render node, its weighted share of sessions
 * and the (already resolved) default video settings and encoders. Sessions get pinned to a GPU so that
 * rendering (the Wayland compositor) and encoding (GStreamer) happen on the same device.
 */
struct Gpu {
  std::string render_node;
  /** Relative share of sessions this GPU should receive (>= 1). */
  int weight = 1;
  GPU_VENDOR vendor = GPU_VENDOR::UNKNOWN;
  bool use_zero_copy = false;
  bool support_hevc = false;
  bool support_av1 = false;

  /** GPU defaults, per-app overrides are merged on top of these when a session is assigned. */
  BaseAppVideoOverride default_video;
  std::string h264_video_params;
  std::string hevc_video_params;
  std::string av1_video_params;
};

/** An immutable list of all the GPUs available to Wolf. */
struct GpuPool {
  std::vector<Gpu> gpus;

  bool empty() const { return gpus.empty(); }

  const Gpu *find(std::string_view render_node) const {
    auto it = std::find_if(gpus.begin(), gpus.end(), [&](const Gpu &gpu) { return gpu.render_node == render_node; });
    return it == gpus.end() ? nullptr : &*it;
  }
};

/** Immutable snapshot of how many sessions are currently pinned to each GPU. */
struct GpuAssignments {
  immer::map<std::string, int> assigned;
};

inline int assignment_count(const GpuAssignments &assignments, const std::string &render_node) {
  if (auto *count = assignments.assigned.find(render_node)) {
    return *count;
  }
  return 0;
}

/**
 * Picks the GPU with the highest `(weight - assigned)` score.
 * Ties are broken by the fewest assigned sessions, then by config order.
 */
inline const Gpu *pick_gpu(const GpuPool &pool, const GpuAssignments &assignments) {
  const Gpu *best = nullptr;
  int best_score = 0;
  int best_assigned = 0;

  for (const auto &gpu : pool.gpus) {
    auto assigned = assignment_count(assignments, gpu.render_node);
    auto score = gpu.weight - assigned;
    if (best == nullptr || score > best_score || (score == best_score && assigned < best_assigned)) {
      best = &gpu;
      best_score = score;
      best_assigned = assigned;
    }
  }
  return best;
}

inline GpuAssignments assign_gpu(const GpuAssignments &assignments, const std::string &render_node) {
  if (render_node.empty()) {
    return assignments;
  }
  return GpuAssignments{.assigned =
                            assignments.assigned.set(render_node, assignment_count(assignments, render_node) + 1)};
}

inline GpuAssignments release_gpu(const GpuAssignments &assignments, const std::string &render_node) {
  if (render_node.empty()) {
    return assignments;
  }
  auto count = assignment_count(assignments, render_node);
  if (count <= 1) {
    return GpuAssignments{.assigned = assignments.assigned.erase(render_node)};
  }
  return GpuAssignments{.assigned = assignments.assigned.set(render_node, count - 1)};
}

/** Composes a GStreamer video pipeline out of its four logical parts. */
inline std::string
build_pipeline(std::string_view source, std::string_view params, std::string_view encoder, std::string_view sink) {
  return fmt::format("{} !\n{} !\n{} !\n{}", source, params, encoder, sink);
}

/**
 * The video settings of an app, resolved against the defaults of a specific GPU.
 */
struct ResolvedVideoSettings {
  std::string h264_gst_pipeline;
  std::string hevc_gst_pipeline;
  std::string av1_gst_pipeline;
  std::string video_producer_buffer_caps;
  bool support_hevc = false;
  bool support_av1 = false;
};

/**
 * Merges an app's video overrides on top of a GPU's defaults to produce the concrete pipelines.
 */
inline ResolvedVideoSettings resolve_video_settings(const wolf::config::BaseAppVideoOverride &app, const Gpu &gpu) {
  ResolvedVideoSettings resolved;
  resolved.video_producer_buffer_caps =
      app.producer_buffer_caps.value_or(gpu.default_video.producer_buffer_caps.value_or("video/x-raw"));
  resolved.support_hevc = gpu.support_hevc;
  resolved.support_av1 = gpu.support_av1;

  auto source = app.source.value_or(gpu.default_video.source.value_or(""));
  auto sink = app.sink.value_or(gpu.default_video.sink.value_or(""));
  auto video_params = app.video_params;

  resolved.h264_gst_pipeline = build_pipeline(source,
                                              video_params.value_or(gpu.h264_video_params),
                                              app.h264_encoder.value_or(gpu.default_video.h264_encoder.value_or("")),
                                              sink);

  if (gpu.support_hevc) {
    resolved.hevc_gst_pipeline = build_pipeline(source,
                                                video_params.value_or(gpu.hevc_video_params),
                                                app.hevc_encoder.value_or(gpu.default_video.hevc_encoder.value_or("")),
                                                sink);
  }

  if (gpu.support_av1) {
    resolved.av1_gst_pipeline = build_pipeline(source,
                                               video_params.value_or(gpu.av1_video_params),
                                               app.av1_encoder.value_or(gpu.default_video.av1_encoder.value_or("")),
                                               sink);
  }

  return resolved;
}

} // namespace state