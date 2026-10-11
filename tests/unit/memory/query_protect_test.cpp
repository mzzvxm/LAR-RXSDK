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

}  // namespace
#endif
