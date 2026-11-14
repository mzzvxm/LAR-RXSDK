/**
 * @file        query_protect_test.cpp
 * @brief       Unit tests for memory::QueryProtect / memory::Protect
 *
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/platform.h>
#include <rex/memory/utils.h>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <sys/mman.h>

namespace {

using rex::memory::PageAccess;

TEST_CASE("QueryProtect reports the protection of a mapping", "[memory][protect]") {
  const size_t page = rex::memory::page_size();
  void* base = mmap(nullptr, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(base != MAP_FAILED);
  auto* bytes = static_cast<uint8_t*>(base);

  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(bytes, length, access));
  CHECK(access == PageAccess::kReadWrite);
  CHECK(length >= page * 4);

  // Splitting the mapping: the middle pages become read-only.
  PageAccess old_access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::Protect(bytes + page, page * 2, PageAccess::kReadOnly, &old_access));
  CHECK(old_access == PageAccess::kReadWrite);

  length = 0;
  REQUIRE(rex::memory::QueryProtect(bytes + page, length, access));
  CHECK(access == PageAccess::kReadOnly);
  CHECK(length == page * 2);

  // The length counts from the queried address, not from the start of the mapping.
  length = 0;
  REQUIRE(rex::memory::QueryProtect(bytes + page * 2, length, access));
  CHECK(access == PageAccess::kReadOnly);
  CHECK(length == page);

  REQUIRE(rex::memory::Protect(bytes + page, page, PageAccess::kNoAccess));
  REQUIRE(rex::memory::QueryProtect(bytes + page, length, access));
  CHECK(access == PageAccess::kNoAccess);

  REQUIRE(rex::memory::QueryProtect(bytes + page * 3, length, access));
  CHECK(access == PageAccess::kReadWrite);
  CHECK(length >= page);  // may continue into a neighbouring mapping with the same protection

  munmap(base, page * 4);
}

TEST_CASE("QueryProtect fails for an unmapped address", "[memory][protect]") {
  const size_t page = rex::memory::page_size();
  void* base = mmap(nullptr, page * 2, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(base != MAP_FAILED);
  REQUIRE(munmap(base, page * 2) == 0);

  size_t length = 123;
  PageAccess access = PageAccess::kReadWrite;
  CHECK_FALSE(rex::memory::QueryProtect(base, length, access));
  CHECK(access == PageAccess::kNoAccess);
  CHECK(length == 0);
}

TEST_CASE("QueryProtect follows protections set page by page", "[memory][protect]") {
  // Write watches split guest memory into many mappings: every lookup has to find the right one.
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 256;
  void* base =
      mmap(nullptr, page * kPages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(base != MAP_FAILED);
  auto* bytes = static_cast<uint8_t*>(base);
  auto expected = [](size_t i) {
    return i % 7 == 0 ? PageAccess::kNoAccess
                      : (i % 3 == 0 ? PageAccess::kReadOnly : PageAccess::kReadWrite);
  };

  for (size_t i = 0; i < kPages; ++i) {
    REQUIRE(rex::memory::Protect(bytes + i * page, page, expected(i)));
  }
  for (size_t i = 0; i < kPages; ++i) {
    size_t length = 0;
    PageAccess access = PageAccess::kExecuteReadWrite;
    REQUIRE(rex::memory::QueryProtect(bytes + i * page, length, access));
    CHECK(access == expected(i));
    CHECK(length >= page);
    // The old protection that Protect reports is the one that was set.
    PageAccess old_access = PageAccess::kExecuteReadWrite;
    REQUIRE(rex::memory::Protect(bytes + i * page, page, PageAccess::kReadWrite, &old_access));
    CHECK(old_access == expected(i));
    REQUIRE(rex::memory::QueryProtect(bytes + i * page, length, access));
    CHECK(access == PageAccess::kReadWrite);
  }

  munmap(base, page * kPages);
}

#if REX_PLATFORM_LINUX
TEST_CASE("AllocFixed commits on a reservation that is split into several mappings",
          "[memory][protect]") {
  const size_t page = rex::memory::page_size();
  constexpr size_t kPages = 8;
  void* base = rex::memory::AllocFixed(nullptr, page * kPages, rex::memory::AllocationType::kReserve,
                                       PageAccess::kNoAccess);
  REQUIRE(base != nullptr);
  auto* bytes = static_cast<uint8_t*>(base);

  // Split the reservation as write watches would.
  REQUIRE(rex::memory::Protect(bytes + page * 2, page, PageAccess::kReadOnly));
  REQUIRE(rex::memory::Protect(bytes + page * 5, page, PageAccess::kReadWrite));

  // The commit goes through the EEXIST path and needs the whole range to be mapped.
  void* committed = rex::memory::AllocFixed(base, page * kPages, rex::memory::AllocationType::kCommit,
                                            PageAccess::kReadWrite);
  CHECK(committed == base);
  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(bytes + page * 2, length, access));
  CHECK(access == PageAccess::kReadWrite);

  // A hole in the range: the commit is refused and nothing is changed.
  REQUIRE(rex::memory::Protect(bytes, page * kPages, PageAccess::kReadOnly));
  REQUIRE(munmap(bytes + page * 3, page) == 0);
  void* refused = rex::memory::AllocFixed(base, page * kPages, rex::memory::AllocationType::kCommit,
                                          PageAccess::kReadWrite);
  CHECK(refused == nullptr);
  REQUIRE(rex::memory::QueryProtect(bytes, length, access));
  CHECK(access == PageAccess::kReadOnly);
  CHECK(length == page * 3);

  munmap(bytes, page * 3);
  munmap(bytes + page * 4, page * (kPages - 4));
}
#endif  // REX_PLATFORM_LINUX

}  // namespace
#endif
