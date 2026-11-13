/**
 * @file        mapped_views_test.cpp
 * @brief       Unit tests for AllocFixed's commit on a reservation inside a mapped file view
 *
 * The runtime maps the guest arena as file views and then commits guest allocations on them with
 * AllocFixed(kCommit), which on Linux goes through the "mapping exists" path and wants to know the
 * range has no hole. The runtime answers that for its own views without reading
 * /proc/self/maps; these tests check that the answer stays right when a view is cut.
 *
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>

#if REX_PLATFORM_LINUX
#include <cstdint>
#include <filesystem>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include <rex/memory/utils.h>

namespace {

using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;

// A file mapping of its own, so the test does not touch the runtime's.
struct TestMapping {
  explicit TestMapping(size_t length) : path("/rex_mapped_views_test_" + std::to_string(getpid())) {
    handle = rex::memory::CreateFileMappingHandle(path, length, PageAccess::kReadWrite, true);
  }
  ~TestMapping() {
    if (handle != rex::memory::kFileMappingHandleInvalid) {
      rex::memory::CloseFileMappingHandle(handle, path);
    }
  }
  std::filesystem::path path;
  rex::memory::FileMappingHandle handle = rex::memory::kFileMappingHandleInvalid;
};

PageAccess AccessAt(void* address) {
  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(address, length, access));
  return access;
}

TEST_CASE("A commit on a mapped file view that write watches have split succeeds",
          "[memory][views]") {
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 64;
  TestMapping mapping(page * kPages);
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  void* view = rex::memory::MapFileView(mapping.handle, nullptr, page * kPages,
                                        PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  auto* bytes = static_cast<uint8_t*>(view);

  // Write watches: read-only pages scattered over the view, each its own mapping.
  for (size_t i = 1; i < kPages; i += 3) {
    REQUIRE(rex::memory::Protect(bytes + i * page, page, PageAccess::kReadOnly));
  }

  // The commit finds the range mapped and makes all of it writable.
  void* committed = rex::memory::AllocFixed(view, page * kPages, AllocationType::kCommit,
                                            PageAccess::kReadWrite);
  CHECK(committed == view);
  for (size_t i = 0; i < kPages; ++i) {
    CHECK(AccessAt(bytes + i * page) == PageAccess::kReadWrite);
  }

  // A sub-range commits too, and changes only itself.
  REQUIRE(rex::memory::Protect(bytes, page * kPages, PageAccess::kNoAccess));
  committed = rex::memory::AllocFixed(bytes + page * 8, page * 4, AllocationType::kCommit,
                                      PageAccess::kReadOnly);
  CHECK(committed == bytes + page * 8);
  CHECK(AccessAt(bytes + page * 7) == PageAccess::kNoAccess);
  CHECK(AccessAt(bytes + page * 8) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * 11) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * 12) == PageAccess::kNoAccess);

  REQUIRE(rex::memory::UnmapFileView(mapping.handle, view, page * kPages));
}

TEST_CASE("A commit across two adjacent file views succeeds", "[memory][views]") {
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 16;
  TestMapping mapping(page * kPages);
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);

  // The arena is a run of views side by side: reserve the space, then map the views into it.
  void* base = rex::memory::AllocFixed(nullptr, page * kPages * 2, AllocationType::kReserve,
                                       PageAccess::kNoAccess);
  REQUIRE(base != nullptr);
  auto* bytes = static_cast<uint8_t*>(base);
  REQUIRE(rex::memory::MapFileView(mapping.handle, bytes, page * kPages, PageAccess::kReadWrite,
                                   0) == bytes);
  REQUIRE(rex::memory::MapFileView(mapping.handle, bytes + page * kPages, page * kPages,
                                   PageAccess::kReadWrite, 0) == bytes + page * kPages);

  void* committed = rex::memory::AllocFixed(bytes + page * (kPages - 2), page * 4,
                                            AllocationType::kCommit, PageAccess::kReadOnly);
  CHECK(committed == bytes + page * (kPages - 2));
  CHECK(AccessAt(bytes + page * (kPages - 1)) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * kPages) == PageAccess::kReadOnly);

  REQUIRE(rex::memory::UnmapFileView(mapping.handle, bytes, page * kPages));
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, bytes + page * kPages, page * kPages));
}

TEST_CASE("A commit over a hole cut into a mapped file view is refused", "[memory][views]") {
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 32;
  TestMapping mapping(page * kPages);
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  void* view = rex::memory::MapFileView(mapping.handle, nullptr, page * kPages,
                                        PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  auto* bytes = static_cast<uint8_t*>(view);

  // Warm path: the whole view commits (and so the view is known to the runtime as mapped).
  REQUIRE(rex::memory::AllocFixed(view, page * kPages, AllocationType::kCommit,
                                  PageAccess::kReadOnly) == view);

  SECTION("a hole made by UnmapFileView") {
    REQUIRE(rex::memory::UnmapFileView(mapping.handle, bytes + page * 10, page * 2));
  }
  SECTION("a hole made by DeallocFixed") {
    REQUIRE(rex::memory::DeallocFixed(bytes + page * 10, page * 2, DeallocationType::kRelease));
  }

  // The range over the hole is refused, and nothing in it was changed.
  void* refused = rex::memory::AllocFixed(view, page * kPages, AllocationType::kCommit,
                                          PageAccess::kReadWrite);
  CHECK(refused == nullptr);
  CHECK(AccessAt(bytes) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * 9) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * 12) == PageAccess::kReadOnly);
  CHECK(AccessAt(bytes + page * 31) == PageAccess::kReadOnly);

  // Either side of the hole still commits.
  CHECK(rex::memory::AllocFixed(view, page * 10, AllocationType::kCommit, PageAccess::kReadWrite) ==
        view);
  CHECK(rex::memory::AllocFixed(bytes + page * 12, page * 20, AllocationType::kCommit,
                                PageAccess::kReadWrite) == bytes + page * 12);
  CHECK(AccessAt(bytes + page * 9) == PageAccess::kReadWrite);
  CHECK(AccessAt(bytes + page * 31) == PageAccess::kReadWrite);

  // A range that only touches the hole's edge is refused as well.
  CHECK(rex::memory::AllocFixed(bytes + page * 9, page * 2, AllocationType::kCommit,
                                PageAccess::kReadOnly) == nullptr);

  REQUIRE(rex::memory::UnmapFileView(mapping.handle, view, page * 10));
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, bytes + page * 12, page * 20));
}

TEST_CASE("A view that is unmapped and mapped again is known again", "[memory][views]") {
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 8;
  TestMapping mapping(page * kPages);
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);

  void* view = rex::memory::MapFileView(mapping.handle, nullptr, page * kPages,
                                        PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, view, page * kPages));

  // The address space is free: a new view at the same address is a new mapping.
  void* again = rex::memory::MapFileView(mapping.handle, view, page * kPages,
                                         PageAccess::kReadWrite, 0);
  REQUIRE(again == view);
  CHECK(rex::memory::AllocFixed(view, page * kPages, AllocationType::kCommit,
                                PageAccess::kReadOnly) == view);
  CHECK(AccessAt(static_cast<uint8_t*>(view) + page * 3) == PageAccess::kReadOnly);
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, view, page * kPages));

  // The address space is free again. A stale entry would call the whole range mapped when only
  // the first two pages are (a reservation, not a view): the commit has to be refused instead of
  // changing the two pages that exist.
  void* reserved =
      rex::memory::AllocFixed(view, page * 2, AllocationType::kReserve, PageAccess::kNoAccess);
  REQUIRE(reserved == view);
  CHECK(rex::memory::AllocFixed(view, page * kPages, AllocationType::kCommit,
                                PageAccess::kReadWrite) == nullptr);
  CHECK(AccessAt(view) == PageAccess::kNoAccess);
  REQUIRE(rex::memory::DeallocFixed(view, page * 2, DeallocationType::kRelease));
}

}  // namespace
#endif  // REX_PLATFORM_LINUX
