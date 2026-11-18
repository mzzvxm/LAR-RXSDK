/**
 * @file        tests/unit/hid/joystick_update_gate_test.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <thread>
#include <vector>

#include <rex/types.h>

#include "hid/sdl3/joystick_update_gate.h"

using rex::hid::sdl3::JoystickUpdateGate;

namespace {
constexpr u64 kMs = 1'000'000;
}

REX_TEST_CASE("The first read is always due", "[hid][sdl3]") {
  JoystickUpdateGate gate;
  REQUIRE(gate.TryBegin(1, JoystickUpdateGate::kGuestMinGapNs));
}

REX_TEST_CASE("Guest calls in the same millisecond share one read", "[hid][sdl3]") {
  JoystickUpdateGate gate;
  const u64 start = 5 * kMs;
  REQUIRE(gate.TryBegin(start, JoystickUpdateGate::kGuestMinGapNs));
  // The rest of one frame's calls: capabilities and state for each user.
  for (u64 offset = 1; offset <= 8; offset++) {
    REQUIRE_FALSE(gate.TryBegin(start + offset * 10'000, JoystickUpdateGate::kGuestMinGapNs));
  }
  // The next frame, 4 ms later, reads again.
  REQUIRE(gate.TryBegin(start + 4 * kMs, JoystickUpdateGate::kGuestMinGapNs));
}

REX_TEST_CASE("A read exactly one gap later is due, one nanosecond earlier is not",
              "[hid][sdl3]") {
  JoystickUpdateGate gate;
  const u64 start = 10 * kMs;
  REQUIRE(gate.TryBegin(start, JoystickUpdateGate::kGuestMinGapNs));
  REQUIRE_FALSE(gate.TryBegin(start + JoystickUpdateGate::kGuestMinGapNs - 1,
                              JoystickUpdateGate::kGuestMinGapNs));
  REQUIRE(gate.TryBegin(start + JoystickUpdateGate::kGuestMinGapNs,
                        JoystickUpdateGate::kGuestMinGapNs));
}

REX_TEST_CASE("The idle timer stays out of the way while the guest reads every frame",
              "[hid][sdl3]") {
  JoystickUpdateGate gate;
  u64 now = kMs;
  int idle_reads = 0;
  int guest_reads = 0;
  // 2 seconds of a 240 fps game, the timer ticking every kIdleTimerMs.
  u64 next_tick = now + JoystickUpdateGate::kIdleTimerMs * kMs;
  for (int frame = 0; frame < 480; frame++) {
    now += 4'166'667;
    if (gate.TryBegin(now, JoystickUpdateGate::kGuestMinGapNs)) {
      guest_reads++;
    }
    while (next_tick <= now) {
      if (gate.TryBegin(next_tick, JoystickUpdateGate::kIdleMaxAgeNs)) {
        idle_reads++;
      }
      next_tick += JoystickUpdateGate::kIdleTimerMs * kMs;
    }
  }
  REQUIRE(guest_reads == 480);  // one per frame
  REQUIRE(idle_reads == 0);     // the timer never had to read
}

REX_TEST_CASE("The idle timer reads every tick when the guest is silent", "[hid][sdl3]") {
  JoystickUpdateGate gate;
  // 5 seconds with no guest calls: a read at every tick, so a new pad is seen within 500 ms.
  int reads = 0;
  for (u64 tick = 1; tick <= 10; tick++) {
    if (gate.TryBegin(tick * JoystickUpdateGate::kIdleTimerMs * kMs,
                      JoystickUpdateGate::kIdleMaxAgeNs)) {
      reads++;
    }
  }
  REQUIRE(reads == 10);
}

REX_TEST_CASE("The idle timer reads again once the guest has been silent long enough",
              "[hid][sdl3]") {
  JoystickUpdateGate gate;
  REQUIRE(gate.TryBegin(100 * kMs, JoystickUpdateGate::kGuestMinGapNs));
  // 300 ms later (less than kIdleMaxAgeNs): the guest was here a moment ago.
  REQUIRE_FALSE(gate.TryBegin(400 * kMs, JoystickUpdateGate::kIdleMaxAgeNs));
  // 500 ms after the last guest read: due.
  REQUIRE(gate.TryBegin(600 * kMs, JoystickUpdateGate::kIdleMaxAgeNs));
  // And the guest then finds that read recent.
  REQUIRE_FALSE(gate.TryBegin(600 * kMs + 100'000, JoystickUpdateGate::kGuestMinGapNs));
}

REX_TEST_CASE("An older time than the recorded one counts as a read just done", "[hid][sdl3]") {
  JoystickUpdateGate gate;
  REQUIRE(gate.TryBegin(50 * kMs, JoystickUpdateGate::kGuestMinGapNs));
  // Another thread took its time stamp a moment earlier than the winner's but arrives later.
  REQUIRE_FALSE(gate.TryBegin(49 * kMs, JoystickUpdateGate::kGuestMinGapNs));
}

REX_TEST_CASE("Racing threads: exactly one wins each window", "[hid][sdl3]") {
  constexpr int kThreads = 8;
  constexpr int kWindows = 200;
  JoystickUpdateGate gate;
  std::atomic<int> winners{0};
  std::atomic<int> go_window{-1};
  std::atomic<int> arrived{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&] {
      for (int window = 0; window < kWindows; window++) {
        while (go_window.load(std::memory_order_acquire) < window) {
          std::this_thread::yield();
        }
        // Every thread uses the same time stamp for the window: only one may read.
        const u64 now = (static_cast<u64>(window) + 1) * 10 * kMs;
        if (gate.TryBegin(now, JoystickUpdateGate::kGuestMinGapNs)) {
          winners.fetch_add(1, std::memory_order_relaxed);
        }
        arrived.fetch_add(1, std::memory_order_acq_rel);
      }
    });
  }
  for (int window = 0; window < kWindows; window++) {
    go_window.store(window, std::memory_order_release);
    while (arrived.load(std::memory_order_acquire) < (window + 1) * kThreads) {
      std::this_thread::yield();
    }
  }
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE(winners.load() == kWindows);
}
