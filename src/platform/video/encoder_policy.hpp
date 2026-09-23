#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wolf::platform {

/**
 * GPU vendors Wolf knows how to encode on.
 *
 * This is the platform module's own enum so the encoder policy has no dependency on the
 * server-side hardware probing (`platforms/hw.hpp`). The server maps its detected vendor onto
 * this enum at the boundary.
 */
enum class GpuVendor {
  Nvidia,
  Amd,
  Intel,
  Apple,
  Unknown,
};

/**
 * The kind of encoder a GStreamer plugin represents.
 *
 * Replaces the old `Encoder` enum + `encoder_type()` switch on hashed plugin names.
 */
enum class EncoderKind {
  Nvidia,
  Vaapi,
  QuickSync,
  Software,
  Apple,
  Unknown,
};

constexpr std::string_view to_string(GpuVendor vendor) {
  switch (vendor) {
  case GpuVendor::Nvidia:
    return "NVIDIA";
  case GpuVendor::Amd:
    return "AMD";
  case GpuVendor::Intel:
    return "Intel";
  case GpuVendor::Apple:
    return "Apple";
  case GpuVendor::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

constexpr std::string_view to_string(EncoderKind kind) {
  switch (kind) {
  case EncoderKind::Nvidia:
    return "NVIDIA";
  case EncoderKind::Vaapi:
    return "VAAPI";
  case EncoderKind::QuickSync:
    return "QuickSync";
  case EncoderKind::Software:
    return "Software";
  case EncoderKind::Apple:
    return "Apple";
  case EncoderKind::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

/**
 * Map a GStreamer plugin name (from config.toml) to an encoder kind.
 *
 * This is the single place that knows plugin-name spellings; everything else works in terms of
 * `EncoderKind`. Unknown names return `EncoderKind::Unknown` (the caller logs and skips).
 */
EncoderKind encoder_kind_from_plugin(std::string_view plugin_name);

/**
 * @return true if `kind` can run on `vendor`.
 *
 * This is the vendor gate that used to be an if/else chain inside `is_available()`:
 *   - NVIDIA encoders need an NVIDIA GPU
 *   - VAAPI needs Intel or AMD
 *   - QuickSync needs Intel
 *   - Software and Apple are handled separately (software always compatible; Apple needs Apple)
 */
bool is_vendor_compatible(EncoderKind kind, GpuVendor vendor);

/**
 * A candidate encoder as declared in config.toml.
 *
 * Mirrors `wolf::config::GstEncoder` but lives in the platform module so the policy is testable
 * without pulling in the config/serialization stack.
 */
struct EncoderCandidate {
  std::string plugin_name;
  std::vector<std::string> check_elements;
  std::optional<std::string> video_params;
  std::optional<std::string> video_params_zero_copy;
  std::string encoder_pipeline;
};

/**
 * Injected probe: "can GStreamer instantiate this element right now?".
 *
 * Injecting it keeps the policy pure and unit-testable (tests pass a lambda over a set of
 * available element names) while production passes a real `gst_element_factory_make` check.
 */
using ElementProbe = std::function<bool(const std::string &element_name)>;

/**
 * Pick the first candidate that is both vendor-compatible and instantiable.
 *
 * Replaces `get_encoder()`. `tech` is the codec ("h264"/"h265"/"av1") and `render_node_name` is
 * the bare node name (e.g. "renderD129"), used for the VAAPI multi-device special case where the
 * per-device element is named `va<node><tech>enc` (e.g. `varenderD129h264enc`).
 *
 * Returns `std::nullopt` when nothing is usable.
 */
std::optional<EncoderCandidate> select_encoder(std::string_view tech,
                                               const std::vector<EncoderCandidate> &candidates,
                                               GpuVendor vendor,
                                               std::string_view render_node_name,
                                               const ElementProbe &probe);

/**
 * Resolve a render node (e.g. /dev/dri/renderD136) to an NVIDIA device index (e.g. "1").
 * Injected so the policy does not depend on libpci/libdrm.
 */
using NvidiaIndexResolver = std::function<std::optional<std::string>(const std::string &render_node)>;

/**
 * Re-point a video pipeline's encoder at a specific render node.
 *
 * Replaces `apply_encoder_node()`. Only the parts of the pipeline that name a device are
 * rewritten:
 *   - VAAPI/QuickSync encoders take a `device=` property.
 *   - NVIDIA nvcodec is handled by swapping in the per-device encoder element
 *     (`nvh265device1enc` for CUDA device 1, etc.) and pinning the writable
 *     `cuda-device-id` on `cudaupload`/`cudaconvertscale`.
 * Software encoders and unrecognised pipelines are returned unchanged.
 */
std::string scope_pipeline_to_node(const std::string &pipeline,
                                   const std::string &render_node,
                                   GpuVendor vendor,
                                   const NvidiaIndexResolver &nvidia_index);

/**
 * The producer buffer caps to use for zero-copy with a given encoder kind.
 *
 * Replaces the `switch (video_encoder)` block in `configTOML.cpp`:
 *   - NVIDIA -> CUDA memory caps
 *   - VAAPI/QuickSync -> DMABuf caps built from the supported `vapostproc` formats
 *   - everything else -> plain `video/x-raw` (no zero copy)
 *
 * `dma_formats` is the list of DRM formats the VAAPI post-processor supports; pass an empty list
 * to fall back to `video/x-raw`.
 */
std::string producer_buffer_caps_for(EncoderKind kind, const std::vector<std::string> &dma_formats);

/**
 * The caps a *shared* desktop's `waylanddisplaysrc` should emit.
 *
 * The zero-copy caps (`CUDAMemory`/`DMABuf`) are device-local: the buffer belongs to one GPU, and an
 * encoder scoped to another GPU cannot address it (which is how a joining client ends up with a black
 * screen even though negotiation "succeeds"). A shared desktop — a lobby — is consumed by several
 * sessions, and the load balancer is free to place each of them on a different GPU, so it cannot
 * assume any single device for its consumers.
 *
 * On a single-GPU host there is only one device to share, so the configured (zero-copy) caps are kept
 * verbatim. As soon as the pool can place more than one session on a different node, the desktop falls
 * back to GPU-agnostic system memory (`video/x-raw`): each consumer then uploads to its own device
 * (`cudaupload`, `vapostproc`), which is exactly the non-zero-copy path the encoders already support.
 *
 * @param configured_caps  the caps the encoder kind would normally want (may be zero-copy)
 * @param multi_gpu        true when more than one GPU can host a session
 */
std::string shared_desktop_producer_buffer_caps(const std::string &configured_caps, bool multi_gpu);

} // namespace wolf::platform
