/**
 * @file        tests/unit/base/thread_priority_test.cpp
 * @brief       A thread priority the host refuses still gives the thread a defined policy
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/platform.h>

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID

#include <sched.h>

#include <atomic>
#include <chrono>

#include <rex/platform/sync.h>
#include <rex/platform/thread.h>

using namespace std::chrono_literals;

// SCHED_FIFO needs CAP_SYS_NICE and a priority from 1 to 99. The priority of 0 that a thread
// starts with used to fail with EINVAL, an assert, and a boost used to fail with EPERM and leave
// the thread as it was. Both now end in SCHED_OTHER.
REX_TEST_CASE("Priority 0 puts a thread in the normal policy, a boost does not assert",
              "[base][thread]") {
  std::atomic<bool> finished{false};
  std::atomic<int> policy_after_boost{-1};
  std::atomic<int> policy_after_zero{-1};
  auto thread = rex::platform::Thread::Create({}, [&] {
    rex::platform::SetCurrentThreadPriority(rex::platform::ThreadPriority::kHighest);
    policy_after_boost = sched_getscheduler(0);
    rex::platform::SetCurrentThreadPriority(0);
    policy_after_zero = sched_getscheduler(0);
    finished = true;
  });
  REQUIRE(thread);
  REQUIRE(rex::platform::Wait(thread.get(), false, 5s) == rex::platform::WaitResult::kSuccess);

  CHECK(finished.load());
  // Real time where the process may have it, the normal policy where it may not.
  const int boosted = policy_after_boost.load();
  CHECK((boosted == SCHED_FIFO || boosted == SCHED_OTHER));
  CHECK(policy_after_zero.load() == SCHED_OTHER);
}

#endif
