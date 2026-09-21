#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <events/events.hpp>
#include <platforms/hw.hpp>
#include <state/config.hpp>

namespace state {

/*
 * Internal declarations shared between the config loader (`configTOML.cpp`) and the app parser
 * (`config_apps.cpp`). These were previously file-local to `configTOML.cpp`.
 */

/**
 * Pick the best encoder for a codec, delegating the vendor/availability policy to the platform
 * module. Returns nullopt when no candidate is both vendor-compatible and instantiable.
 */
std::optional<GstEncoder> get_encoder(std::string_view tech,
                                      std::string_view encoder_node,
                                      const std::vector<GstEncoder> &encoders,
                                      const GPU_VENDOR &vendor);

/**
 * ID is used by Moonlight to uniquely identify the app.
 * We have to change it if we change something that will be displayed.
 */
std::string generate_app_id(const BaseApp &app);

/**
 * Turn the TOML app definitions into `events::App` records with fully-built GStreamer pipelines.
 */
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
           const std::shared_ptr<events::EventBusType> &ev_bus);

} // namespace state
