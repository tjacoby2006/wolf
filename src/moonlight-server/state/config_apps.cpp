#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <gst/gstelementfactory.h>
#include <gst/gstregistry.h>
#include <platform/video/encoder_policy.hpp>
#include <platforms/hw.hpp>
#include <range/v3/view.hpp>
#include <state/config.hpp>
#include <state/config_internal.hpp>

namespace state {

/*
 * App parsing: turning the TOML app definitions into `events::App` records with fully-built
 * GStreamer pipelines.
 *
 * Extracted from `configTOML.cpp` so the config loader stays focused on loading/migrating the file
 * and this file owns the (long) pipeline-string assembly.
 */

using namespace wolf::config;

/**
 * Map the server-side hardware vendor enum onto the platform module's vendor enum.
 * This is the single boundary between hardware probing and the encoder policy.
 */
static wolf::platform::GpuVendor to_platform_vendor(GPU_VENDOR vendor) {
  switch (vendor) {
  case GPU_VENDOR::NVIDIA:
    return wolf::platform::GpuVendor::Nvidia;
  case GPU_VENDOR::AMD:
    return wolf::platform::GpuVendor::Amd;
  case GPU_VENDOR::INTEL:
    return wolf::platform::GpuVendor::Intel;
  default:
    return wolf::platform::GpuVendor::Unknown;
  }
}

/** Adapt a config `GstEncoder` to the platform module's `EncoderCandidate`. */
static wolf::platform::EncoderCandidate to_candidate(const GstEncoder &settings) {
  return wolf::platform::EncoderCandidate{.plugin_name = settings.plugin_name,
                                          .check_elements = settings.check_elements,
                                          .video_params = settings.video_params,
                                          .video_params_zero_copy = settings.video_params_zero_copy,
                                          .encoder_pipeline = settings.encoder_pipeline};
}

/** Adapt a platform `EncoderCandidate` back to a config `GstEncoder`. */
static GstEncoder to_gst_encoder(const wolf::platform::EncoderCandidate &candidate) {
  return GstEncoder{.plugin_name = candidate.plugin_name,
                    .check_elements = candidate.check_elements,
                    .video_params = candidate.video_params,
                    .video_params_zero_copy = candidate.video_params_zero_copy,
                    .encoder_pipeline = candidate.encoder_pipeline};
}

/**
 * The real GStreamer probe: can this element be instantiated right now?
 * This is the only impure part of encoder selection; the policy itself is pure and unit-tested.
 */
static bool gst_element_available(const std::string &element_name) {
  if (auto el = gst_element_factory_make(element_name.c_str(), nullptr)) {
    gst_object_unref(el);
    return true;
  }
  return false;
}

/**
 * Pick the best encoder for a codec, delegating the vendor/availability policy to the platform
 * module. Kept as a thin adapter so the rest of the server keeps its existing call shape.
 */
std::optional<GstEncoder> get_encoder(std::string_view tech,
                                      std::string_view encoder_node,
                                      const std::vector<GstEncoder> &encoders,
                                      const GPU_VENDOR &vendor) {  std::vector<wolf::platform::EncoderCandidate> candidates;
  candidates.reserve(encoders.size());
  for (const auto &encoder : encoders) {
    candidates.push_back(to_candidate(encoder));
  }

  auto picked = wolf::platform::select_encoder(tech,
                                               candidates,
                                               to_platform_vendor(vendor),
                                               get_render_node_name(encoder_node),
                                               gst_element_available);
  if (!picked) {
    return std::nullopt;
  }

  auto kind = wolf::platform::encoder_kind_from_plugin(picked->plugin_name);
  if (kind == wolf::platform::EncoderKind::Software) {
    logs::log(logs::warning, "Software {} encoder detected", tech);
  } else {
    logs::log(logs::info, "Using {} encoder: {}", tech, picked->plugin_name);
  }
  return to_gst_encoder(*picked);
}

/**
 * ID is used by Moonlight to uniquely identify the app.
 * We have to change it if we change something that will be displayed
 */
std::string generate_app_id(const BaseApp &app) {
  auto hash = utils::hash(app.icon_png_path.value_or("") + app.title);
  // Value must be truncated to signed 32-bit range due to client limitations
  return std::to_string(abs((int32_t)hash));
}

std::shared_ptr<immer::atom<immer::vector<immer::box<events::App>>>>
parse_apps(const std::vector<BaseApp> &apps,
           const std::string &default_app_render_node,
           const std::string &default_gst_render_node,
           const BaseAppVideoOverride &default_video_settings,
           const std::string &h264_video_params,
           const std::string &hevc_video_params,
           const std::string &av1_video_params,
           const BaseAppAudioOverride &default_audio_settings,
           SessionsAtoms running_sessions,
           const std::shared_ptr<events::EventBusType> &ev_bus) {

  auto parsed_apps =
      apps |                                             //
      ranges::views::transform([&](const BaseApp &app) { //
        auto app_render_node = app.render_node.value_or(default_app_render_node);
        if (app_render_node != default_gst_render_node) {
          logs::log(logs::warning,
                    "App {} render node ({}) doesn't match the default GPU ({})",
                    app.title,
                    app_render_node,
                    default_gst_render_node);
          // TODO: allow user to override gst_render_node
        }
        auto app_video_settings = app.video.value_or(default_video_settings);
        auto app_audio_settings = app.audio.value_or(default_audio_settings);

        auto h264_gst_pipeline = fmt::format(
            "{} !\n{} !\n{} !\n{}", //
            app_video_settings.source.value_or(default_video_settings.source.value()),
            app_video_settings.video_params.value_or(h264_video_params),
            app_video_settings.h264_encoder.value_or(default_video_settings.h264_encoder.value()),
            app_video_settings.sink.value_or(default_video_settings.sink.value()));

        auto hevc_gst_pipeline =
            default_video_settings.hevc_encoder.has_value()
                ? fmt::format("{} !\n{} !\n{} !\n{}", //
                              app_video_settings.source.value_or(default_video_settings.source.value()),
                              app_video_settings.video_params.value_or(hevc_video_params),
                              app_video_settings.hevc_encoder.value_or(default_video_settings.hevc_encoder.value()),
                              app_video_settings.sink.value_or(default_video_settings.sink.value()))
                : "";

        auto av1_gst_pipeline =
            default_video_settings.av1_encoder.has_value()
                ? fmt::format("{} !\n{} !\n{} !\n{}", //
                              app_video_settings.source.value_or(default_video_settings.source.value()),
                              app_video_settings.video_params.value_or(av1_video_params),
                              app_video_settings.av1_encoder.value_or(default_video_settings.av1_encoder.value()),
                              app_video_settings.sink.value_or(default_video_settings.sink.value()))
                : "";

        auto opus_gst_pipeline = fmt::format(
            "{} !\n{} !\n{} !\n{}", //
            app_audio_settings.source.value_or(default_audio_settings.source.value()),
            app_audio_settings.audio_params.value_or(default_audio_settings.audio_params.value()),
            app_audio_settings.opus_encoder.value_or(default_audio_settings.opus_encoder.value()),
            app_audio_settings.sink.value_or(default_audio_settings.sink.value()));

        return immer::box<events::App>{
            events::App{.base = {.title = app.title,
                                 .id = generate_app_id(app),
                                 .support_hdr = false,
                                 .icon_png_path = app.icon_png_path},
                        .video_producer_buffer_caps = default_video_settings.producer_buffer_caps.value(),
                        .h264_gst_pipeline = h264_gst_pipeline,
                        .hevc_gst_pipeline = hevc_gst_pipeline,
                        .av1_gst_pipeline = av1_gst_pipeline,
                        .render_node = app_render_node,

                        .opus_gst_pipeline = opus_gst_pipeline,
                        .start_virtual_compositor = app.start_virtual_compositor.value_or(true),
                        .start_audio_server = app.start_audio_server.value_or(true),
                        .runner = get_runner(app.runner, ev_bus)}};
      }) |                                                  //
      ranges::to<immer::vector<immer::box<events::App>>>(); //

  return std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(parsed_apps);
}

} // namespace state
