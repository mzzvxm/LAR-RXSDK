/**
 * @file        tests/unit/gpu/shared_memory_test.cpp
 * @brief       The shared memory request fast path and the hot pages
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/setting.h>

#include "fake_title_context.h"
#include "gpu/xenos/shared_memory.h"
#include "gpu/xenos/xenos_settings.h"
#include "test_memory.h"

namespace {

using rex::test::FakeTitleContext;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
  ~Fixture() { rex::test::ReleaseTestMemory(); }
};

struct TestXenosSettings final : rex::gpu::XenosSettings {};

/**
 * A shared memory that records what is uploaded, in pages and in bytes, and how often sparse
 * memory is allocated. An upload marks its pages valid, as the backends do.
 */
class TestSharedMemory final : public rex::gpu::SharedMemory {
 public:
  TestSharedMemory(FakeTitleContext& title, const rex::gpu::XenosSettings& settings,
                   bool byte_path = false)
      : SharedMemory(title, title, settings), byte_path_(byte_path) {
    InitializeCommon();
    InitializeSparseHostGPUMemory(kHostGPUMemoryOptimalSparseAllocationLog2);
  }

  rex::u32 PageSize() const { return rex::u32(1) << GetPageSizeLog2(); }
  void Mark(rex::u32 start, rex::u32 length) { MarkRangeValid(start, length, false); }

  /** The page ranges (first page, count) of every non empty UploadRanges call. */
  std::vector<std::pair<rex::u32, rex::u32>> page_uploads;
  /** The byte ranges of every UploadByteRanges call that took them. */
  std::vector<std::pair<rex::u32, rex::u32>> byte_uploads;
  rex::u32 allocations = 0;

 protected:
  bool UploadRanges(const std::vector<std::pair<rex::u32, rex::u32>>& ranges) override {
    for (const auto& [start, count] : ranges) {
      page_uploads.emplace_back(start, count);
      MarkRangeValid(start << GetPageSizeLog2(), count << GetPageSizeLog2(), false);
    }
    return true;
  }

  bool UploadByteRanges(const std::vector<std::pair<rex::u32, rex::u32>>& ranges) override {
    if (!byte_path_) {
      return false;
    }
    byte_uploads.insert(byte_uploads.end(), ranges.begin(), ranges.end());
    return true;
  }

  bool AllocateSparseHostGPUMemoryRange(rex::u32, rex::u32) override {
    ++allocations;
    return true;
  }

 private:
  bool byte_path_;
};

constexpr rex::u32 kBase = 0x400000;

void Set(rex::Setting<rex::u32>& setting, const char* text) {
  REQUIRE(setting.SetText(rex::SettingLayer::kRuntime, text));
}

/** Writes a page as the guest does: a write fault, not an exact range. */
void Fault(FakeTitleContext& title, rex::u32 address, rex::u32 length) {
  title.listeners.at(0)->OnPhysicalWrite({address, length}, false);
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "shared memory: a request for valid pages returns early",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 length = shared.PageSize() * 2;

  // Pages the GPU wrote are valid without any host memory behind them. The fast path trusts them,
  // the full path allocates.
  shared.RangeWrittenByGPU(kBase, length);
  REQUIRE(shared.RequestRange(kBase, length));
  CHECK(shared.allocations == 0);
  CHECK(shared.page_uploads.empty());

  REQUIRE(settings.shared_memory_request_fast_path.SetText(rex::SettingLayer::kRuntime, "false"));
  REQUIRE(shared.RequestRange(kBase, length));
  CHECK(shared.allocations == 1);
  CHECK(shared.page_uploads.empty());
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a request uploads only the pages that are not valid",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();
  const rex::u32 first_page = kBase / page;

  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.page_uploads.size() == 1);
  CHECK(shared.page_uploads[0] == std::make_pair(first_page, rex::u32(1)));

  // Three pages, the first valid already: the other two go up, as one range.
  REQUIRE(shared.RequestRange(kBase, page * 3));
  REQUIRE(shared.page_uploads.size() == 2);
  CHECK(shared.page_uploads[1] == std::make_pair(first_page + 1, rex::u32(2)));

  // Two ranges that are valid, and one whose last byte is on a page that is not.
  const std::pair<rex::u32, rex::u32> ranges[] = {
      {kBase, page}, {kBase + page, page}, {kBase + page * 3, page + 1}};
  REQUIRE(shared.RequestRanges(ranges, 3));
  REQUIRE(shared.page_uploads.size() == 3);
  CHECK(shared.page_uploads[2] == std::make_pair(first_page + 3, rex::u32(2)));

  // Everything is valid now.
  REQUIRE(shared.RequestRanges(ranges, 3));
  CHECK(shared.page_uploads.size() == 3);
}

REX_TEST_CASE_METHOD(Fixture,
                     "shared memory: the fast path leaves empty and bad ranges to the full path",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  CHECK(shared.RequestRanges(nullptr, 0));
  CHECK(shared.RequestRange(kBase, 0));
  CHECK_FALSE(shared.RequestRange(rex::gpu::SharedMemory::kBufferSize - page, page * 2));
  CHECK_FALSE(shared.RequestRange(rex::gpu::SharedMemory::kBufferSize + 1, 1));
  CHECK(shared.page_uploads.empty());

  // A zero length range beside a valid one changes nothing.
  REQUIRE(shared.RequestRange(kBase, page));
  const std::pair<rex::u32, rex::u32> ranges[] = {{kBase + page * 8, 0}, {kBase, page}};
  CHECK(shared.RequestRanges(ranges, 2));
  CHECK(shared.page_uploads.size() == 1);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a CPU write makes the pages upload again",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page * 2));
  REQUIRE(shared.page_uploads.size() == 1);
  title.listeners.at(0)->OnPhysicalWrite({kBase + page, page}, true);
  REQUIRE(shared.RequestRange(kBase, page * 2));
  REQUIRE(shared.page_uploads.size() == 2);
  CHECK(shared.page_uploads[1] == std::make_pair(kBase / page + 1, rex::u32(1)));
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: hot pages are off by default",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  for (int i = 0; i < 20; ++i) {
    Fault(title, kBase, page);
    REQUIRE(shared.RequestRange(kBase, page));
  }
  // Every fault invalidated the page and the next request uploaded it once, nothing more.
  CHECK(shared.page_uploads.size() == 21);
  REQUIRE(shared.RequestRange(kBase, page));
  CHECK(shared.page_uploads.size() == 21);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a page that keeps faulting turns hot",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "3");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();
  const rex::u32 first_page = kBase / page;

  // Two faults are under the threshold: the page is tracked as usual.
  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);
  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);
  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.page_uploads.size() == 3);
  REQUIRE(shared.RequestRange(kBase, page));
  CHECK(shared.page_uploads.size() == 3);
  const size_t watched_before = title.GetWatchedRanges().size();

  // The third fault makes it hot: every request uploads it, and nothing watches it.
  Fault(title, kBase, page);
  for (size_t i = 0; i < 3; ++i) {
    REQUIRE(shared.RequestRange(kBase, page));
    REQUIRE(shared.page_uploads.size() == 4 + i);
    CHECK(shared.page_uploads.back() == std::make_pair(first_page, rex::u32(1)));
  }
  CHECK(title.GetWatchedRanges().size() == watched_before);

  // A page next to it that does not fault stays tracked.
  REQUIRE(shared.RequestRange(kBase + page, page));
  REQUIRE(shared.RequestRange(kBase + page, page));
  CHECK(shared.page_uploads.size() == 7);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: only guest write faults heat a page",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "2");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  for (int i = 0; i < 6; ++i) {
    // The GPU side and explicit requests invalidate an exact range.
    title.listeners.at(0)->OnPhysicalWrite({kBase, page}, true);
    REQUIRE(shared.RequestRange(kBase, page));
  }
  const size_t uploads = shared.page_uploads.size();
  REQUIRE(shared.RequestRange(kBase, page));
  CHECK(shared.page_uploads.size() == uploads);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a hot page cools down", "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "1");
  Set(settings.shared_memory_hot_page_ms, "200");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);
  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.page_uploads.size() == 3);

  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  // Cool again: the next request uploads it once more, and marks it valid.
  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.page_uploads.size() == 4);
  REQUIRE(shared.RequestRange(kBase, page));
  CHECK(shared.page_uploads.size() == 4);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a hot page is not uploaded when the GPU wrote it",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "1");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  // A page of the GPU's own data, then a fault on the page beside it, which makes both hot (the
  // fault is not exact, so it widens to the page it is on only: the GPU data stays valid).
  shared.RangeWrittenByGPU(kBase, page);
  REQUIRE(shared.RequestRange(kBase + page, page));
  Fault(title, kBase + page, page);
  const size_t uploads = shared.page_uploads.size();

  const std::pair<rex::u32, rex::u32> ranges[] = {{kBase, page * 2}};
  REQUIRE(shared.RequestRanges(ranges, 1));
  // Only the hot page goes up; the GPU's page is valid and guest memory has nothing for it.
  REQUIRE(shared.page_uploads.size() == uploads + 1);
  CHECK(shared.page_uploads.back() == std::make_pair((kBase + page) / page, rex::u32(1)));
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a backend with a byte path gets only the bytes used",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "1");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings, true);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);
  const size_t page_uploads = shared.page_uploads.size();

  // From 0x44 for 0x30 bytes: widened to 64 byte boundaries, 0x40 to 0x80.
  REQUIRE(shared.RequestRange(kBase + 0x44, 0x30));
  REQUIRE(shared.byte_uploads.size() == 1);
  CHECK(shared.byte_uploads[0] == std::make_pair(kBase + 0x40, rex::u32(0x40)));
  CHECK(shared.page_uploads.size() == page_uploads);

  // Two ranges in the page are two byte ranges, and a range that spills out of the hot page is cut
  // at the page's end.
  const std::pair<rex::u32, rex::u32> ranges[] = {{kBase + 0x100, 0x10},
                                                  {kBase + page - 0x20, 0x100}};
  REQUIRE(shared.RequestRanges(ranges, 2));
  REQUIRE(shared.byte_uploads.size() == 3);
  CHECK(shared.byte_uploads[1] == std::make_pair(kBase + 0x100, rex::u32(0x40)));
  CHECK(shared.byte_uploads[2] == std::make_pair(kBase + page - 0x40, rex::u32(0x40)));
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: a hot page is not marked valid or watched",
                     "[gpu][shared_memory]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "1");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);

  // Marking a range with a hot page in the middle watches the runs around it.
  const size_t watched_before = title.GetWatchedRanges().size();
  shared.Mark(kBase - page, page * 3);
  const std::vector<rex::PhysicalRange> watched = title.GetWatchedRanges();
  REQUIRE(watched.size() == watched_before + 2);
  CHECK(watched[watched_before].address == kBase - page);
  CHECK(watched[watched_before].length == page);
  CHECK(watched[watched_before + 1].address == kBase + page);
  CHECK(watched[watched_before + 1].length == page);
}

namespace {

struct WatchCounter {
  int fired = 0;
  static void Callback(void* context, void*, rex::u64, bool) {
    ++static_cast<WatchCounter*>(context)->fired;
  }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "shared memory: a page that a texture watches is not hot",
                     "[gpu][shared_memory]") {
  // The counter outlives the shared memory, which fires the watches left at its end.
  WatchCounter counter;
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  Set(settings.shared_memory_hot_page_faults, "1");
  Set(settings.shared_memory_hot_page_ms, "60000");
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  Fault(title, kBase, page);
  REQUIRE(shared.RequestRange(kBase, page));
  const size_t uploads = shared.page_uploads.size();

  // Watching the page cools it. Protecting it waits for the lock to be released.
  const size_t watched_before = title.GetWatchedRanges().size();
  {
    const rex::ScopeGuard lock = shared.AcquireLock();
    REQUIRE(
        shared.WatchMemoryRange(lock, kBase, page, WatchCounter::Callback, &counter, nullptr, 0));
  }
  CHECK(title.GetWatchedRanges().size() == watched_before);
  shared.ApplyPendingHotPageWatches();
  const std::vector<rex::PhysicalRange> watched = title.GetWatchedRanges();
  REQUIRE(watched.size() == watched_before + 1);
  CHECK(watched.back().address == kBase);
  CHECK(watched.back().length == page);
  // Nothing is left to apply.
  shared.ApplyPendingHotPageWatches();
  CHECK(title.GetWatchedRanges().size() == watched_before + 1);

  // The page is not hot: the next request uploads it once and marks it valid.
  REQUIRE(shared.RequestRange(kBase, page));
  REQUIRE(shared.page_uploads.size() == uploads + 1);
  REQUIRE(shared.RequestRange(kBase, page));
  CHECK(shared.page_uploads.size() == uploads + 1);

  // A write now faults and fires the watch.
  CHECK(counter.fired == 0);
  Fault(title, kBase, page);
  CHECK(counter.fired == 1);
}

REX_TEST_CASE_METHOD(Fixture, "shared memory: watching pages that are not hot protects nothing",
                     "[gpu][shared_memory]") {
  WatchCounter counter;
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  TestSharedMemory shared(title, settings);
  const rex::u32 page = shared.PageSize();

  REQUIRE(shared.RequestRange(kBase, page));
  const size_t watched_before = title.GetWatchedRanges().size();
  {
    const rex::ScopeGuard lock = shared.AcquireLock();
    REQUIRE(
        shared.WatchMemoryRange(lock, kBase, page, WatchCounter::Callback, &counter, nullptr, 0));
  }
  shared.ApplyPendingHotPageWatches();
  CHECK(title.GetWatchedRanges().size() == watched_before);
}
