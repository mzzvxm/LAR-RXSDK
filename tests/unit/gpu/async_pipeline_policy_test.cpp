/**
 * @file        tests/unit/gpu/async_pipeline_policy_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <optional>

#include "gpu/xenos/async_pipeline_policy.h"

using namespace rex::gpu::async_pipeline;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

REX_TEST_CASE("A budget of 0 leaves every draw out at once", "[gpu][async_pipeline]") {
  CHECK(GetBudgetedWaitUs(0, 0) == 0);
  CHECK(GetBudgetedWaitUs(0, 12345) == 0);
}

REX_TEST_CASE("A budget of -1 waits as long as it takes", "[gpu][async_pipeline]") {
  CHECK(GetBudgetedWaitUs(-1, 0) == kUnlimitedWaitUs);
  CHECK(GetBudgetedWaitUs(-1, 1'000'000'000) == kUnlimitedWaitUs);
}

REX_TEST_CASE("A draw may wait for what is left of the frame's budget", "[gpu][async_pipeline]") {
  // 4 ms of budget: a first draw gets all of it, a later one what the earlier ones left.
  CHECK(GetBudgetedWaitUs(4, 0) == 4000);
  CHECK(GetBudgetedWaitUs(4, 1500) == 2500);
  CHECK(GetBudgetedWaitUs(4, 3999) == 1);
}

REX_TEST_CASE("A spent budget leaves the draws out", "[gpu][async_pipeline]") {
  CHECK(GetBudgetedWaitUs(4, 4000) == 0);
  CHECK(GetBudgetedWaitUs(4, 4001) == 0);
  CHECK(GetBudgetedWaitUs(1000, 1'000'000) == 0);
}

REX_TEST_CASE("The waits are what the setting's range allows", "[gpu][async_pipeline]") {
  // The setting goes up to 1000 ms; the wait for a draw in an occlusion query is its own.
  CHECK(GetBudgetedWaitUs(1000, 0) == 1'000'000);
  CHECK(kOcclusionQueryWaitUs == 500'000);
  CHECK(kUnlimitedWaitUs > 1'000'000);
}

REX_TEST_CASE("A complete frame is presented and keeps no run", "[gpu][async_pipeline]") {
  const auto t0 = steady_clock::now();
  std::optional<steady_clock::time_point> since;
  CHECK_FALSE(ShouldSkipPresent(false, t0, 100, since));
  CHECK_FALSE(since.has_value());
}

REX_TEST_CASE("The first incomplete frame starts a run and is skipped", "[gpu][async_pipeline]") {
  const auto t0 = steady_clock::now();
  std::optional<steady_clock::time_point> since;
  CHECK(ShouldSkipPresent(true, t0, 100, since));
  REQUIRE(since.has_value());
  CHECK(*since == t0);
}

REX_TEST_CASE("Incomplete frames are skipped until the run reaches the limit",
              "[gpu][async_pipeline]") {
  const auto t0 = steady_clock::now();
  std::optional<steady_clock::time_point> since;
  CHECK(ShouldSkipPresent(true, t0, 100, since));
  CHECK(ShouldSkipPresent(true, t0 + milliseconds(16), 100, since));
  CHECK(ShouldSkipPresent(true, t0 + milliseconds(99), 100, since));
  // From the limit on, the frame is shown with the draws missing, and stays shown...
  CHECK_FALSE(ShouldSkipPresent(true, t0 + milliseconds(100), 100, since));
  CHECK_FALSE(ShouldSkipPresent(true, t0 + milliseconds(500), 100, since));
  // ...with the run still on, measured from the first skip.
  REQUIRE(since.has_value());
  CHECK(*since == t0);
}

REX_TEST_CASE("A complete frame ends the run and the next incomplete one starts a new one",
              "[gpu][async_pipeline]") {
  const auto t0 = steady_clock::now();
  std::optional<steady_clock::time_point> since;
  CHECK(ShouldSkipPresent(true, t0, 100, since));
  CHECK_FALSE(ShouldSkipPresent(true, t0 + milliseconds(150), 100, since));
  CHECK_FALSE(ShouldSkipPresent(false, t0 + milliseconds(160), 100, since));
  CHECK_FALSE(since.has_value());

  const auto t1 = t0 + milliseconds(170);
  CHECK(ShouldSkipPresent(true, t1, 100, since));
  CHECK(ShouldSkipPresent(true, t1 + milliseconds(99), 100, since));
  CHECK_FALSE(ShouldSkipPresent(true, t1 + milliseconds(100), 100, since));
}

REX_TEST_CASE("A limit of 0 skips for as long as it takes", "[gpu][async_pipeline]") {
  const auto t0 = steady_clock::now();
  std::optional<steady_clock::time_point> since;
  CHECK(ShouldSkipPresent(true, t0, 0, since));
  CHECK(ShouldSkipPresent(true, t0 + std::chrono::seconds(10), 0, since));
  CHECK(ShouldSkipPresent(true, t0 + std::chrono::hours(1), 0, since));
  CHECK_FALSE(ShouldSkipPresent(false, t0 + std::chrono::hours(1), 0, since));
  CHECK_FALSE(since.has_value());
}
