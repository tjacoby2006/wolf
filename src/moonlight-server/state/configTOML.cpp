#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <fstream>
#include <gst/gstelementfactory.h>
#include <gst/gstregistry.h>
#include <platforms/hw.hpp>
#include <range/v3/view.hpp>
#include <rfl/toml.hpp>
#include <state/config.hpp>
#include <state/gpu.hpp>

namespace state {

/**
 * A bit of magic here, it'll load up the default/config.toml via Cmake (look for `make_includable`)
 */
constexpr char const *default_toml =
#include "default/config.include.toml"

    ;

using namespace std::literals;
using namespace wolf::config;

void create_default(const std::string &source) {
  std::ofstream out_file;
  out_file.open(source);
  out_file << "# A unique identifier for this host" << std::endl;
  out_file << "uuid = \"" << gen_uuid() << "\"" << std::endl;
  out_file << default_toml;
  out_file.close();
}

static Encoder encoder_type(const GstEncoder &settings) {
  switch (utils::hash(settings.plugin_name)) {
  case (utils::hash("nvcodec")):
    return NVIDIA;
  case (utils::hash("vaapi")):
  case (utils::hash("va")):
    return VAAPI;
  case (utils::hash("qsv")):
    return QUICKSYNC;
  case (utils::hash("applemedia")):
    return APPLE;
  case (utils::hash("x264")):
  case (utils::hash("x265")):
  case (utils::hash("aom")):
    return SOFTWARE;
  }
  logs::log(logs::warning, "Unrecognised Gstreamer plugin name: {}", settings.plugin_name);
  return UNKNOWN;
}

static bool is_available(const GPU_VENDOR &gpu_vendor, const GstEncoder &settings) {
  if (auto plugin = gst_registry_find_plugin(gst_registry_get(), settings.plugin_name.c_str())) {
    gst_object_unref(plugin);
    return std::all_of(
        settings.check_elements.begin(),
        settings.check_elements.end(),
        [settings, gpu_vendor](const auto &el_name) {
          // Is the selected GPU vendor compatible with the encoder?
          // (Particularly useful when using multiple GPUs, e.g. nvcodec might be available but user
          // wants to encode using the Intel GPU)
          auto encoder_vendor = encoder_type(settings);
          if (encoder_vendor == NVIDIA && gpu_vendor != GPU_VENDOR::NVIDIA) {
            logs::log(logs::debug, "Skipping NVIDIA encoder, not a NVIDIA GPU ({})", (int)gpu_vendor);
          } else if (encoder_vendor == VAAPI && (gpu_vendor != GPU_VENDOR::INTEL && gpu_vendor != GPU_VENDOR::AMD)) {
            logs::log(logs::debug, "Skipping VAAPI encoder, not an Intel or AMD GPU ({})", (int)gpu_vendor);
          } else if (encoder_vendor == QUICKSYNC && gpu_vendor != GPU_VENDOR::INTEL) {
            logs::log(logs::debug, "Skipping QUICKSYNC encoder, not an Intel GPU ({})", (int)gpu_vendor);
          }
          // Can Gstreamer instantiate the element? This will only work if all the drivers are in place
          else if (auto el = gst_element_factory_make(el_name.c_str(), nullptr)) {
            gst_object_unref(el);
            return true;
          }

          return false;
        });
  }
  return false;
}

std::optional<GstEncoder> get_encoder(std::string_view tech,
                                      std::string_view encoder_node,
                                      const std::vector<GstEncoder> &encoders,
                                      const GPU_VENDOR &vendor) {
  auto default_is_available = std::bind(is_available, vendor, std::placeholders::_1);
  auto encoder = std::find_if(encoders.begin(), encoders.end(), default_is_available);
  if (encoder != std::end(encoders)) {
    auto encoder_node_name = get_render_node_name(encoder_node);
    if (encoder_type(*encoder) == VAAPI && encoder_node_name != "renderD128") {
      auto possible_vaapi_plugin = fmt::format("va{}{}enc", encoder_node_name, tech);
      auto possible_encoder = GstEncoder{.plugin_name = encoder->plugin_name,
                                         .check_elements = {possible_vaapi_plugin, "vapostproc"},
                                         .video_params = encoder->video_params,
                                         .video_params_zero_copy = encoder->video_params_zero_copy,
                                         .encoder_pipeline = encoder->encoder_pipeline};
      logs::log(logs::debug, "Checking if {} is available", possible_vaapi_plugin);
      if (is_available(vendor, possible_encoder)) {
        possible_encoder.encoder_pipeline = std::regex_replace(possible_encoder.encoder_pipeline,
                                                               std::regex(fmt::format("va{}enc", tech)),
                                                               possible_vaapi_plugin);
        logs::log(logs::info, "Detected multiple VAAPI capable devices, using {} encoder", possible_vaapi_plugin);
        return possible_encoder;
      }
    }
    if (encoder_type(*encoder) == NVIDIA && encoder_node_name != "renderD128") {
      // TODO: do the same trick for Nvidia and nvh265device{dev-number}enc we have to get that dev-number though..
    }
    logs::log(logs::info, "Using {} encoder: {}", tech, encoder->plugin_name);
    if (encoder_type(*encoder) == SOFTWARE) {
      logs::log(logs::warning, "Software {} encoder detected", tech);
    }
    return *encoder;
  }
  return std::nullopt;
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

/**
 * Resolves a single GPU: vendor detection, zero-copy buffer caps and the best available encoders.
 * Returns std::nullopt if no H.264 encoder can be found for the device.
 */
std::optional<Gpu> build_gpu(const std::string &render_node,
                             int weight,
                             const GstVideoCfg &video_settings,
                             const BaseAppVideoOverride &base_video) {
  auto vendor = get_vendor(render_node);
  bool use_zero_copy = utils::get_env("WOLF_USE_ZERO_COPY", "") != std::string("FALSE");
  if (vendor == GPU_VENDOR::UNKNOWN) {
    logs::log(logs::warning, "Unable to detect GPU vendor for {}, disabling zero copy pipeline.", render_node);
    use_zero_copy = false;
  }

  auto h264_encoder = get_encoder("h264", render_node, video_settings.h264_encoders, vendor);
  if (!h264_encoder) {
    logs::log(logs::warning,
              "Unable to find a compatible H.264 encoder for {} ({}), skipping this GPU",
              render_node,
              get_vendor_name(vendor));
    return std::nullopt;
  }
  auto hevc_encoder = get_encoder("h265", render_node, video_settings.hevc_encoders, vendor);
  auto av1_encoder = get_encoder("av1", render_node, video_settings.av1_encoders, vendor);

  auto gpu_video = base_video;

  auto video_encoder = encoder_type(*h264_encoder);
  if (use_zero_copy) {
    switch (video_encoder) {
    case NVIDIA: {
      gpu_video.producer_buffer_caps = "video/x-raw(memory:CUDAMemory)";
      break;
    }
    case VAAPI:
    case QUICKSYNC: {
      auto required_caps = gstreamer::get_dma_caps("vapostproc");
      logs::log(logs::debug, "Required DMA formats for vapostproc: {}", required_caps);
      auto gst_caps = required_caps | //
                      ranges::views::remove_if([](const std::string &cap) {
                        // TODO: HDR isn't supported by Wolf yet (so we remove P010 and AR30 format)
                        return cap.find("P010") != std::string::npos || cap.find("AR30") != std::string::npos ||
                               // We also remove formats that are padded with spaces since they need escaping
                               cap.find(" ") != std::string::npos;
                      }) | //
                      ranges::to<std::vector>();
      if (gst_caps.empty()) {
        logs::log(logs::warning,
                  "Unable to find any compatible DMA formats for vapostproc, disabling zero copy pipeline.");
        use_zero_copy = false;
      } else {
        gpu_video.producer_buffer_caps =
            fmt::format("video/x-raw(memory:DMABuf), drm-format={{{}}}", utils::join(gst_caps, ","));
      }
      break;
    }
    default: {
    }
    }
  }

  logs::log(logs::info,
            "Using {} pipeline on {} ({})",
            use_zero_copy ? "zero copy" : "legacy",
            get_vendor_name(vendor),
            render_node);

  gpu_video.h264_encoder = h264_encoder->encoder_pipeline;
  if (hevc_encoder) {
    gpu_video.hevc_encoder = hevc_encoder->encoder_pipeline;
  } else {
    logs::log(logs::warning, "Unable to find an HEVC encoder for {}, disabling it", render_node);
  }

  if (av1_encoder) {
    gpu_video.av1_encoder = av1_encoder->encoder_pipeline;
  } else {
    logs::log(logs::warning, "Unable to find an AV1 encoder for {}, disabling it", render_node);
  }

  auto empty_enc = GstEncoderDefault{};
  auto default_h264 = utils::get_optional(video_settings.defaults, h264_encoder.value_or(GstEncoder{}).plugin_name)
                          .value_or(empty_enc);
  auto default_hevc = utils::get_optional(video_settings.defaults, hevc_encoder.value_or(GstEncoder{}).plugin_name)
                          .value_or(empty_enc);
  auto default_av1 = utils::get_optional(video_settings.defaults, av1_encoder.value_or(GstEncoder{}).plugin_name)
                         .value_or(empty_enc);

  auto h264_video_params = use_zero_copy
                               ? h264_encoder->video_params_zero_copy.value_or(default_h264.video_params_zero_copy)
                               : h264_encoder->video_params.value_or(default_h264.video_params);

  std::string hevc_video_params;
  if (hevc_encoder) {
    hevc_video_params = use_zero_copy
                            ? hevc_encoder->video_params_zero_copy.value_or(default_hevc.video_params_zero_copy)
                            : hevc_encoder->video_params.value_or(default_hevc.video_params);
  }

  std::string av1_video_params;
  if (av1_encoder) {
    av1_video_params = use_zero_copy ? av1_encoder->video_params_zero_copy.value_or(default_av1.video_params_zero_copy)
                                     : av1_encoder->video_params.value_or(default_av1.video_params);
  }

  return Gpu{.render_node = render_node,
             .weight = std::max(1, weight),
             .vendor = vendor,
             .use_zero_copy = use_zero_copy,
             .support_hevc = hevc_encoder.has_value(),
             .support_av1 = av1_encoder.has_value() && encoder_type(*av1_encoder) != SOFTWARE,
             .default_video = gpu_video,
             .h264_video_params = h264_video_params,
             .hevc_video_params = hevc_video_params,
             .av1_video_params = av1_video_params};
}

/**
 * Builds the pool of GPUs that Wolf will balance sessions across.
 *
 * If the config has an explicit `[[gpus]]` list we honour it (each entry carrying its own weight);
 * otherwise we auto-detect every render node on the system (minus `excluded_gpus`) with weight 1.
 * When no render node can be detected at all we fall back to `WOLF_RENDER_NODE` so that single-GPU
 * deployments (and non-Linux platforms) keep working.
 */
GpuPool build_gpu_pool(const WolfConfig &cfg) {
  auto video_settings = cfg.gstreamer.video;
  if (video_settings.default_source.find("name=interpipesrc") == std::string::npos) {
    logs::log(logs::debug, "Found interpipesrc without name, adding it");
    video_settings.default_source =
        video_settings.default_source.replace(0, 12, "interpipesrc name=interpipesrc_{}_video");
  }

  auto base_video = BaseAppVideoOverride{.source = video_settings.default_source,
                                         .sink = video_settings.default_sink,
                                         .producer_buffer_caps = "video/x-raw"};

  // Work out which render nodes (and weights) we should use
  std::vector<GpuConfig> gpu_configs;
  if (!cfg.gpus.empty()) {
    logs::log(logs::info, "Using {} GPU(s) from config", cfg.gpus.size());
    gpu_configs = cfg.gpus;
  } else {
    auto render_nodes = list_render_nodes();
    if (render_nodes.empty()) {
      // Fallback to the (possibly overridden) single render node from the environment
      auto default_render_node = utils::get_env("WOLF_RENDER_NODE", "/dev/dri/renderD128");
      logs::log(logs::info, "No render nodes detected, falling back to {}", default_render_node);
      render_nodes.push_back(default_render_node);
    } else {
      logs::log(logs::info, "Auto-detected {} GPU(s)", render_nodes.size());
    }

    for (const auto &render_node : render_nodes) {
      if (std::find(cfg.excluded_gpus.begin(), cfg.excluded_gpus.end(), render_node) != cfg.excluded_gpus.end()) {
        logs::log(logs::info, "Skipping excluded GPU: {}", render_node);
        continue;
      }
      gpu_configs.push_back(GpuConfig{.render_node = render_node, .weight = 1});
    }
  }

  GpuPool pool;
  for (const auto &gpu_config : gpu_configs) {
    if (auto gpu = build_gpu(gpu_config.render_node, gpu_config.weight, video_settings, base_video)) {
      logs::log(logs::info,
                "Added GPU {} ({}), weight: {}",
                gpu->render_node,
                get_vendor_name(gpu->vendor),
                gpu->weight);
      pool.gpus.push_back(std::move(*gpu));
    }
  }

  if (pool.gpus.empty()) {
    throw std::runtime_error(
        "Unable to find a usable GPU (no compatible H.264 encoder found). "
        "Please check [[gstreamer.video.h264_encoders]] in your config.toml or your Gstreamer installation");
  }

  return pool;
}

std::shared_ptr<immer::atom<immer::vector<immer::box<events::App>>>>
parse_apps(const std::vector<BaseApp> &apps,
           const Gpu &default_gpu,
           const BaseAppAudioOverride &default_audio_settings,
           SessionsAtoms running_sessions,
           const std::shared_ptr<events::EventBusType> &ev_bus) {

  auto parsed_apps =
      apps |                                             //
      ranges::views::transform([&](const BaseApp &app) { //
        auto app_video_settings = app.video.value_or(BaseAppVideoOverride{});
        auto app_audio_settings = app.audio.value_or(default_audio_settings);

        // Pipelines are resolved against the default GPU here; they'll be re-resolved per-session
        // once the load balancer assigns an actual GPU.
        auto resolved = resolve_video_settings(app_video_settings, default_gpu);

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
                        .video_override = app_video_settings,
                        .video_producer_buffer_caps = resolved.video_producer_buffer_caps,
                        .h264_gst_pipeline = resolved.h264_gst_pipeline,
                        .hevc_gst_pipeline = resolved.hevc_gst_pipeline,
                        .av1_gst_pipeline = resolved.av1_gst_pipeline,
                        .render_node = default_gpu.render_node,
                        .support_hevc = resolved.support_hevc,
                        .support_av1 = resolved.support_av1,

                        .opus_gst_pipeline = opus_gst_pipeline,
                        .start_virtual_compositor = app.start_virtual_compositor.value_or(true),
                        .start_audio_server = app.start_audio_server.value_or(true),
                        .runner = get_runner(app.runner, ev_bus)}};
      }) |                                                  //
      ranges::to<immer::vector<immer::box<events::App>>>(); //

  return std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(parsed_apps);
}

/**
 * Per-app render_node pins are no longer supported (the GPU load balancer decides placement now).
 * The field is dropped when deserializing, so scan the raw TOML and warn about any pins we find.
 */
static void warn_ignored_render_nodes(const std::string &source) {
  try {
    auto raw = toml::parse_file(source);

    auto *profiles_node = raw.get("profiles");
    auto *profiles = profiles_node != nullptr ? profiles_node->as_array() : nullptr;
    if (profiles == nullptr) {
      return;
    }

    for (const auto &profile_node : *profiles) {
      auto *profile = profile_node.as_table();
      if (profile == nullptr) {
        continue;
      }
      auto *apps_node = profile->get("apps");
      auto *apps = apps_node != nullptr ? apps_node->as_array() : nullptr;
      if (apps == nullptr) {
        continue;
      }

      for (const auto &app_node : *apps) {
        auto *app = app_node.as_table();
        if (app == nullptr) {
          continue;
        }
        auto *render_node_node = app->get("render_node");
        if (render_node_node == nullptr) {
          continue;
        }
        auto render_node = render_node_node->value<std::string>();
        if (!render_node.has_value()) {
          continue;
        }
        auto *title_node = app->get("title");
        auto title = title_node != nullptr ? title_node->value<std::string>().value_or("<unnamed>")
                                           : std::string("<unnamed>");
        logs::log(logs::warning,
                  "App '{}' has a per-app render_node pin ({}) which is no longer supported and will be "
                  "ignored; use the top-level [[gpus]] list and weights to control GPU placement instead",
                  title,
                  *render_node);
      }
    }
  } catch (const std::exception &e) {
    logs::log(logs::debug, "Unable to scan {} for dropped render_node pins: {}", source, e.what());
  }
}

Config load_or_default(const std::string &source,
                       const std::shared_ptr<events::EventBusType> &ev_bus,
                       SessionsAtoms running_sessions) {
  if (!file_exist(source)) {
    logs::log(logs::warning, "Unable to open config file: {}, creating one using defaults", source);
    create_default(source);
  }

  // First check the version of the config file
  auto base_cfg = rfl::toml::load<BaseConfig, rfl::DefaultIfMissing>(source).value();
  auto version = base_cfg.config_version.value_or(0);
  if (version <= 7) {
    logs::log(logs::warning, "Found old config file (v{}), migrating to v8", version);
    auto backup = source + ".v" + std::to_string(version) + ".old";
    std::filesystem::rename(source, backup);
    auto old_cfg = toml::parse_file(backup);
    create_default(source);
    auto new_cfg = toml::parse_file(source);
    new_cfg.insert_or_assign("hostname", old_cfg.at("hostname"));
    new_cfg.insert_or_assign("uuid", old_cfg.at("uuid"));
    new_cfg.insert_or_assign("paired_clients", old_cfg.at("paired_clients"));
    if (old_cfg.contains("profiles")) {
      new_cfg.insert_or_assign("profiles", old_cfg.at("profiles"));
    } else if (old_cfg.contains("apps")) {
      auto moonlight_profile = new_cfg["profiles"].as_array()->at(0).as_table();
      new_cfg.insert_or_assign(
          "profiles",
          toml::array{*moonlight_profile,
                      toml::table({{"id", "user"}, {"name", "User"}, {"apps", old_cfg.at("apps")}})});
    }
    std::ofstream out_file;
    out_file.open(source);
    if (!out_file.is_open()) {
      throw std::runtime_error("Failed to open config file for writing");
    }
    out_file << new_cfg;
    out_file.close();
    logs::log(logs::debug, "Migrated config from v{} to v8", version);
  }

  // Will throw if the config is invalid
  auto cfg = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(source).value();

  // Per-app render_node pins are no longer supported; warn about any that are present in the raw TOML
  // since the unknown field is silently dropped by the deserializer.
  warn_ignored_render_nodes(source);

  auto default_gst_audio_settings = cfg.gstreamer.audio;
  if (default_gst_audio_settings.default_source.find("name=interpipesrc") == std::string::npos) {
    logs::log(logs::debug, "Found interpipesrc without name, adding it");
    default_gst_audio_settings.default_source =
        default_gst_audio_settings.default_source.replace(0, 12, "interpipesrc name=interpipesrc_{}_audio");
  }

  auto default_base_audio = BaseAppAudioOverride{.source = default_gst_audio_settings.default_source,
                                                 .audio_params = default_gst_audio_settings.default_audio_params,
                                                 .opus_encoder = default_gst_audio_settings.default_opus_encoder,
                                                 .sink = default_gst_audio_settings.default_sink};

  /* Build the GPU pool; per-GPU vendor detection + encoder selection happens here */
  auto gpu_pool = build_gpu_pool(cfg);

  /* Get paired clients */
  auto paired_clients =
      cfg.paired_clients                                                                                      //
      | ranges::views::transform([](const PairedClient &client) { return immer::box<PairedClient>{client}; }) //
      | ranges::to<immer::vector<immer::box<PairedClient>>>();

  auto clients_atom = std::make_shared<immer::atom<PairedClientList>>(paired_clients);

  // Apps are parsed against the first GPU in the pool; pipelines are re-resolved per-session once a
  // GPU has been assigned by the load balancer.
  const auto &default_gpu = gpu_pool.gpus.front();

  /* Get profiles, for each app defined will merge with default settings */
  auto profiles = cfg.profiles | //
                  ranges::views::transform([&](const Profile &profile) {
                    return events::Profile{.id = profile.id,
                                           .name = profile.name.value_or(""),
                                           .icon_png_path = profile.icon_png_path.value_or(""),
                                           .pin = profile.pin,
                                           .apps = parse_apps(profile.apps,
                                                              default_gpu,
                                                              default_base_audio,
                                                              running_sessions,
                                                              ev_bus)};
                  }) |
                  ranges::to<ProfilesList>();
  auto profiles_atom = std::make_shared<immer::atom<ProfilesList>>(profiles);

  // HEVC/AV1 are supported as long as at least one GPU in the pool can encode them
  auto support_hevc = std::any_of(gpu_pool.gpus.begin(), gpu_pool.gpus.end(), [](const Gpu &gpu) {
    return gpu.support_hevc;
  });
  auto support_av1 = std::any_of(gpu_pool.gpus.begin(), gpu_pool.gpus.end(), [](const Gpu &gpu) {
    return gpu.support_av1;
  });

  return Config{.uuid = cfg.uuid,
                .hostname = cfg.hostname,
                .config_source = source,
                .support_hevc = support_hevc,
                .support_av1 = support_av1,
                .gpus = gpu_pool,
                .paired_clients = clients_atom,
                .profiles = profiles_atom};
}

void pair(const Config &cfg, const PairedClient &client) {
  // Update CFG
  cfg.paired_clients->update([&client](const PairedClientList &paired_clients) {
    // Removing the client if already present (see: https://github.com/games-on-whales/wolf/issues/211)
    auto filtered_clients = paired_clients                                               //
                            | ranges::views::filter([&client](auto paired_client) {      //
                                return paired_client->client_cert != client.client_cert; //
                              })                                                         //
                            | ranges::to<PairedClientList>();                            //
    return filtered_clients.push_back(client);
  });

  // Update TOML
  auto tml = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();
  tml.paired_clients.push_back(client);
  rfl::toml::save(cfg.config_source, tml);
}

void unpair(const Config &cfg, const PairedClient &client) {
  // Update CFG
  cfg.paired_clients->update([&client](const PairedClientList &paired_clients) {
    return paired_clients                                               //
           | ranges::views::filter([&client](auto paired_client) {      //
               return paired_client->client_cert != client.client_cert; //
             })                                                         //
           | ranges::to<PairedClientList>();                            //
  });

  // Update TOML
  auto tml = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();
  tml.paired_clients.erase(std::remove_if(tml.paired_clients.begin(),
                                          tml.paired_clients.end(),
                                          [&client](const auto &v) { return v.client_cert == client.client_cert; }),
                           tml.paired_clients.end());
  rfl::toml::save(cfg.config_source, tml);
}

void update_client_settings(const Config &cfg, std::size_t client_id, const PairedClient &updated_client) {

  auto update_client_fn = [&](immer::box<PairedClient> client) -> immer::box<PairedClient> {
    if (get_client_id(client) == client_id) {
      return immer::box<PairedClient>(updated_client);
    }
    return client;
  };

  cfg.paired_clients->update([&](const PairedClientList &paired_clients) {
    return paired_clients |                             //
           ranges::views::transform(update_client_fn) | //
           ranges::to<PairedClientList>();
  });

  // Update the TOML file
  auto tml = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();

  tml.paired_clients = tml.paired_clients |                         //
                       ranges::views::transform(update_client_fn) | //
                       ranges::to<std::vector<PairedClient>>();

  // Save back to file
  rfl::toml::save(cfg.config_source, tml);
}

void update_profiles(const Config &cfg, const ProfilesList &profiles) {
  cfg.profiles->store(profiles);

  auto tml = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();
  tml.profiles = profiles | //
                 ranges::views::transform([](const immer::box<events::Profile> &p) {
                   return Profile{
                       .id = p->id,
                       .name = p->name,
                       .icon_png_path = p->icon_png_path,
                       .pin = p->pin,
                       .apps = p->apps->load().get() | //
                               ranges::views::transform([](const immer::box<events::App> &app) {
                                 return BaseApp{.title = app->base.title,
                                                .icon_png_path = app->base.icon_png_path,
                                                .start_virtual_compositor = app->start_virtual_compositor,
                                                .start_audio_server = app->start_audio_server,
                                                .runner = app->runner->serialize()};
                               }) | //
                               ranges::to_vector,
                   };
                 }) | //
                 ranges::to_vector;
  rfl::toml::save(cfg.config_source, tml);
}

} // namespace state