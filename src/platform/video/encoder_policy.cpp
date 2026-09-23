#include <platform/video/encoder_policy.hpp>

#include <algorithm>
#include <regex>

namespace wolf::platform {

EncoderKind encoder_kind_from_plugin(std::string_view plugin_name) {
  if (plugin_name == "nvcodec") {
    return EncoderKind::Nvidia;
  }
  if (plugin_name == "vaapi" || plugin_name == "va") {
    return EncoderKind::Vaapi;
  }
  if (plugin_name == "qsv") {
    return EncoderKind::QuickSync;
  }
  if (plugin_name == "applemedia") {
    return EncoderKind::Apple;
  }
  if (plugin_name == "x264" || plugin_name == "x265" || plugin_name == "aom") {
    return EncoderKind::Software;
  }
  return EncoderKind::Unknown;
}

bool is_vendor_compatible(EncoderKind kind, GpuVendor vendor) {
  switch (kind) {
  case EncoderKind::Nvidia:
    return vendor == GpuVendor::Nvidia;
  case EncoderKind::Vaapi:
    return vendor == GpuVendor::Intel || vendor == GpuVendor::Amd;
  case EncoderKind::QuickSync:
    return vendor == GpuVendor::Intel;
  case EncoderKind::Apple:
    return vendor == GpuVendor::Apple;
  case EncoderKind::Software:
    return true;
  case EncoderKind::Unknown:
    return false;
  }
  return false;
}

namespace {

/** @return true if every element the candidate needs is instantiable. */
bool elements_available(const EncoderCandidate &candidate, const ElementProbe &probe) {
  return std::all_of(candidate.check_elements.begin(), candidate.check_elements.end(), probe);
}

/**
 * VAAPI multi-device special case: on a non-default render node the per-device element is named
 * `va<node><tech>enc` (e.g. `varenderD129h264enc`). If that element exists, rewrite the candidate
 * to use it.
 */
std::optional<EncoderCandidate> try_vaapi_device_element(const EncoderCandidate &candidate,
                                                         std::string_view tech,
                                                         std::string_view render_node_name,
                                                         const ElementProbe &probe) {
  auto device_element = "va" + std::string(render_node_name) + std::string(tech) + "enc";
  EncoderCandidate scoped = candidate;
  scoped.check_elements = {device_element, "vapostproc"};
  if (!elements_available(scoped, probe)) {
    return std::nullopt;
  }
  // Point the pipeline at the per-device element.
  scoped.encoder_pipeline =
      std::regex_replace(scoped.encoder_pipeline, std::regex("va" + std::string(tech) + "enc"), device_element);
  return scoped;
}

} // namespace

std::optional<EncoderCandidate> select_encoder(std::string_view tech,
                                               const std::vector<EncoderCandidate> &candidates,
                                               GpuVendor vendor,
                                               std::string_view render_node_name,
                                               const ElementProbe &probe) {
  for (const auto &candidate : candidates) {
    auto kind = encoder_kind_from_plugin(candidate.plugin_name);
    if (!is_vendor_compatible(kind, vendor)) {
      continue;
    }
    if (!elements_available(candidate, probe)) {
      continue;
    }

    // VAAPI on a non-default node: prefer the per-device element when it exists.
    if (kind == EncoderKind::Vaapi && render_node_name != "renderD128") {
      if (auto scoped = try_vaapi_device_element(candidate, tech, render_node_name, probe)) {
        return scoped;
      }
    }

    return candidate;
  }
  return std::nullopt;
}

std::string scope_pipeline_to_node(const std::string &pipeline,
                                   const std::string &render_node,
                                   GpuVendor vendor,
                                   const NvidiaIndexResolver &nvidia_index) {
  if (vendor == GpuVendor::Unknown) {
    return pipeline;
  }

  // VAAPI / QuickSync: the encoder element takes a `device=` property pointing at the render node.
  if (vendor == GpuVendor::Intel || vendor == GpuVendor::Amd) {
    std::string result = pipeline;
    for (const auto &tech : {"h264", "h265", "av1"}) {
      for (const auto &suffix : {"enc", "lpenc"}) {
        // Match the element name, then insert `device=...` after it if not already present.
        std::regex re(std::string("(\\bva") + tech + std::string(suffix) + "\\b)(?!\\s*device=)");
        result = std::regex_replace(result, re, "$1 device=" + render_node);
      }
    }
    return result;
  }

  // NVIDIA: the CUDA-mode nvcodec encoders (nvh264enc/nvh265enc/nvav1enc) install their
  // `cuda-device-id` property as **read-only** ("CUDA device ID of associated GPU", flags: Read).
  // The device is baked into the element class when the plugin is loaded: the first CUDA device
  // registers under the plain name (`nvh265enc`) and every further device registers as a
  // per-device element (`nvh265device1enc`, `nvh264device1enc`, ...). Writing `cuda-device-id=` on
  // the plain element therefore only produces
  //   GLib-GObject-CRITICAL: property 'cuda-device-id' of object class 'GstNvH265Enc' is not writable
  // and the value is silently dropped, leaving the encoder pinned to the first GPU.
  //
  // To actually move encoding onto another GPU we switch to the per-device element instead.
  // `cudaupload`/`cudaconvertscale` own the CUDA buffers and *do* expose a writable
  // `cuda-device-id` (-1 means "auto"), so they are pinned to the same device to keep the whole
  // upload -> convert -> encode chain on one GPU.
  if (vendor == GpuVendor::Nvidia) {
    auto idx = nvidia_index(render_node);
    if (!idx) {
      return pipeline;
    }
    std::string result = pipeline;

    // CUDA device 0 is the plain element name; anything else needs the per-device element.
    if (*idx != "0") {
      for (const auto &codec : {"h264", "h265", "av1"}) {
        std::regex re(std::string("(\\bnv") + codec + std::string("enc\\b)"));
        result = std::regex_replace(result, re, "nv" + std::string(codec) + "device" + *idx + "enc");
      }
    }

    // Pin the CUDA upload/convert elements (writable property) to the same device.
    for (const auto &el : {"cudaupload", "cudaconvertscale"}) {
      std::regex re(std::string("(\\b") + el + std::string("\\b)(?!\\s*cuda-device-id=)"));
      result = std::regex_replace(result, re, "$1 cuda-device-id=" + *idx);
    }
    return result;
  }

  return pipeline;
}

std::string producer_buffer_caps_for(EncoderKind kind, const std::vector<std::string> &dma_formats) {
  switch (kind) {
  case EncoderKind::Nvidia:
    return "video/x-raw(memory:CUDAMemory)";
  case EncoderKind::Vaapi:
  case EncoderKind::QuickSync: {
    if (dma_formats.empty()) {
      return "video/x-raw";
    }
    std::string joined;
    for (const auto &format : dma_formats) {
      if (!joined.empty()) {
        joined += ",";
      }
      joined += format;
    }
    return "video/x-raw(memory:DMABuf), drm-format={" + joined + "}";
  }
  case EncoderKind::Software:
  case EncoderKind::Apple:
  case EncoderKind::Unknown:
    return "video/x-raw";
  }
  return "video/x-raw";
}

std::string shared_desktop_producer_buffer_caps(const std::string &configured_caps, bool multi_gpu) {
  if (!multi_gpu) {
    return configured_caps;
  }
  // The desktop is shared, so no single device can be assumed for every consumer.
  return "video/x-raw";
}

} // namespace wolf::platform
