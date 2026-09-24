#include <catch2/catch_test_macros.hpp>
#include <helpers/logger.hpp>
#include <platforms/hw.hpp>
#include <state/gpu.hpp>

using namespace state;

static Gpu make_gpu(const std::string &render_node, int weight) {
  return Gpu{.render_node = render_node, .weight = weight, .vendor = GPU_VENDOR::NVIDIA};
}

TEST_CASE("GPU load balancer", "[GPU]") {
  SECTION("Weighted assignment follows the expected sequence") {
    // A 3:1 pool: GPU-A should receive three sessions for every one that GPU-B gets.
    // Pick the GPU with max(weight - assigned), tie-break on fewest assigned then config order.
    GpuPool pool{.gpus = {make_gpu("/dev/dri/renderD128", 3), make_gpu("/dev/dri/renderD129", 1)}};

    GpuAssignments assignments{};
    std::vector<std::string> sequence;
    for (int i = 0; i < 8; i++) {
      auto gpu = pick_gpu(pool, assignments);
      REQUIRE(gpu != nullptr);
      sequence.push_back(gpu->render_node);
      assignments = assign_gpu(assignments, gpu->render_node);
    }

    REQUIRE(sequence == std::vector<std::string>{"/dev/dri/renderD128",
                                                 "/dev/dri/renderD128",
                                                 "/dev/dri/renderD129",
                                                 "/dev/dri/renderD128",
                                                 "/dev/dri/renderD129",
                                                 "/dev/dri/renderD128",
                                                 "/dev/dri/renderD129",
                                                 "/dev/dri/renderD128"});
  }

  SECTION("Equal weights round-robin") {
    GpuPool pool{.gpus = {make_gpu("/dev/dri/renderD128", 1), make_gpu("/dev/dri/renderD129", 1)}};

    GpuAssignments assignments{};
    std::vector<std::string> sequence;
    for (int i = 0; i < 4; i++) {
      auto gpu = pick_gpu(pool, assignments);
      REQUIRE(gpu != nullptr);
      sequence.push_back(gpu->render_node);
      assignments = assign_gpu(assignments, gpu->render_node);
    }

    REQUIRE(sequence == std::vector<std::string>{"/dev/dri/renderD128",
                                                 "/dev/dri/renderD129",
                                                 "/dev/dri/renderD128",
                                                 "/dev/dri/renderD129"});
  }

  SECTION("Releasing a GPU frees it back up") {
    GpuPool pool{.gpus = {make_gpu("/dev/dri/renderD128", 1), make_gpu("/dev/dri/renderD129", 1)}};

    GpuAssignments assignments{};
    assignments = assign_gpu(assignments, "/dev/dri/renderD128");
    assignments = assign_gpu(assignments, "/dev/dri/renderD129");

    REQUIRE(assignment_count(assignments, "/dev/dri/renderD128") == 1);
    REQUIRE(assignment_count(assignments, "/dev/dri/renderD129") == 1);

    assignments = release_gpu(assignments, "/dev/dri/renderD128");
    REQUIRE(assignment_count(assignments, "/dev/dri/renderD128") == 0);

    // The freed GPU (first in config order) should be picked again
    auto gpu = pick_gpu(pool, assignments);
    REQUIRE(gpu != nullptr);
    REQUIRE(gpu->render_node == "/dev/dri/renderD128");
  }

  SECTION("Releasing a GPU that was never assigned is a no-op") {
    GpuAssignments assignments{};
    assignments = release_gpu(assignments, "/dev/dri/renderD128");
    REQUIRE(assignment_count(assignments, "/dev/dri/renderD128") == 0);
  }

  SECTION("Empty pool returns no GPU") {
    GpuPool pool{};
    GpuAssignments assignments{};
    REQUIRE(pick_gpu(pool, assignments) == nullptr);
  }
}

TEST_CASE("GPU video settings resolution", "[GPU]") {
  auto gpu = make_gpu("/dev/dri/renderD128", 1);
  gpu.support_hevc = true;
  gpu.support_av1 = false;
  gpu.default_video = BaseAppVideoOverride{.source = "default_source",
                                           .sink = "default_sink",
                                           .producer_buffer_caps = "video/x-raw(memory:CUDAMemory)",
                                           .h264_encoder = "h264_enc",
                                           .hevc_encoder = "hevc_enc"};
  gpu.h264_video_params = "h264_params";
  gpu.hevc_video_params = "hevc_params";

  SECTION("App with no overrides uses GPU defaults") {
    auto resolved = resolve_video_settings(BaseAppVideoOverride{}, gpu);
    REQUIRE(resolved.video_producer_buffer_caps == "video/x-raw(memory:CUDAMemory)");
    REQUIRE(resolved.h264_gst_pipeline == "default_source !\nh264_params !\nh264_enc !\ndefault_sink");
    REQUIRE(resolved.hevc_gst_pipeline == "default_source !\nhevc_params !\nhevc_enc !\ndefault_sink");
    REQUIRE(resolved.av1_gst_pipeline.empty());
    REQUIRE(resolved.support_hevc);
    REQUIRE_FALSE(resolved.support_av1);
  }

  SECTION("App overrides win over GPU defaults") {
    auto app = BaseAppVideoOverride{.source = "app_source", .sink = "app_sink", .h264_encoder = "app_h264"};
    auto resolved = resolve_video_settings(app, gpu);
    REQUIRE(resolved.h264_gst_pipeline == "app_source !\nh264_params !\napp_h264 !\napp_sink");
    REQUIRE(resolved.hevc_gst_pipeline == "app_source !\nhevc_params !\nhevc_enc !\napp_sink");
  }
}