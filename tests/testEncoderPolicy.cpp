#include <catch2/catch_test_macros.hpp>

#include <platform/video/encoder_policy.hpp>

#include <set>

using namespace wolf::platform;

namespace {

/** A probe backed by a fixed set of "installed" element names. */
ElementProbe probe_with(std::set<std::string> available) {
  return [available = std::move(available)](const std::string &name) { return available.count(name) > 0; };
}

EncoderCandidate candidate(std::string plugin, std::vector<std::string> elements, std::string pipeline) {
  return EncoderCandidate{.plugin_name = std::move(plugin),
                          .check_elements = std::move(elements),
                          .encoder_pipeline = std::move(pipeline)};
}

} // namespace

TEST_CASE("plugin names map to encoder kinds", "[encoder]") {
  REQUIRE(encoder_kind_from_plugin("nvcodec") == EncoderKind::Nvidia);
  REQUIRE(encoder_kind_from_plugin("vaapi") == EncoderKind::Vaapi);
  REQUIRE(encoder_kind_from_plugin("va") == EncoderKind::Vaapi);
  REQUIRE(encoder_kind_from_plugin("qsv") == EncoderKind::QuickSync);
  REQUIRE(encoder_kind_from_plugin("applemedia") == EncoderKind::Apple);
  REQUIRE(encoder_kind_from_plugin("x264") == EncoderKind::Software);
  REQUIRE(encoder_kind_from_plugin("x265") == EncoderKind::Software);
  REQUIRE(encoder_kind_from_plugin("aom") == EncoderKind::Software);
  REQUIRE(encoder_kind_from_plugin("nonsense") == EncoderKind::Unknown);
}

TEST_CASE("vendor compatibility gates encoders", "[encoder]") {
  REQUIRE(is_vendor_compatible(EncoderKind::Nvidia, GpuVendor::Nvidia));
  REQUIRE_FALSE(is_vendor_compatible(EncoderKind::Nvidia, GpuVendor::Intel));

  REQUIRE(is_vendor_compatible(EncoderKind::Vaapi, GpuVendor::Intel));
  REQUIRE(is_vendor_compatible(EncoderKind::Vaapi, GpuVendor::Amd));
  REQUIRE_FALSE(is_vendor_compatible(EncoderKind::Vaapi, GpuVendor::Nvidia));

  REQUIRE(is_vendor_compatible(EncoderKind::QuickSync, GpuVendor::Intel));
  REQUIRE_FALSE(is_vendor_compatible(EncoderKind::QuickSync, GpuVendor::Amd));

  REQUIRE(is_vendor_compatible(EncoderKind::Software, GpuVendor::Unknown));
  REQUIRE(is_vendor_compatible(EncoderKind::Software, GpuVendor::Nvidia));
}

TEST_CASE("select_encoder picks the first compatible and available candidate", "[encoder]") {
  auto candidates = std::vector<EncoderCandidate>{
      candidate("nvcodec", {"nvh264enc"}, "nvh264enc"),
      candidate("va", {"vah264enc", "vapostproc"}, "vah264enc"),
      candidate("x264", {"x264enc"}, "x264enc"),
  };

  SECTION("NVIDIA GPU picks nvcodec") {
    auto probe = probe_with({"nvh264enc", "vah264enc", "vapostproc", "x264enc"});
    auto picked = select_encoder("h264", candidates, GpuVendor::Nvidia, "renderD128", probe);
    REQUIRE(picked.has_value());
    REQUIRE(picked->plugin_name == "nvcodec");
  }

  SECTION("Intel GPU skips nvcodec and picks vaapi") {
    auto probe = probe_with({"nvh264enc", "vah264enc", "vapostproc", "x264enc"});
    auto picked = select_encoder("h264", candidates, GpuVendor::Intel, "renderD128", probe);
    REQUIRE(picked.has_value());
    REQUIRE(picked->plugin_name == "va");
  }

  SECTION("falls through to software when hardware elements are missing") {
    auto probe = probe_with({"x264enc"});
    auto picked = select_encoder("h264", candidates, GpuVendor::Nvidia, "renderD128", probe);
    REQUIRE(picked.has_value());
    REQUIRE(picked->plugin_name == "x264");
  }

  SECTION("returns nullopt when nothing is usable") {
    auto probe = probe_with({});
    REQUIRE_FALSE(select_encoder("h264", candidates, GpuVendor::Nvidia, "renderD128", probe).has_value());
  }
}

TEST_CASE("select_encoder uses the per-device VAAPI element on non-default nodes", "[encoder]") {
  auto candidates = std::vector<EncoderCandidate>{
      candidate("va", {"vah264enc", "vapostproc"}, "vah264enc ! queue"),
  };
  // The base element must be available (as in the original get_encoder), and the per-device
  // element is preferred when present.
  auto probe = probe_with({"vah264enc", "varenderD129h264enc", "vapostproc"});

  auto picked = select_encoder("h264", candidates, GpuVendor::Intel, "renderD129", probe);
  REQUIRE(picked.has_value());
  REQUIRE(picked->check_elements == std::vector<std::string>{"varenderD129h264enc", "vapostproc"});
  REQUIRE(picked->encoder_pipeline == "varenderD129h264enc ! queue");
}

TEST_CASE("scope_pipeline_to_node rewrites only device-specific elements", "[encoder]") {
  auto no_nvidia = [](const std::string &) { return std::optional<std::string>{}; };

  SECTION("VAAPI gets a device= property") {
    auto out = scope_pipeline_to_node("vah264enc ! queue", "/dev/dri/renderD129", GpuVendor::Intel, no_nvidia);
    REQUIRE(out == "vah264enc device=/dev/dri/renderD129 ! queue");
  }

  SECTION("VAAPI device= is not added twice") {
    auto out = scope_pipeline_to_node("vah264enc device=/dev/dri/renderD129 ! queue",
                                      "/dev/dri/renderD129",
                                      GpuVendor::Intel,
                                      no_nvidia);
    REQUIRE(out == "vah264enc device=/dev/dri/renderD129 ! queue");
  }

  SECTION("NVIDIA switches to the per-device encoder element") {
    // CUDA-mode nvcodec encoders expose a read-only `cuda-device-id`; the device is baked into the
    // element class instead. Non-zero devices must be addressed by their per-device name.
    auto resolver = [](const std::string &) { return std::optional<std::string>("1"); };
    auto out = scope_pipeline_to_node("nvh264enc ! queue", "/dev/dri/renderD136", GpuVendor::Nvidia, resolver);
    REQUIRE(out == "nvh264device1enc ! queue");
  }

  SECTION("NVIDIA keeps the plain element name on CUDA device 0") {
    auto resolver = [](const std::string &) { return std::optional<std::string>("0"); };
    auto out = scope_pipeline_to_node("nvh265enc ! queue", "/dev/dri/renderD128", GpuVendor::Nvidia, resolver);
    REQUIRE(out == "nvh265enc ! queue");
  }

  SECTION("NVIDIA pins the writable cuda-device-id on upload/convert") {
    auto resolver = [](const std::string &) { return std::optional<std::string>("1"); };
    auto out = scope_pipeline_to_node("cudaupload ! cudaconvertscale add-borders=true ! queue",
                                      "/dev/dri/renderD136",
                                      GpuVendor::Nvidia,
                                      resolver);
    REQUIRE(out == "cudaupload cuda-device-id=1 ! cudaconvertscale cuda-device-id=1 add-borders=true ! queue");
  }

  SECTION("NVIDIA rewrites a full nvcodec pipeline") {
    auto resolver = [](const std::string &) { return std::optional<std::string>("1"); };
    auto out = scope_pipeline_to_node("cudaupload !\n"
                                      "cudaconvertscale add-borders=true !\n"
                                      "video/x-raw(memory:CUDAMemory), format=NV12 !\n"
                                      "nvh265enc gop-size=-1 bitrate=1000 ! h265parse",
                                      "/dev/dri/renderD136",
                                      GpuVendor::Nvidia,
                                      resolver);
    REQUIRE(out == "cudaupload cuda-device-id=1 !\n"
                   "cudaconvertscale cuda-device-id=1 add-borders=true !\n"
                   "video/x-raw(memory:CUDAMemory), format=NV12 !\n"
                   "nvh265device1enc gop-size=-1 bitrate=1000 ! h265parse");
  }

  SECTION("NVIDIA does not add cuda-device-id twice") {
    auto resolver = [](const std::string &) { return std::optional<std::string>("1"); };
    auto out = scope_pipeline_to_node("cudaupload cuda-device-id=1 ! cudaconvertscale cuda-device-id=1 add-borders=true",
                                      "/dev/dri/renderD136",
                                      GpuVendor::Nvidia,
                                      resolver);
    REQUIRE(out == "cudaupload cuda-device-id=1 ! cudaconvertscale cuda-device-id=1 add-borders=true");
  }

  SECTION("NVIDIA without a resolvable index is left unchanged") {
    auto out = scope_pipeline_to_node("nvh264enc ! queue", "/dev/dri/renderD136", GpuVendor::Nvidia, no_nvidia);
    REQUIRE(out == "nvh264enc ! queue");
  }

  SECTION("software pipelines are untouched") {
    auto out = scope_pipeline_to_node("x264enc ! queue", "/dev/dri/renderD128", GpuVendor::Unknown, no_nvidia);
    REQUIRE(out == "x264enc ! queue");
  }
}

TEST_CASE("producer_buffer_caps_for maps encoder kinds to caps", "[encoder]") {
  REQUIRE(producer_buffer_caps_for(EncoderKind::Nvidia, {}) == "video/x-raw(memory:CUDAMemory)");
  REQUIRE(producer_buffer_caps_for(EncoderKind::Software, {}) == "video/x-raw");
  REQUIRE(producer_buffer_caps_for(EncoderKind::Vaapi, {}) == "video/x-raw");
  REQUIRE(producer_buffer_caps_for(EncoderKind::Vaapi, {"NV12", "BGRA"}) ==
          "video/x-raw(memory:DMABuf), drm-format={NV12,BGRA}");
  REQUIRE(producer_buffer_caps_for(EncoderKind::QuickSync, {"NV12"}) ==
          "video/x-raw(memory:DMABuf), drm-format={NV12}");
}
