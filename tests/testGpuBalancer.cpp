#include <catch2/catch_test_macros.hpp>
#include <immer/atom.hpp>
#include <memory>
#include <state/gpu_balancer.hpp>
#include <thread>

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

TEST_CASE("GpuBalancer picks the least-loaded GPU by (usage+1)/weight", "[gpu_balancer]") {
  auto balancer = make_balancer();

  SECTION("an empty pool picks the highest-weighted node") {
    REQUIRE(balancer.pick() == "/dev/dri/renderD128");
  }

  SECTION("higher-weight GPU takes more apps before spilling over") {
    // Weights 4 vs 1. The strong GPU takes the first three: with usage 2 it scores 3/4 = 0.75, still
    // below the weak GPU's 1/1. On the fourth pick 4/4 = 1.0 ties with 1/1 = 1.0 and the tie-break
    // (lowest absolute usage) hands the idle weak GPU its first container.
    auto b = balancer;
    for (int i = 0; i < 3; ++i) {
      REQUIRE(b.pick() == "/dev/dri/renderD128");
      b = b.acquire("/dev/dri/renderD128");
    }
    // 128=3 (4/4 = 1.0) vs 136=0 (1/1 = 1.0) -> the weak GPU gets one.
    REQUIRE(b.pick() == "/dev/dri/renderD136");
    b = b.acquire("/dev/dri/renderD136");

    // From here it settles at the 4:1 ratio the weight asks for: four on the strong GPU, then one.
    for (int i = 0; i < 4; ++i) {
      REQUIRE(b.pick() == "/dev/dri/renderD128");
      b = b.acquire("/dev/dri/renderD128");
    }
    // 128=8 (9/4 = 2.25)? No: 128=7 (8/4 = 2.0) ties with 136=1 (2/1 = 2.0) -> weak GPU again.
    REQUIRE(b.pick() == "/dev/dri/renderD136");
  }

  SECTION("a freed GPU is reused before assigning to another") {
    // Equal weights, so the ratio is not what decides here: both start idle and one is released.
    GpuBalancer b;
    b.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 1, .excluded = false};
    b.pool["/dev/dri/renderD136"] = GpuInfo{.weight = 1, .excluded = false};
    b = b.acquire("/dev/dri/renderD136");
    // 128=0 (1/1 = 1.0) vs 136=1 (2/1 = 2.0) -> the free GPU wins instead of round-robining.
    REQUIRE(b.pick() == "/dev/dri/renderD128");
  }

  SECTION("release floors at zero and is idempotent below zero") {
    // Acquire first so the node has a usage entry; releasing an unknown node is a no-op and
    // would leave no entry to inspect.
    auto b = balancer.acquire("/dev/dri/renderD128").release("/dev/dri/renderD128").release("/dev/dri/renderD128");
    REQUIRE(b.usage.at("/dev/dri/renderD128") == 0);
  }
}

TEST_CASE("GpuBalancer honours the weight ratio for a stronger GPU", "[gpu_balancer]") {
  // Regression: renderD128 (weight 9) sorts before renderD129 (weight 10), so the path tie-break
  // used to win and the *weaker* GPU was always picked first.
  GpuBalancer balancer;
  balancer.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 9, .excluded = false};
  balancer.pool["/dev/dri/renderD129"] = GpuInfo{.weight = 10, .excluded = false};

  SECTION("the highest-weighted GPU is picked first even when it sorts later") {
    REQUIRE(balancer.pick() == "/dev/dri/renderD129");
  }

  SECTION("the weight ratio is honoured while both GPUs are in use") {
    // 129=1 (2/10 = 0.2) vs 128=0 (1/9 = 0.111) -> the idle, lighter GPU takes the next container.
    auto b = balancer.acquire("/dev/dri/renderD129");
    REQUIRE(b.pick() == "/dev/dri/renderD128");

    // 129=1 (0.2) vs 128=1 (2/9 = 0.222) -> back to the heavier GPU.
    b = b.acquire("/dev/dri/renderD128");
    REQUIRE(b.pick() == "/dev/dri/renderD129");
  }

  SECTION("lowest absolute usage still outranks the weight on an equal score") {
    // 128=1 with weight 4 -> 2/4 = 0.5, 129=4 with weight 16 -> 5/16 = 0.3125? No: pick the numbers
    // so the scores tie exactly, then assert the least-loaded GPU wins rather than piling another
    // container onto the big one.
    GpuBalancer b;
    b.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 2, .excluded = false};
    b.pool["/dev/dri/renderD129"] = GpuInfo{.weight = 4, .excluded = false};
    // 128: usage 1, (1+1)/2 = 1.0   129: usage 3, (3+1)/4 = 1.0 -> equal, so usage decides: 128.
    b = b.acquire("/dev/dri/renderD128");
    for (int i = 0; i < 3; ++i)
      b = b.acquire("/dev/dri/renderD129");
    REQUIRE(b.pick() == "/dev/dri/renderD128");
  }
}

TEST_CASE("GpuBalancer excludes GPUs from the pool", "[gpu_balancer]") {
  auto balancer = make_balancer();
  balancer.pool["/dev/dri/renderD128"].excluded = true;

  SECTION("excluded node is never picked") {
    REQUIRE(balancer.pick() == "/dev/dri/renderD136");
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

  SECTION("a [gpus] node that discovery missed is still added, with its weight") {
    // Discovery is best-effort, so an explicitly configured node must not be dropped silently.
    std::map<std::string, int> weights = {{"/dev/dri/renderD129", 10}};
    auto balancer = GpuBalancer::from_pool(discovered, weights, {}, "/dev/dri/renderD128");
    REQUIRE(balancer.pool.count("/dev/dri/renderD129") == 1);
    REQUIRE(balancer.pool.at("/dev/dri/renderD129").weight == 10);
    // ...and it is the strongest node, so it wins the first pick.
    REQUIRE(balancer.pick() == "/dev/dri/renderD129");
  }
}

TEST_CASE("GpuBalancer::pick_preferring uses the preferred node when possible", "[gpu_balancer]") {
  auto balancer = make_balancer();

  SECTION("no preference falls back to load balancing") {
    REQUIRE(balancer.pick_preferring(std::nullopt) == "/dev/dri/renderD128");
  }

  SECTION("a preferred node wins even when it is the busier one") {
    // renderD136 is otherwise the idle GPU, but the caller asked for renderD128.
    auto b = balancer.acquire("/dev/dri/renderD136");
    REQUIRE(b.pick_preferring(std::optional<std::string>("/dev/dri/renderD128")) == "/dev/dri/renderD128");
  }

  SECTION("an unknown or excluded preferred node falls back to load balancing") {
    REQUIRE(balancer.pick_preferring(std::optional<std::string>("/dev/dri/renderD999")) == "/dev/dri/renderD128");

    auto excluded = make_balancer();
    excluded.pool["/dev/dri/renderD128"].excluded = true;
    REQUIRE(excluded.pick_preferring(std::optional<std::string>("/dev/dri/renderD128")) == "/dev/dri/renderD136");
  }

  SECTION("an avoided preferred node is skipped in favour of load balancing") {
    // This is what lets the runtime walk past a node the probe just rejected instead of looping on it.
    REQUIRE(balancer.pick_preferring(std::optional<std::string>("/dev/dri/renderD128"),
                                     {"/dev/dri/renderD128"}) == "/dev/dri/renderD136");
  }

  SECTION("an empty pool still yields nullopt") {
    GpuBalancer empty;
    REQUIRE(empty.pick_preferring(std::optional<std::string>("/dev/dri/renderD128")).has_value() == false);
  }
}

TEST_CASE("GpuBalancer::pick_avoiding skips the given nodes", "[gpu_balancer]") {
  auto balancer = make_balancer();

  SECTION("the avoided node is never returned while another is available") {
    REQUIRE(balancer.pick_avoiding({"/dev/dri/renderD128"}) == "/dev/dri/renderD136");
    REQUIRE(balancer.pick_avoiding({"/dev/dri/renderD136"}) == "/dev/dri/renderD128");
  }

  SECTION("everything avoided yields nullopt") {
    REQUIRE(balancer.pick_avoiding({"/dev/dri/renderD128", "/dev/dri/renderD136"}).has_value() == false);
  }
}

// A shared desktop (lobby) must not emit device-local memory when its consumers can land on more
// than one GPU; these helpers decide that. Excluded nodes are never handed out, so they must not
// count towards "multi-GPU" either.
TEST_CASE("GpuBalancer reports whether sessions can be placed on more than one GPU", "[gpu_balancer]") {
  SECTION("a single-node pool is not multi-GPU") {
    GpuBalancer single;
    single.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 1, .excluded = false};
    REQUIRE(single.usable_node_count() == 1);
    REQUIRE(single.is_multi_gpu() == false);
  }

  SECTION("an empty pool is not multi-GPU") {
    GpuBalancer empty;
    REQUIRE(empty.usable_node_count() == 0);
    REQUIRE(empty.is_multi_gpu() == false);
  }

  SECTION("two usable nodes are multi-GPU") {
    auto balancer = make_balancer();
    REQUIRE(balancer.usable_node_count() == 2);
    REQUIRE(balancer.is_multi_gpu() == true);
  }

  SECTION("an excluded node does not make the pool multi-GPU") {
    auto balancer = make_balancer();
    balancer.pool["/dev/dri/renderD136"].excluded = true;
    REQUIRE(balancer.usable_node_count() == 1);
    REQUIRE(balancer.is_multi_gpu() == false);
  }

  SECTION("exclusion leaves a pool at one usable node even with several nodes present") {
    GpuBalancer balancer;
    balancer.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 1, .excluded = false};
    balancer.pool["/dev/dri/renderD136"] = GpuInfo{.weight = 1, .excluded = true};
    balancer.pool["/dev/dri/renderD137"] = GpuInfo{.weight = 1, .excluded = true};
    REQUIRE(balancer.usable_node_count() == 1);
    REQUIRE(balancer.is_multi_gpu() == false);
  }
}

TEST_CASE("pick_and_acquire reserves atomically and skips unusable nodes", "[gpu_balancer]") {
  auto make_pool = [] {
    auto bal = std::make_shared<immer::atom<GpuBalancer>>(GpuBalancer{});
    bal->update([](const GpuBalancer &b) {
      auto next = b;
      // Equal weights so the resulting spread is unambiguous: any skew would mean two callers were
      // handed the same node from the same snapshot.
      next.pool["/dev/dri/renderD128"] = GpuInfo{.weight = 1, .excluded = false};
      next.pool["/dev/dri/renderD136"] = GpuInfo{.weight = 1, .excluded = false};
      return next;
    });
    return bal;
  };

  SECTION("the chosen node is reserved exactly once") {
    auto bal = make_pool();
    auto chosen = state::pick_and_acquire(bal, [](const GpuBalancer &b, const std::vector<std::string> &avoid) {
      return b.pick_avoiding(avoid);
    }, [](const std::string &) { return true; });

    REQUIRE(chosen == "/dev/dri/renderD128");
    REQUIRE(bal->load()->usage.at("/dev/dri/renderD128") == 1);
    // The losing node must not have been touched.
    REQUIRE(bal->load()->usage.count("/dev/dri/renderD136") == 0);
  }

  SECTION("a failing probe skips to the next node without reserving the dead one") {
    auto bal = make_pool();
    // renderD128 is the best-scoring node, but the probe says it is unusable.
    auto chosen = state::pick_and_acquire(bal, [](const GpuBalancer &b, const std::vector<std::string> &avoid) {
      return b.pick_avoiding(avoid);
    }, [](const std::string &node) { return node != "/dev/dri/renderD128"; });

    REQUIRE(chosen == "/dev/dri/renderD136");
    // Crucially the dead node has no usage entry: had it been "acquired" (even at 0) it would look
    // idle forever and win every subsequent pick.
    REQUIRE(bal->load()->usage.count("/dev/dri/renderD128") == 0);
    REQUIRE(bal->load()->usage.at("/dev/dri/renderD136") == 1);
  }

  SECTION("every node failing the probe yields nullopt and changes nothing") {
    auto bal = make_pool();
    auto chosen = state::pick_and_acquire(bal, [](const GpuBalancer &b, const std::vector<std::string> &avoid) {
      return b.pick_avoiding(avoid);
    }, [](const std::string &) { return false; });

    REQUIRE(chosen.has_value() == false);
    REQUIRE(bal->load()->usage.empty());
  }

  SECTION("concurrent acquisitions never hand out more copies of a node than it can take") {
    // Two GPUs, weights 1/1: four acquisitions must spread 2+2. Without the atomic pick+acquire two
    // sessions racing on the same snapshot would both be handed the same GPU.
    auto bal = make_pool();
    auto choose = [](const GpuBalancer &b, const std::vector<std::string> &avoid) {
      return b.pick_avoiding(avoid);
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&bal, &choose] {
        state::pick_and_acquire(bal, choose, [](const std::string &) { return true; });
      });
    }
    for (auto &t : threads)
      t.join();

    auto snapshot = bal->load();
    REQUIRE(snapshot->usage.at("/dev/dri/renderD128") == 2);
    REQUIRE(snapshot->usage.at("/dev/dri/renderD136") == 2);
  }
}
