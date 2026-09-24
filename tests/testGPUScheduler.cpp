#include <catch2/catch_test_macros.hpp>

#include <state/gpu_scheduler.hpp>

TEST_CASE("GPU scheduler assigns weighted least-loaded sessions") {
  state::GPUScheduler scheduler({
      {.render_node = "/dev/dri/renderD128", .weight = 10},
      {.render_node = "/dev/dri/renderD129", .weight = 7},
  });

  auto first = scheduler.acquire();
  REQUIRE(first);
  REQUIRE(first->render_node == "/dev/dri/renderD128");

  auto second = scheduler.acquire();
  REQUIRE(second);
  REQUIRE(second->render_node == "/dev/dri/renderD129");

  auto third = scheduler.acquire();
  REQUIRE(third);
  REQUIRE(third->render_node == "/dev/dri/renderD128");

  scheduler.release(*first);
  scheduler.release(*second);
  scheduler.release(*third);
  for (const auto &[node, active] : scheduler.loads()) {
    REQUIRE(active == 0);
  }
}

TEST_CASE("GPU scheduler rejects invalid entries and duplicate nodes") {
  state::GPUScheduler scheduler({
      {.render_node = "", .weight = 1},
      {.render_node = "/dev/dri/renderD128", .weight = 0},
      {.render_node = "/dev/dri/renderD128", .weight = 3},
      {.render_node = "/dev/dri/renderD128", .weight = 4},
  });
  auto assignment = scheduler.acquire();
  REQUIRE(assignment);
  REQUIRE(assignment->render_node == "/dev/dri/renderD128");
  auto second = scheduler.acquire();
  REQUIRE(second);
  REQUIRE(second->render_node == "/dev/dri/renderD128");
  scheduler.release(*assignment);
  scheduler.release(*second);
}
