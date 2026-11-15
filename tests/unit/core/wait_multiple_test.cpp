/**
 * @file        wait_multiple_test.cpp
 * @brief       Unit tests for rex::thread::WaitAny / WaitAll on several handles
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/platform.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <time.h>
#endif

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::WaitAll;
using rex::thread::WaitAny;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

namespace {

// Ends the test run, with a message, if the test is still going after `limit`. A wait that ignores
// its timeout never returns, which would hang the whole run instead of failing one test.
class HangGuard {
 public:
  explicit HangGuard(std::chrono::milliseconds limit)
      : thread_([this, limit] {
          std::unique_lock<std::mutex> lock(mutex_);
          if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
            std::fputs("wait_multiple_test: a wait did not end within its timeout\n", stderr);
            std::abort();
          }
        }) {}
  ~HangGuard() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;  // last: starts once the members above exist
};

}  // namespace

TEST_CASE("rex::thread::WaitAny - wakes on a signal from another thread", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  auto sem = Semaphore::Create(0, 4);
  WaitHandle* handles[] = {a.get(), b.get(), sem.get()};

  std::thread signaler([&] {
    std::this_thread::sleep_for(50ms);
    b->Set();
  });
  auto start = std::chrono::steady_clock::now();
  auto [result, index] = WaitAny(handles, 3, false, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
  REQUIRE(elapsed >= 40ms);
  REQUIRE(elapsed < 1000ms);

  // Semaphore release wakes it too.
  std::thread releaser([&] {
    std::this_thread::sleep_for(50ms);
    sem->Release(1, nullptr);
  });
  auto [result2, index2] = WaitAny(handles, 3, false, 5000ms);
  releaser.join();
  REQUIRE(result2 == WaitResult::kSuccess);
  REQUIRE(index2 == 2);
}

TEST_CASE("rex::thread::WaitAny - times out when nothing is signaled", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  auto start = std::chrono::steady_clock::now();
  auto [result, index] = WaitAny(handles, 2, false, 100ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  (void)index;

  REQUIRE(result == WaitResult::kTimeout);
  REQUIRE(elapsed >= 95ms);
  REQUIRE(elapsed < 1000ms);
}

TEST_CASE("rex::thread::WaitAny - returns at once for a handle already signaled", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(true);
  WaitHandle* handles[] = {a.get(), b.get()};

  auto [result, index] = WaitAny(handles, 2, false, 5000ms);
  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
}

TEST_CASE("rex::thread::WaitAll - waits for every handle", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  std::thread signaler([&] {
    std::this_thread::sleep_for(30ms);
    a->Set();
    std::this_thread::sleep_for(60ms);
    b->Set();
  });
  auto start = std::chrono::steady_clock::now();
  auto result = WaitAll(handles, 2, false, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(elapsed >= 80ms);  // not satisfied by the first event alone
  REQUIRE(elapsed < 1000ms);
}

TEST_CASE("rex::thread::WaitAll - times out while one handle stays unsignaled", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(true);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  REQUIRE(WaitAll(handles, 2, false, 50ms) == WaitResult::kTimeout);
}

TEST_CASE("rex::thread::WaitAny - ping-pong between two threads does not stall", "[thread][wait]") {
  // Each side waits on two events (its own and a never-signaled one) with no timeout and
  // signals the other. A lost wakeup would cost the full poll cap per round trip.
  constexpr int kRounds = 2000;
  auto ping = Event::CreateAutoResetEvent(false);
  auto pong = Event::CreateAutoResetEvent(false);
  auto never_a = Event::CreateAutoResetEvent(false);
  auto never_b = Event::CreateAutoResetEvent(false);

  std::atomic<int> bad{0};  // Catch assertions are not thread-safe: count here, check after join
  std::thread other([&] {
    WaitHandle* handles[] = {never_b.get(), ping.get()};
    for (int i = 0; i < kRounds; ++i) {
      auto [result, index] = WaitAny(handles, 2, false);
      if (result != WaitResult::kSuccess || index != 1) {
        ++bad;
      }
      pong->Set();
    }
  });

  WaitHandle* handles[] = {pong.get(), never_a.get()};
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kRounds; ++i) {
    ping->Set();
    auto [result, index] = WaitAny(handles, 2, false);
    REQUIRE(result == WaitResult::kSuccess);
    REQUIRE(index == 0);
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  other.join();

  REQUIRE(bad == 0);
  REQUIRE(elapsed < 5s);
}

TEST_CASE("rex::thread::WaitAny - different handles: the lowest signaled index, one taken per wait",
          "[thread][wait]") {
  HangGuard guard(30s);
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(true);
  auto c = Event::CreateAutoResetEvent(true);
  WaitHandle* handles[] = {a.get(), b.get(), c.get()};

  for (bool alertable : {false, true}) {
    INFO("alertable " << alertable);
    b->Set();
    c->Set();
    // (A signaled handle ends these at once; a zero timeout is only used where nothing is signaled:
    // an alertable wait with a zero timeout answers "timeout" before it looks at the handles.)
    auto [first, first_index] = WaitAny(handles, 3, alertable, 5000ms);
    REQUIRE(first == WaitResult::kSuccess);
    REQUIRE(first_index == 1);
    auto [second, second_index] = WaitAny(handles, 3, alertable, 5000ms);
    REQUIRE(second == WaitResult::kSuccess);
    REQUIRE(second_index == 2);
    auto [third, third_index] = WaitAny(handles, 3, alertable, 0ms);
    (void)third_index;
    REQUIRE(third == WaitResult::kTimeout);
  }
}

TEST_CASE("rex::thread::WaitAll - different handles: all taken together, none on a timeout",
          "[thread][wait]") {
  HangGuard guard(30s);
  auto a = Event::CreateAutoResetEvent(true);
  auto b = Event::CreateAutoResetEvent(true);
  auto never = Event::CreateAutoResetEvent(false);
  WaitHandle* both[] = {a.get(), b.get()};
  WaitHandle* with_never[] = {a.get(), never.get()};

  // A wait-all that times out takes nothing.
  REQUIRE(WaitAll(with_never, 2, false, 0ms) == WaitResult::kTimeout);
  REQUIRE(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kSuccess);
  a->Set();

  REQUIRE(WaitAll(both, 2, false, 0ms) == WaitResult::kSuccess);
  // Both auto-reset events were taken.
  REQUIRE(WaitAll(both, 2, false, 0ms) == WaitResult::kTimeout);
  REQUIRE(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kTimeout);
  REQUIRE(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("rex::thread::WaitAny/WaitAll - a zero timeout poll of signaled handles never times out",
          "[thread][wait]") {
  // Two threads keep taking the handles' locks (Set on an event that is already set changes
  // nothing), so now and then the poll finds a lock held. It must wait for the lock and see the
  // signaled state: a timeout here would be a false answer.
  HangGuard guard(60s);
  auto a = Event::CreateManualResetEvent(true);
  auto b = Event::CreateManualResetEvent(true);
  WaitHandle* handles[] = {a.get(), b.get()};

  std::atomic<bool> stop{false};
  std::thread hammer_a([&] {
    while (!stop) {
      a->Set();
      std::this_thread::yield();
    }
  });
  std::thread hammer_b([&] {
    while (!stop) {
      b->Set();
      std::this_thread::yield();
    }
  });

  int bad = 0;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 5000; ++i) {
    auto [result, index] = WaitAny(handles, 2, false, 0ms);
    if (result != WaitResult::kSuccess || index != 0) {
      ++bad;
    }
    if (WaitAll(handles, 2, false, 0ms) != WaitResult::kSuccess) {
      ++bad;
    }
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  stop = true;
  hammer_a.join();
  hammer_b.join();

  REQUIRE(bad == 0);
  REQUIRE(elapsed < 10s);
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
namespace {
// CPU time used so far by the calling thread.
std::chrono::nanoseconds ThreadCpuTime() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}
}  // namespace

TEST_CASE("rex::thread::WaitAny - an idle wait on several handles does not spin", "[thread][wait]") {
  // The poll used to sleep for whole milliseconds of the time left, so the last millisecond of a
  // wait (all of an alertable one: it waits in 1 ms slices) was a busy loop. 400 ms of waiting
  // must cost far less than 400 ms of CPU, alertable or not.
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  for (bool alertable : {false, true}) {
    const auto cpu_before = ThreadCpuTime();
    auto [result, index] = WaitAny(handles, 2, alertable, 400ms);
    const auto cpu_used = ThreadCpuTime() - cpu_before;
    (void)index;
    REQUIRE(result == WaitResult::kTimeout);
    INFO("alertable " << alertable << ": " << std::chrono::duration_cast<std::chrono::milliseconds>(cpu_used).count()
                      << " ms of CPU in a 400 ms wait");
    CHECK(cpu_used < 100ms);
  }
}

TEST_CASE("rex::thread::WaitAny - an alertable wait on several handles wakes for a user callback",
          "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  rex::thread::Thread* self = rex::thread::Thread::GetCurrentThread();
  std::atomic<bool> ran{false};
  std::thread queuer([&] {
    std::this_thread::sleep_for(50ms);
    self->QueueUserCallback([&] { ran = true; });
  });
  auto start = std::chrono::steady_clock::now();
  auto [result, index] = WaitAny(handles, 2, true, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  queuer.join();
  (void)index;

  REQUIRE(result == WaitResult::kUserCallback);
  CHECK(ran.load());
  CHECK(elapsed >= 40ms);
  CHECK(elapsed < 1000ms);
}

TEST_CASE("rex::thread::WaitAny - an alertable wait on several handles wakes for a signal",
          "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  std::thread signaler([&] {
    std::this_thread::sleep_for(50ms);
    a->Set();
  });
  const auto cpu_before = ThreadCpuTime();
  auto [result, index] = WaitAny(handles, 2, true, 5000ms);
  const auto cpu_used = ThreadCpuTime() - cpu_before;
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  CHECK(index == 0);
  CHECK(cpu_used < 30ms);
}
#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
// A handle listed twice. The POSIX scan locks every handle's mutex in turn with trylock and used to
// retry for ever on the second lock of the same mutex, without looking at the timeout. It now waits
// on such a handle once. (CHECK: Windows is not asserted here; its WaitForMultipleObjects documents
// a repeated handle as invalid and fails a wait-all that has one.)

TEST_CASE("rex::thread::WaitAny - a handle listed twice times out within the timeout",
          "[thread][wait][duplicate]") {
  HangGuard guard(30s);
  auto a = Event::CreateAutoResetEvent(false);
  WaitHandle* twice[] = {a.get(), a.get()};
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* mixed[] = {a.get(), b.get(), a.get()};

  for (bool alertable : {false, true}) {
    for (WaitHandle** handles : {twice, mixed}) {
      const size_t count = handles == twice ? 2 : 3;
      INFO("alertable " << alertable << ", " << count << " handles");

      auto start = std::chrono::steady_clock::now();
      auto [result, index] = WaitAny(handles, count, alertable, 100ms);
      auto elapsed = std::chrono::steady_clock::now() - start;
      (void)index;
      REQUIRE(result == WaitResult::kTimeout);
      CHECK(elapsed >= 90ms);
      CHECK(elapsed < 1000ms);

      // A zero timeout answers at once.
      start = std::chrono::steady_clock::now();
      auto [zero_result, zero_index] = WaitAny(handles, count, alertable, 0ms);
      elapsed = std::chrono::steady_clock::now() - start;
      (void)zero_index;
      REQUIRE(zero_result == WaitResult::kTimeout);
      CHECK(elapsed < 500ms);
    }
  }
}

TEST_CASE("rex::thread::WaitAll - a handle listed twice times out within the timeout",
          "[thread][wait][duplicate]") {
  HangGuard guard(30s);
  auto a = Event::CreateAutoResetEvent(false);
  WaitHandle* twice[] = {a.get(), a.get()};

  for (bool alertable : {false, true}) {
    INFO("alertable " << alertable);
    auto start = std::chrono::steady_clock::now();
    REQUIRE(WaitAll(twice, 2, alertable, 100ms) == WaitResult::kTimeout);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 90ms);
    CHECK(elapsed < 1000ms);
    REQUIRE(WaitAll(twice, 2, alertable, 0ms) == WaitResult::kTimeout);
  }
}

TEST_CASE("rex::thread::WaitAny - a handle listed twice returns its lowest index when signaled",
          "[thread][wait][duplicate]") {
  HangGuard guard(30s);
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get(), b.get()};
  WaitHandle* first_twice[] = {b.get(), a.get(), b.get()};

  for (bool alertable : {false, true}) {
    INFO("alertable " << alertable);

    // Signaled from another thread while the wait sleeps.
    std::thread signaler([&] {
      std::this_thread::sleep_for(50ms);
      b->Set();
    });
    auto start = std::chrono::steady_clock::now();
    auto [result, index] = WaitAny(handles, 3, alertable, 5000ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    signaler.join();
    REQUIRE(result == WaitResult::kSuccess);
    REQUIRE(index == 1);
    CHECK(elapsed >= 40ms);
    CHECK(elapsed < 1000ms);
    // The auto-reset event was taken once.
    REQUIRE(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout);

    // Already signaled: the first of its two places answers.
    b->Set();
    auto [result2, index2] = WaitAny(first_twice, 3, alertable, 5000ms);
    REQUIRE(result2 == WaitResult::kSuccess);
    REQUIRE(index2 == 0);
    REQUIRE(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout);

    // Only the same handle twice.
    WaitHandle* same[] = {a.get(), a.get()};
    std::thread signaler2([&] {
      std::this_thread::sleep_for(50ms);
      a->Set();
    });
    auto [result3, index3] = WaitAny(same, 2, alertable, 5000ms);
    signaler2.join();
    REQUIRE(result3 == WaitResult::kSuccess);
    REQUIRE(index3 == 0);
  }
}

TEST_CASE("rex::thread::WaitAll - a handle listed twice is taken once",
          "[thread][wait][duplicate]") {
  HangGuard guard(30s);

  for (bool alertable : {false, true}) {
    INFO("alertable " << alertable);

    // A semaphore with one count: taking it twice would underflow it.
    auto sem = Semaphore::Create(1, 4);
    WaitHandle* twice[] = {sem.get(), sem.get()};
    REQUIRE(WaitAll(twice, 2, alertable, 5000ms) == WaitResult::kSuccess);
    int previous = -1;
    REQUIRE(sem->Release(1, &previous));
    REQUIRE(previous == 0);

    // Two different events, one of them listed twice: both are taken, once each.
    auto a = Event::CreateAutoResetEvent(true);
    auto b = Event::CreateAutoResetEvent(true);
    WaitHandle* mixed[] = {a.get(), b.get(), a.get()};
    REQUIRE(WaitAll(mixed, 3, alertable, 5000ms) == WaitResult::kSuccess);
    REQUIRE(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kTimeout);
    REQUIRE(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout);
  }
}
#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
