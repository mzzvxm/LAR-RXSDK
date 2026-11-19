/**
 * @file        tests/unit/kernel/kernel_time_gate_test.cpp
 * @brief       The rate limit on the CPU time a wait writes to its thread's KTHREAD
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/types.h>

#include "kernel/kernel_time_gate.h"

namespace {

using rex::u64;
using rex::kernel::KernelTimeDue;

constexpr u64 kMillisecond = 1'000'000;  // ticks of a nanosecond clock

REX_TEST_CASE("The first call writes", "[kernel][thread][kernel_time]") {
  u64 last = 0;
  CHECK(KernelTimeDue(last, 5 * kMillisecond, kMillisecond));
  CHECK(last == 5 * kMillisecond);
}

REX_TEST_CASE("A call less than the interval after a write does nothing",
              "[kernel][thread][kernel_time]") {
  u64 last = 0;
  REQUIRE(KernelTimeDue(last, 10 * kMillisecond, kMillisecond));
  CHECK_FALSE(KernelTimeDue(last, 10 * kMillisecond, kMillisecond));
  CHECK_FALSE(KernelTimeDue(last, 10 * kMillisecond + kMillisecond / 2, kMillisecond));
  CHECK_FALSE(KernelTimeDue(last, 11 * kMillisecond - 1, kMillisecond));
  CHECK(last == 10 * kMillisecond);
}

REX_TEST_CASE("A call a full interval after a write writes and starts the next interval",
              "[kernel][thread][kernel_time]") {
  u64 last = 0;
  REQUIRE(KernelTimeDue(last, 10 * kMillisecond, kMillisecond));
  CHECK(KernelTimeDue(last, 11 * kMillisecond, kMillisecond));
  CHECK(last == 11 * kMillisecond);
  CHECK_FALSE(KernelTimeDue(last, 11 * kMillisecond + 1, kMillisecond));
  CHECK(KernelTimeDue(last, 20 * kMillisecond, kMillisecond));
}

REX_TEST_CASE("An interval of 0 writes every time", "[kernel][thread][kernel_time]") {
  u64 last = 0;
  for (u64 tick = 100; tick < 110; ++tick) {
    CHECK(KernelTimeDue(last, tick, 0));
  }
}

REX_TEST_CASE("A clock that moved back writes at once", "[kernel][thread][kernel_time]") {
  u64 last = 0;
  REQUIRE(KernelTimeDue(last, 50 * kMillisecond, kMillisecond));
  CHECK(KernelTimeDue(last, 40 * kMillisecond, kMillisecond));
  CHECK(last == 40 * kMillisecond);
}

REX_TEST_CASE("A thread that waits every 2 microseconds writes once a millisecond",
              "[kernel][thread][kernel_time]") {
  constexpr u64 kWaitEvery = 2'000;  // 500,000 waits a second
  constexpr u64 kSpan = 100 * kMillisecond;
  u64 last = 0;
  u64 writes = 0;
  u64 waits = 0;
  for (u64 tick = kWaitEvery; tick <= kSpan; tick += kWaitEvery) {
    ++waits;
    if (KernelTimeDue(last, tick, kMillisecond)) {
      ++writes;
    }
  }
  CHECK(waits == 50'000);
  // One write at the first wait, then one every millisecond: 100 ms is 100 or 101 writes.
  CHECK(writes >= 100);
  CHECK(writes <= 101);
}

REX_TEST_CASE("A thread that waits less often than the interval writes every time",
              "[kernel][thread][kernel_time]") {
  u64 last = 0;
  u64 writes = 0;
  for (u64 tick = 1; tick <= 50; ++tick) {
    if (KernelTimeDue(last, tick * 2 * kMillisecond, kMillisecond)) {
      ++writes;
    }
  }
  CHECK(writes == 50);
}

}  // namespace
