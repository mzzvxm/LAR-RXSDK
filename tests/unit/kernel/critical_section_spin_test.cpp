/**
 * @file        tests/unit/kernel/critical_section_spin_test.cpp
 * @brief       The spin phase of RtlEnterCriticalSection
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "kernel/xboxkrnl/critical_section_spin.h"

using rex::kernel::xboxkrnl::SpinAcquire;
using namespace std::chrono_literals;

REX_TEST_CASE("Spin acquire takes a free lock word", "[kernel][critical_section]") {
  i32 lock_count = -1;
  CHECK(SpinAcquire(std::atomic_ref<i32>(lock_count), 1));
  CHECK(lock_count == 0);
}

REX_TEST_CASE("Spin count 0 does not touch the lock word", "[kernel][critical_section]") {
  i32 free_lock = -1;
  CHECK_FALSE(SpinAcquire(std::atomic_ref<i32>(free_lock), 0));
  CHECK(free_lock == -1);

  i32 held_lock = 0;
  CHECK_FALSE(SpinAcquire(std::atomic_ref<i32>(held_lock), 0));
  CHECK(held_lock == 0);
}

REX_TEST_CASE("Spin acquire gives up on a held lock word and leaves it alone",
              "[kernel][critical_section]") {
  i32 held = 0;
  CHECK_FALSE(SpinAcquire(std::atomic_ref<i32>(held), 1000));
  CHECK(held == 0);

  i32 held_with_waiters = 3;
  CHECK_FALSE(SpinAcquire(std::atomic_ref<i32>(held_with_waiters), 1000));
  CHECK(held_with_waiters == 3);
}

REX_TEST_CASE("Spin acquire takes the lock when it is released during the spin",
              "[kernel][critical_section]") {
  i32 lock_count = 0;  // held by somebody else
  std::thread releaser([&lock_count] {
    std::this_thread::sleep_for(20ms);
    std::atomic_ref<i32>(lock_count).store(-1, std::memory_order_release);
  });
  // The spin count is far more than the releaser needs; a loop that gives up early fails.
  const bool acquired = SpinAcquire(std::atomic_ref<i32>(lock_count), 0xFFFFFFFFu);
  releaser.join();
  CHECK(acquired);
  CHECK(lock_count == 0);
}

REX_TEST_CASE("Only one of many spinners takes a free lock word", "[kernel][critical_section]") {
  constexpr int kThreads = 6;
  constexpr int kRounds = 500;
  for (int round = 0; round < kRounds; ++round) {
    i32 lock_count = -1;
    std::atomic<int> start{0};
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        start.fetch_add(1);
        while (start.load() < kThreads) {
          std::this_thread::yield();
        }
        if (SpinAcquire(std::atomic_ref<i32>(lock_count), 64)) {
          winners.fetch_add(1);
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    REQUIRE(winners.load() == 1);
    REQUIRE(lock_count == 0);
  }
}
