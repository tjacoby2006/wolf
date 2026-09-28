#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <fstream>
#include <gst/gstelementfactory.h>
#include <gst/gstregistry.h>
#include <platform/video/encoder_policy.hpp>
#include <platforms/hw.hpp>
#include <range/v3/view.hpp>
#include <rfl/toml.hpp>
#include <state/config.hpp>
#include <state/config_internal.hpp>
#include <state/gpu_balancer.hpp>

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
  if (version <= 6) {
    logs::log(logs::warning, "Found old config file (v{}), migrating to v7", version);
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
    logs::log(logs::debug, "Migrated config from v{} to v7", version);
  }

  // Will throw if the config is invalid
  auto cfg = rfl::toml::load<WolfConfig, rfl::DefaultIfMissing>(source).value();

  auto default_gst_video_settings = cfg.gstreamer.video;
  auto default_gst_audio_settings = cfg.gstreamer.audio;
  if (default_gst_video_settings.default_source.find("name=interpipesrc") == std::string::npos) {
    logs::log(logs::debug, "Found interpipesrc without name, adding it");
    default_gst_video_settings.default_source =
        default_gst_video_settings.default_source.replace(0, 12, "interpipesrc name=interpipesrc_{}_video");
  }
  if (default_gst_audio_settings.default_source.find("name=interpipesrc") == std::string::npos) {
    logs::log(logs::debug, "Found interpipesrc without name, adding it");
    default_gst_audio_settings.default_source =
        default_gst_audio_settings.default_source.replace(0, 12, "interpipesrc name=interpipesrc_{}_audio");
  }

  auto default_gst_encoder_settings = default_gst_video_settings.defaults;
  bool use_zero_copy = utils::get_env("WOLF_USE_ZERO_COPY", "") != std::string("FALSE");

  auto default_app_render_node = utils::get_env("WOLF_RENDER_NODE", "/dev/dri/renderD128");
  auto default_gst_render_node = utils::get_env("WOLF_ENCODER_NODE", default_app_render_node);
  auto vendor = get_vendor(default_gst_render_node);
  if (vendor == GPU_VENDOR::UNKNOWN) {
    logs::log(logs::warning, "Unable to detect GPU vendor, disabling zero copy pipeline.");
    use_zero_copy = false;
  }

  /* Automatically pick the best encoders */
  auto h264_encoder = get_encoder("h264", default_gst_render_node, default_gst_video_settings.h264_encoders, vendor);
  if (!h264_encoder) {
    throw std::runtime_error(
        "Unable to find a compatible H.264 encoder, please check [[gstreamer.video.h264_encoders]] "
        "in your config.toml or your Gstreamer installation");
  }
  auto hevc_encoder = get_encoder("h265", default_gst_render_node, default_gst_video_settings.hevc_encoders, vendor);
  auto av1_encoder = get_encoder("av1", default_gst_render_node, default_gst_video_settings.av1_encoders, vendor);

  /* Get paired clients */
  auto paired_clients =
      cfg.paired_clients                                                                                      //
      | ranges::views::transform([](const PairedClient &client) { return immer::box<PairedClient>{client}; }) //
      | ranges::to<immer::vector<immer::box<PairedClient>>>();

  auto default_base_video = BaseAppVideoOverride{.source = default_gst_video_settings.default_source,
                                                 .sink = default_gst_video_settings.default_sink,
                                                 .producer_buffer_caps = "video/x-raw"};
  auto default_base_audio = BaseAppAudioOverride{.source = default_gst_audio_settings.default_source,
                                                 .audio_params = default_gst_audio_settings.default_audio_params,
                                                 .opus_encoder = default_gst_audio_settings.default_opus_encoder,
                                                 .sink = default_gst_audio_settings.default_sink};

  auto video_encoder = wolf::platform::encoder_kind_from_plugin(h264_encoder->plugin_name);
  if (use_zero_copy) {
    switch (video_encoder) {
    case wolf::platform::EncoderKind::Nvidia: {
      default_base_video.producer_buffer_caps = "video/x-raw(memory:CUDAMemory)";
      break;
    }
    case wolf::platform::EncoderKind::Vaapi:
    case wolf::platform::EncoderKind::QuickSync: {
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
        default_base_video.producer_buffer_caps = wolf::platform::producer_buffer_caps_for(video_encoder, gst_caps);
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
            default_gst_render_node);

  default_base_video.h264_encoder = h264_encoder.value().encoder_pipeline;
  if (hevc_encoder) {
    default_base_video.hevc_encoder = hevc_encoder.value().encoder_pipeline;
  } else {
    logs::log(logs::warning, "Unable to find an HEVC encoder, disabling it");
  }

  if (av1_encoder) {
    default_base_video.av1_encoder = av1_encoder.value().encoder_pipeline;
  } else {
    logs::log(logs::warning, "Unable to find an AV1 encoder, disabling it");
  }

  auto empty_enc = GstEncoderDefault{};
  auto default_h264 = utils::get_optional(default_gst_encoder_settings, h264_encoder.value_or(GstEncoder{}).plugin_name)
                          .value_or(empty_enc);
  auto default_hevc = utils::get_optional(default_gst_encoder_settings, hevc_encoder.value_or(GstEncoder{}).plugin_name)
                          .value_or(empty_enc);
  auto default_av1 = utils::get_optional(default_gst_encoder_settings, av1_encoder.value_or(GstEncoder{}).plugin_name)
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

  auto clients_atom = std::make_shared<immer::atom<PairedClientList>>(paired_clients);

  /* Build the GPU pool for load balancing: discovered render nodes + user [gpus] weights/exclusions */
  auto discovered = discover_render_nodes();
  std::string discovered_str;
  for (const auto &n : discovered) {
    if (!discovered_str.empty())
      discovered_str += ", ";
    discovered_str += n;
  }
  logs::log(logs::info, "[GPU] Discovered {} render node(s): {}", discovered.size(), discovered_str);
  auto gpu_pool = GpuBalancer::from_pool(discovered, cfg.gpus, cfg.excluded_gpus, default_app_render_node);
  for (const auto &[node, info] : gpu_pool.pool) {
    logs::log(logs::info, "[GPU] Pool node: {} (weight={}, excluded={})", node, info.weight, info.excluded);
  }

  /* Get profiles, for each app defined will merge with default settings */
  auto profiles = cfg.profiles | //
                  ranges::views::transform([&](const Profile &profile) {
                    return events::Profile{.id = profile.id,
                                           .name = profile.name.value_or(""),
                                           .icon_png_path = profile.icon_png_path.value_or(""),
                                           .pin = profile.pin,
                                           .apps = parse_apps(profile.apps,
                                                              default_app_render_node,
                                                              default_gst_render_node,
                                                              default_base_video,
                                                              h264_video_params,
                                                              hevc_video_params,
                                                              av1_video_params,
                                                              default_base_audio,
                                                              running_sessions,
                                                              ev_bus)};
                  }) |
                  ranges::to<ProfilesList>();
  auto profiles_atom = std::make_shared<immer::atom<ProfilesList>>(profiles);

  std::map<std::string, std::map<std::string, std::shared_ptr<events::App>>> gpu_apps;
  for (const auto &[node, info] : gpu_pool.pool) {
    if (info.excluded) {
      continue;
    }
    if (node == default_gst_render_node || get_vendor(node) == GPU_VENDOR::UNKNOWN) {
      continue;
    }
    auto node_vendor = get_vendor(node);
    auto node_h264 = get_encoder("h264", node, default_gst_video_settings.h264_encoders, node_vendor);
    if (!node_h264) {
      logs::log(logs::warning, "[GPU] No H.264 encoder available for {}; skipping node", node);
      continue;
    }
    auto node_hevc = get_encoder("h265", node, default_gst_video_settings.hevc_encoders, node_vendor);
    auto node_av1 = get_encoder("av1", node, default_gst_video_settings.av1_encoders, node_vendor);
    auto node_video = default_base_video;
    // Each session has its own producer on its assigned node. Keep zero-copy enabled on
    // mixed-vendor hosts: VA consumers receive DMABuf, NVIDIA consumers receive CUDAMemory.
    node_video.producer_buffer_caps = "video/x-raw";
    if (use_zero_copy) {
      auto kind = wolf::platform::encoder_kind_from_plugin(node_h264->plugin_name);
      if (kind == wolf::platform::EncoderKind::Nvidia) {
        node_video.producer_buffer_caps = wolf::platform::producer_buffer_caps_for(kind, {});
      } else if (kind == wolf::platform::EncoderKind::Vaapi || kind == wolf::platform::EncoderKind::QuickSync) {
        auto dma_caps = gstreamer::get_dma_caps("vapostproc") |
                        ranges::views::remove_if([](const std::string &cap) {
                          return cap.find("P010") != std::string::npos || cap.find("AR30") != std::string::npos ||
                                 cap.find(' ') != std::string::npos;
                        }) |
                        ranges::to<std::vector>();
        node_video.producer_buffer_caps = wolf::platform::producer_buffer_caps_for(kind, dma_caps);
      }
    }
    node_video.h264_encoder = node_h264->encoder_pipeline;
    node_video.hevc_encoder = node_hevc ? std::optional<std::string>(node_hevc->encoder_pipeline) : std::nullopt;
    node_video.av1_encoder = node_av1 ? std::optional<std::string>(node_av1->encoder_pipeline) : std::nullopt;

    auto node_params = [&](const GstEncoder &encoder) {
      auto defaults = utils::get_optional(default_gst_encoder_settings, encoder.plugin_name).value_or(GstEncoderDefault{});
      const bool node_zero_copy = use_zero_copy && node_video.producer_buffer_caps != "video/x-raw";
      return node_zero_copy ? encoder.video_params_zero_copy.value_or(defaults.video_params_zero_copy)
                            : encoder.video_params.value_or(defaults.video_params);
    };
    for (const auto &profile : cfg.profiles) {
      auto apps = parse_apps(profile.apps,
                             default_app_render_node,
                             node,
                             node_video,
                             node_params(*node_h264),
                             node_hevc ? node_params(*node_hevc) : "",
                             node_av1 ? node_params(*node_av1) : "",
                             default_base_audio,
                             running_sessions,
                             ev_bus)->load();
      for (const auto &app : *apps) {
        gpu_apps[node].emplace(app->base.id, std::make_shared<events::App>(*app));
      }
    }
  }

  return Config{.uuid = cfg.uuid,
                .hostname = cfg.hostname,
                .config_source = source,
                .support_hevc = hevc_encoder.has_value(),
                .support_av1 = av1_encoder.has_value() &&
                               wolf::platform::encoder_kind_from_plugin(av1_encoder->plugin_name) !=
                                   wolf::platform::EncoderKind::Software,
                .paired_clients = clients_atom,
                .profiles = profiles_atom,
                .gpu_apps = std::move(gpu_apps),
                .gpu_pool = gpu_pool};
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
                                                .render_node = app->render_node,
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
