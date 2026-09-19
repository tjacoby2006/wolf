#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <platforms/hw.hpp>
#include <state/config.hpp>
#include <state/gpu_balancer.hpp>

using namespace state;

namespace {

GpuBalancer make_balancer() {
  // renderD128 (weight 4, e.g. RTX 3090) and renderD136 (weight 1, e.g. 1660)
  GpuBalancer balancer;
  balancer.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 4, .excluded = false};
  balancer.pool["/dev/dri/renderD136"] = GpuInfo{.weight = 1, .excluded = false};
  return balancer;
}

} // namespace

TEST_CASE("GpuBalancer picks the least-loaded GPU by usage/weight", "[gpu_balancer]") {
  auto balancer = make_balancer();

  SECTION("empty pool picks the first node (deterministic)") {
    REQUIRE(balancer.pick(std::nullopt) == "/dev/dri/renderD128");
  }

  SECTION("higher-weight GPU takes more apps before spilling over") {
    // After 3 acquisitions on renderD128, its score is 3/4 = 0.75 < 0/1 = 0? No:
    // start: 128=0 (score 0), 136=0 (score 0) -> tie broken by usage then path -> 128
    auto b = balancer.acquire("/dev/dri/renderD128");
    // 128=1 (score 0.25), 136=0 (score 0) -> 136 wins
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD136");

    b = b.acquire("/dev/dri/renderD136");
    // 128=1 (0.25), 136=1 (1.0) -> 128 wins again
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD128");

    b = b.acquire("/dev/dri/renderD128");
    // 128=2 (0.5), 136=1 (1.0) -> 128 wins
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD128");

    b = b.acquire("/dev/dri/renderD128");
    // 128=3 (0.75), 136=1 (1.0) -> 128 still wins (weight lets it take 4 before spilling)
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD128");

    b = b.acquire("/dev/dri/renderD128");
    // 128=4 (1.0), 136=1 (1.0) -> tie broken by lowest usage -> 136
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD136");
  }

  SECTION("a freed GPU is reused before assigning to another") {
    auto b = balancer.acquire("/dev/dri/renderD128").acquire("/dev/dri/renderD136");
    // Both at usage 1; releasing one makes it the clear favourite
    b = b.release("/dev/dri/renderD128");
    REQUIRE(b.pick(std::nullopt) == "/dev/dri/renderD128");
  }

  SECTION("release floors at zero and is idempotent below zero") {
    auto b = balancer.release("/dev/dri/renderD128").release("/dev/dri/renderD128");
    REQUIRE(b.usage.at("/dev/dri/renderD128") == 0);
  }
}

TEST_CASE("GpuBalancer honours pinned GPUs", "[gpu_balancer]") {
  auto balancer = make_balancer();

  SECTION("pin to an available node returns that node regardless of load") {
    auto b = balancer.acquire("/dev/dri/renderD128").acquire("/dev/dri/renderD136");
    REQUIRE(b.pick(std::optional<std::string>("/dev/dri/renderD136")) == "/dev/dri/renderD136");
  }

  SECTION("pin to an unknown node returns nullopt") {
    REQUIRE(balancer.pick(std::optional<std::string>("/dev/dri/renderD999")).has_value() == false);
  }
}

TEST_CASE("GpuBalancer excludes GPUs from the pool", "[gpu_balancer]") {
  auto balancer = make_balancer();
  balancer.pool["/dev/dri/renderD128"].excluded = true;

  SECTION("excluded node is never picked") {
    REQUIRE(balancer.pick(std::nullopt) == "/dev/dri/renderD136");
  }

  SECTION("pin to an excluded node returns nullopt") {
    REQUIRE(balancer.pick(std::optional<std::string>("/dev/dri/renderD128")).has_value() == false);
  }

  SECTION("is_available reflects exclusion") {
    REQUIRE(balancer.is_available("/dev/dri/renderD128") == false);
    REQUIRE(balancer.is_available("/dev/dri/renderD136") == true);
  }
}

TEST_CASE("GpuBalancer::from_pool builds the pool from config", "[gpu_balancer]") {
  std::vector<std::string> discovered = {"/dev/dri/renderD128", "/dev/dri/renderD136"};
  std::map<std::string, int> weights = {{"renderD128-path", 4}};
  std::vector<std::string> excluded = {"/dev/dri/renderD136"};

  SECTION("applies weights and exclusions, always keeps the default node") {
    auto balancer = GpuBalancer::from_pool(discovered, weights, excluded, "/dev/dri/renderD128");
    REQUIRE(balancer.pool.count("/dev/dri/renderD128") == 1);
    REQUIRE(balancer.pool.count("/dev/dri/renderD136") == 1);
    REQUIRE(balancer.pool.at("/dev/dri/renderD136").excluded == true);
    // default node kept even though it would otherwise be absent
    auto with_default = GpuBalancer::from_pool({}, {}, {}, "/dev/dri/renderD128");
    REQUIRE(with_default.pool.count("/dev/dri/renderD128") == 1);
  }

  SECTION("weight below 1 is clamped to 1") {
    std::map<std::string, int> low_weights = {{"renderD128-path", 0}};
    auto balancer = GpuBalancer::from_pool(discovered, low_weights, {}, "/dev/dri/renderD128");
    REQUIRE(balancer.pool.at("/dev/dri/renderD128").weight == 1);
  }
}

TEST_CASE("GpuBalancer sticky sessions keep a client on its assigned GPU", "[gpu_balancer][sticky]") {
  // Equal-weight pool so that without stickiness every pick would tie-break to renderD128.
  GpuBalancer balancer;
  balancer.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 1, .excluded = false};
  balancer.pool["/dev/dri/renderD136"] = GpuInfo{.weight = 1, .excluded = false};

  SECTION("first app from a client is load-balanced as normal") {
    // No sticky entry yet -> falls through to the load-balance loop.
    auto chosen = balancer.pick(std::nullopt, std::string("10.0.0.1"));
    REQUIRE(chosen.has_value());
  }

  SECTION("second app from the same client sticks to the first node even when it is busier") {
    // Client 10.0.0.1 gets renderD128 on its first app.
    auto b = balancer.acquire_sticky("/dev/dri/renderD128", std::string("10.0.0.1"));
    // A different client grabs renderD136, so 128 is now the busier node.
    b = b.acquire("/dev/dri/renderD136");
    // Without stickiness, pick would return renderD136 (usage 1 vs 1, tie-break by path -> 128? no:
    // both at usage 1, tie broken by lowest usage then path; both equal so path wins -> 128).
    // To make the test unambiguous, load 128 further so it is strictly busier.
    b = b.acquire("/dev/dri/renderD128");
    // Now: 128=2 (score 2.0), 136=1 (score 1.0). Load-balance alone would pick 136.
    REQUIRE(b.pick(std::nullopt, std::string("10.0.0.1")) == "/dev/dri/renderD128");
    // A different client with no sticky entry still gets the least-loaded node.
    REQUIRE(b.pick(std::nullopt, std::string("10.0.0.2")) == "/dev/dri/renderD136");
  }

  SECTION("sticky is released when the client's stream stops") {
    auto b = balancer.acquire_sticky("/dev/dri/renderD128", std::string("10.0.0.1"));
    b = b.acquire("/dev/dri/renderD136");
    b = b.acquire("/dev/dri/renderD128"); // 128=2, 136=1 -> 128 is busier
    // Unstick the client (as StopStreamEvent does) and release its node.
    b = b.release("/dev/dri/renderD128").unstick("10.0.0.1");
    // Now: 128=1, 136=1. No sticky entry for 10.0.0.1 -> load-balance picks by path -> 128.
    REQUIRE(b.pick(std::nullopt, std::string("10.0.0.1")) == "/dev/dri/renderD128");
  }

  SECTION("sticky falls through to load-balance if the stuck node is no longer available") {
    auto b = balancer.acquire_sticky("/dev/dri/renderD128", std::string("10.0.0.1"));
    // Exclude the node the client was stuck to (simulates GPU disappearing).
    b.pool["/dev/dri/renderD128"].excluded = true;
    REQUIRE(b.pick(std::nullopt, std::string("10.0.0.1")) == "/dev/dri/renderD136");
  }

  SECTION("no client_key means no stickiness (lobbies / shared sessions)") {
    auto b = balancer.acquire_sticky("/dev/dri/renderD128", std::nullopt);
    // acquire_sticky with nullopt must not create a sticky entry.
    REQUIRE(b.sticky.empty());
  }

  SECTION("acquire_sticky records the mapping and is visible in the snapshot") {
    auto b = balancer.acquire_sticky("/dev/dri/renderD136", std::string("192.168.1.50"));
    REQUIRE(b.sticky.at("192.168.1.50") == "/dev/dri/renderD136");
  }

  SECTION("unstick on an unknown client is a no-op") {
    auto b = balancer.unstick("does-not-exist");
    REQUIRE(b.sticky.empty());
  }
}

TEST_CASE("apply_encoder_node re-points the encoder at the session's render node", "[encoder_node]") {
  SECTION("NVIDIA: nvh264enc gets cuda-device=<index>") {
    // get_nvidia_device_index is a real syscall-based helper; on a host without NVIDIA it returns
    // nullopt and apply_encoder_node leaves the pipeline unchanged.  Guard with the vendor check so
    // the test is meaningful only where the mapping can succeed.
    auto node = "/dev/dri/renderD128";
    if (get_vendor(node) == GPU_VENDOR::NVIDIA) {
      auto idx = get_nvidia_device_index(node);
      REQUIRE(idx.has_value());
      auto in = "nvh264enc preset=low-latency-hq zerolatency=true ! fakesink";
      auto out = apply_encoder_node(in, node);
      REQUIRE(out.find("nvh264enc cuda-device=" + *idx) != std::string::npos);
    }
  }

  SECTION("NVIDIA: already-scoped encoder is not double-annotated") {
    auto node = "/dev/dri/renderD128";
    if (get_vendor(node) == GPU_VENDOR::NVIDIA) {
      auto idx = get_nvidia_device_index(node);
      REQUIRE(idx.has_value());
      auto in = "nvh264enc cuda-device=1 preset=low-latency-hq ! fakesink";
      auto out = apply_encoder_node(in, node);
      // The negative lookahead must prevent a second cuda-device from being inserted.
      REQUIRE(std::count(out.begin(), out.end(), 'c') == std::count(in.begin(), in.end(), 'c'));
    }
  }

  SECTION("unknown vendor returns the pipeline unchanged") {
    auto in = "nvh264enc preset=low-latency-hq ! fakesink";
    // A path that does not resolve to a known DRM device -> UNKNOWN vendor.
    auto out = apply_encoder_node(in, "/dev/dri/renderD999");
    REQUIRE(out == in);
  }

  SECTION("pipeline with no recognisable encoder is returned unchanged") {
    auto node = "/dev/dri/renderD128";
    if (get_vendor(node) != GPU_VENDOR::UNKNOWN) {
      auto in = "x264enc speed-preset=ultrafast ! fakesink";
      auto out = apply_encoder_node(in, node);
      REQUIRE(out == in);
    }
  }
}
