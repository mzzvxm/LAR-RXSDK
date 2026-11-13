/**
 * @file        wait_alertable_test.cpp
 * @brief       Unit tests for alertable waits: they block, and a queued user callback wakes them
 *
 * An alertable wait used to run in 1 ms slices to notice a callback queued by another thread. These
 * tests check that an idle one does not wake up at all (counted as the voluntary context switches
 * of the thread, one per time it slept and was woken) and that a queued callback and a signal still
 * end it at once.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/platform.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <thread>

#if REX_PLATFORM_LINUX
#include <sys/resource.h>
#endif

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::SleepResult;
using rex::thread::Thread;
using rex::thread::WaitAny;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

namespace {

// Waits until the thread object of a std::thread is known, i.e. the thread has called
// Thread::GetCurrentThread.
Thread* WaitForThreadObject(const std::atomic<Thread*>& slot) {
  for (int i = 0; i < 5000 && !slot.load(); ++i) {
    std::this_thread::sleep_for(1ms);
  }
  return slot.load();
}

}  // namespace

TEST_CASE("alertable waits - a queued user callback wakes a wait on one handle", "[thread][alert]") {
  auto never = Event::CreateAutoResetEvent(false);
  std::atomic<Thread*> slot{nullptr};
  std::atomic<bool> ran{false};
  WaitResult result = WaitResult::kFailed;
  std::chrono::steady_clock::duration elapsed{};

  std::thread waiter([&] {
    slot = Thread::GetCurrentThread();
    auto start = std::chrono::steady_clock::now();
    result = rex::thread::Wait(never.get(), true, 5000ms);
    elapsed = std::chrono::steady_clock::now() - start;
  });
  Thread* thread = WaitForThreadObject(slot);
  REQUIRE(thread != nullptr);
  std::this_thread::sleep_for(100ms);
  thread->QueueUserCallback([&] { ran = true; });
  waiter.join();

  CHECK(result == WaitResult::kUserCallback);
  CHECK(ran.load());
  CHECK(elapsed >= 90ms);
  CHECK(elapsed < 1000ms);
}

TEST_CASE("alertable waits - a queued user callback wakes an alertable sleep", "[thread][alert]") {
  std::atomic<Thread*> slot{nullptr};
  std::atomic<bool> ran{false};
  SleepResult result = SleepResult::kSuccess;
  std::chrono::steady_clock::duration elapsed{};

  std::thread sleeper([&] {
    slot = Thread::GetCurrentThread();
    auto start = std::chrono::steady_clock::now();
    result = rex::thread::AlertableSleep(5000ms);
    elapsed = std::chrono::steady_clock::now() - start;
  });
  Thread* thread = WaitForThreadObject(slot);
  REQUIRE(thread != nullptr);
  std::this_thread::sleep_for(100ms);
  thread->QueueUserCallback([&] { ran = true; });
  sleeper.join();

  CHECK(result == SleepResult::kAlerted);
  CHECK(ran.load());
  CHECK(elapsed >= 90ms);
  CHECK(elapsed < 1000ms);
}

TEST_CASE("alertable waits - an alertable sleep lasts its time", "[thread][alert]") {
  auto start = std::chrono::steady_clock::now();
  auto result = rex::thread::AlertableSleep(150ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(result == SleepResult::kSuccess);
  CHECK(elapsed >= 145ms);
  CHECK(elapsed < 1000ms);
}

TEST_CASE("alertable waits - a signal wakes a wait on one handle", "[thread][alert]") {
  auto event = Event::CreateAutoResetEvent(false);
  std::thread signaler([&] {
    std::this_thread::sleep_for(60ms);
    event->Set();
  });
  auto start = std::chrono::steady_clock::now();
  auto result = rex::thread::Wait(event.get(), true, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  signaler.join();

  CHECK(result == WaitResult::kSuccess);
  CHECK(elapsed >= 50ms);
  CHECK(elapsed < 1000ms);
}

TEST_CASE("alertable waits - a callback queued before the wait is run by it", "[thread][alert]") {
  auto never = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {never.get()};
  Thread* self = Thread::GetCurrentThread();
  bool ran = false;
  std::thread queuer([&] { self->QueueUserCallback([&] { ran = true; }); });
  queuer.join();

  // Every kind of alertable wait finds the queued callback at once, whatever it waits on.
  CHECK(rex::thread::Wait(never.get(), true, 5000ms) == WaitResult::kUserCallback);
  CHECK(ran);
  CHECK(rex::thread::Wait(never.get(), true, 20ms) == WaitResult::kTimeout);

  ran = false;
  std::thread queuer2([&] { self->QueueUserCallback([&] { ran = true; }); });
  queuer2.join();
  CHECK(WaitAny(handles, 1, true, 5000ms).first == WaitResult::kUserCallback);
  CHECK(ran);
}

TEST_CASE("alertable waits - a wait with a timeout shorter than a millisecond ends", "[thread][alert]") {
  auto never = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {never.get(), never.get()};
  auto start = std::chrono::steady_clock::now();
  CHECK(rex::thread::Wait(never.get(), true, 0ms) == WaitResult::kTimeout);
  CHECK(rex::thread::Wait(never.get(), true, 1ms) == WaitResult::kTimeout);
  CHECK(WaitAny(handles, 2, true, 1ms).first == WaitResult::kTimeout);
  CHECK(rex::thread::AlertableSleep(std::chrono::microseconds(300)) == SleepResult::kSuccess);
  CHECK(std::chrono::steady_clock::now() - start < 500ms);
}

TEST_CASE("alertable waits - queued callbacks all reach a thread that waits in every way",
          "[thread][alert]") {
  // The thread waits without a timeout worth waiting for. A wakeup that got lost would cost the
  // whole 5 s of the wait, so a few hundred callbacks must be through well inside 3 s.
  constexpr int kCallbacks = 400;
  auto never_a = Event::CreateAutoResetEvent(false);
  auto never_b = Event::CreateAutoResetEvent(false);
  std::atomic<Thread*> slot{nullptr};
  std::atomic<int> ran{0};
  std::atomic<int> lost{0};  // Catch assertions are not thread-safe: count here, check after join

  std::thread worker([&] {
    slot = Thread::GetCurrentThread();
    WaitHandle* two[] = {never_a.get(), never_b.get()};
    WaitHandle* one[] = {never_a.get()};
    for (int kind = 0; ran.load() < kCallbacks; ++kind) {
      bool alerted = false;
      switch (kind % 4) {
        case 0:
          alerted = rex::thread::Wait(never_a.get(), true, 5000ms) == WaitResult::kUserCallback;
          break;
        case 1:
          alerted = WaitAny(two, 2, true, 5000ms).first == WaitResult::kUserCallback;
          break;
        case 2:
          alerted = rex::thread::AlertableSleep(5000ms) == SleepResult::kAlerted;
          break;
        default:
          alerted = WaitAny(one, 1, true, 5000ms).first == WaitResult::kUserCallback;
          break;
      }
      if (!alerted) {
        ++lost;
      }
    }
  });
  Thread* thread = WaitForThreadObject(slot);
  REQUIRE(thread != nullptr);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kCallbacks; ++i) {
    thread->QueueUserCallback([&] { ++ran; });
    // Some gaps long enough for the worker to be asleep again, some back to back.
    if (i % 3 != 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(i % 2 ? 100 : 700));
    }
  }
  worker.join();
  auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(lost.load() == 0);
  CHECK(ran.load() == kCallbacks);
  CHECK(elapsed < 3s);
}

#if REX_PLATFORM_LINUX
namespace {
// How many times the calling thread has gone to sleep and been woken so far (voluntary context
// switches): a wait that polls in 1 ms slices adds about a thousand a second.
long ThreadWakeups() {
  rusage usage{};
  getrusage(RUSAGE_THREAD, &usage);
  return usage.ru_nvcsw;
}
}  // namespace

TEST_CASE("alertable waits - an idle alertable wait on one handle does not wake up", "[thread][alert]") {
  auto never = Event::CreateAutoResetEvent(false);
  const long before = ThreadWakeups();
  auto result = rex::thread::Wait(never.get(), true, 300ms);
  const long wakeups = ThreadWakeups() - before;
  CHECK(result == WaitResult::kTimeout);
  INFO(wakeups << " wake-ups in a 300 ms alertable wait");
  CHECK(wakeups < 10);  // about 300 in 1 ms slices
}

TEST_CASE("alertable waits - an idle alertable sleep does not wake up", "[thread][alert]") {
  const long before = ThreadWakeups();
  auto result = rex::thread::AlertableSleep(300ms);
  const long wakeups = ThreadWakeups() - before;
  CHECK(result == SleepResult::kSuccess);
  INFO(wakeups << " wake-ups in a 300 ms alertable sleep");
  CHECK(wakeups < 10);  // about 300 in 1 ms slices
}

TEST_CASE("alertable waits - an idle alertable wait on several handles wakes up rarely",
          "[thread][alert]") {
  // The wait keeps its 20 ms safety cap, so it may wake about fifty times a second.
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};
  const long before = ThreadWakeups();
  auto result = WaitAny(handles, 2, true, 300ms);
  const long wakeups = ThreadWakeups() - before;
  CHECK(result.first == WaitResult::kTimeout);
  INFO(wakeups << " wake-ups in a 300 ms alertable wait on two handles");
  CHECK(wakeups < 40);  // about 300 in 1 ms slices
}
#endif  // REX_PLATFORM_LINUX
