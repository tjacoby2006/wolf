#include <catch2/catch_test_macros.hpp>
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
    // Acquire first so the node has a usage entry; releasing an unknown node is a no-op and
    // would leave no entry to inspect.
    auto b = balancer.acquire("/dev/dri/renderD128").release("/dev/dri/renderD128").release("/dev/dri/renderD128");
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
  // The [gpus] table keys on the full render node path (see docs: "/dev/dri/renderD128" = 4).
  std::map<std::string, int> weights = {{"/dev/dri/renderD128", 4}};
  std::vector<std::string> excluded = {"/dev/dri/renderD136"};

  SECTION("applies weights and exclusions, always keeps the default node") {
    auto balancer = GpuBalancer::from_pool(discovered, weights, excluded, "/dev/dri/renderD128");
    REQUIRE(balancer.pool.count("/dev/dri/renderD128") == 1);
    REQUIRE(balancer.pool.count("/dev/dri/renderD136") == 1);
    // The configured weight must actually be applied to the matching node.
    REQUIRE(balancer.pool.at("/dev/dri/renderD128").weight == 4);
    // A node with no configured weight defaults to 1.
    REQUIRE(balancer.pool.at("/dev/dri/renderD136").weight == 1);
    REQUIRE(balancer.pool.at("/dev/dri/renderD136").excluded == true);
    // default node kept even though it would otherwise be absent
    auto with_default = GpuBalancer::from_pool({}, {}, {}, "/dev/dri/renderD128");
    REQUIRE(with_default.pool.count("/dev/dri/renderD128") == 1);
  }

  SECTION("weight below 1 is clamped to 1") {
    std::map<std::string, int> low_weights = {{"/dev/dri/renderD128", 0}};
    auto balancer = GpuBalancer::from_pool(discovered, low_weights, {}, "/dev/dri/renderD128");
    REQUIRE(balancer.pool.at("/dev/dri/renderD128").weight == 1);
  }
}
