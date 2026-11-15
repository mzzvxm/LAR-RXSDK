/**
 * @file        exact_e0_view_test.cpp
 * @brief       Unit tests for mapping a file view 4 KB into the file (the guest's 0xE0000000 range)
 *
 * The 0xE0000000 range of guest memory shows the physical pages from 0x1000 on, so its view of the
 * backing file starts 4 KB in. Windows maps views at multiples of 64 KB; with exact_e0_view the
 * runtime maps this one through a placeholder (MapViewOfFile3) so that a guest address is simply
 * base + address and generated code needs no +0x1000. These tests map two views of one file the way
 * the runtime does (the raw physical view at offset 0, the window at offset 0x1000), write through
 * one and read back through the other.
 *
 * They are skipped where a page-aligned file offset cannot be mapped: Windows before 10 version
 * 1803, and a host whose pages are larger than 4 KB (macOS arm64).
 *
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include <rex/chrono/clock.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;

constexpr size_t kPageBytes = 0x1000;
// The window, like 0x20000000 bytes in the arena.
constexpr size_t kViewBytes = 0x100000;
// The raw view reaches 4 KB further, as the arena's physical view does.
constexpr size_t kPhysicalViewBytes = kViewBytes + kPageBytes;
// A multiple of 64 KB, so both view bases are legal.
constexpr size_t kViewSpacing = 0x200000;

// A file mapping of its own, so the test does not touch the runtime's. Like the runtime's it is
// reserve-only (SEC_RESERVE): every view commits what it uses.
struct TestMapping {
  explicit TestMapping(size_t length) {
    std::string name =
        "rex_exact_e0_view_test_" + std::to_string(rex::chrono::Clock::QueryHostTickCount());
#if REX_PLATFORM_LINUX
    name = "/" + name;
#endif
    path = name;
    handle = rex::memory::CreateFileMappingHandle(path, length, PageAccess::kReadWrite, false);
  }
  ~TestMapping() {
    if (handle != rex::memory::kFileMappingHandleInvalid) {
      rex::memory::CloseFileMappingHandle(handle, path);
    }
  }
  std::filesystem::path path;
  rex::memory::FileMappingHandle handle = rex::memory::kFileMappingHandleInvalid;
};

size_t MappingBytes() {
  // Room for the window at offset 0x1000 plus the 64 KB rounding of the arena's mapping.
  const size_t granularity = rex::memory::allocation_granularity();
  const size_t wanted = kPhysicalViewBytes + granularity;
  return (wanted + granularity - 1) / granularity * granularity;
}

// A 64 KB-aligned address range that is free right now: reserve it and let it go again, the way
// Memory::Initialize tries base addresses.
uint8_t* FreeRegion(size_t length) {
  void* region = rex::memory::AllocFixed(nullptr, length, AllocationType::kReserve,
                                         PageAccess::kNoAccess);
  REQUIRE(region != nullptr);
  REQUIRE(rex::memory::DeallocFixed(region, length, DeallocationType::kRelease));
  return static_cast<uint8_t*>(region);
}

bool Supported() {
  return rex::memory::MapFileViewTakesPageOffsets() && rex::memory::page_size() == kPageBytes;
}

uint64_t Read64(const uint8_t* address) {
  uint64_t value;
  std::memcpy(&value, address, sizeof(value));
  return value;
}

void Write64(uint8_t* address, uint64_t value) {
  std::memcpy(address, &value, sizeof(value));
}

// Both views the way Memory::Initialize makes them: mapped, then committed.
struct TwoViews {
  TwoViews(TestMapping& mapping, uint8_t* base) {
    physical = base;
    window = base + kViewSpacing;
    REQUIRE(rex::memory::MapFileView(mapping.handle, physical, kPhysicalViewBytes,
                                     PageAccess::kReadWrite, 0) == physical);
    REQUIRE(rex::memory::MapFileView(mapping.handle, window, kViewBytes, PageAccess::kReadWrite,
                                     kPageBytes) == window);
    REQUIRE(rex::memory::AllocFixed(physical, kPhysicalViewBytes, AllocationType::kCommit,
                                    PageAccess::kReadWrite) == physical);
    REQUIRE(rex::memory::AllocFixed(window, kViewBytes, AllocationType::kCommit,
                                    PageAccess::kReadWrite) == window);
  }
  uint8_t* physical = nullptr;
  uint8_t* window = nullptr;
};

TEST_CASE("A view mapped 4 KB into the file is the raw view shifted by 4 KB", "[memory][e0view]") {
  if (!Supported()) {
    SKIP("this host cannot map a view at a 4 KB file offset");
  }
  TestMapping mapping(MappingBytes());
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  uint8_t* base = FreeRegion(kViewSpacing * 2);
  TwoViews views(mapping, base);

  // Through the window, read through the raw view 4 KB in: the first bytes, the middle, the last.
  Write64(views.window, 0x1122334455667788ull);
  Write64(views.window + 0x2340, 0x99AABBCCDDEEFF00ull);
  Write64(views.window + kViewBytes - 8, 0x0123456789ABCDEFull);
  CHECK(Read64(views.physical + kPageBytes) == 0x1122334455667788ull);
  CHECK(Read64(views.physical + kPageBytes + 0x2340) == 0x99AABBCCDDEEFF00ull);
  CHECK(Read64(views.physical + kPageBytes + kViewBytes - 8) == 0x0123456789ABCDEFull);

  // Through the raw view, read through the window 4 KB back.
  Write64(views.physical + 0x5000, 0xCAFEF00DDEADBEEFull);
  CHECK(Read64(views.window + 0x4000) == 0xCAFEF00DDEADBEEFull);

  // The page the window skips is not in it: a write there leaves the window's first page alone.
  Write64(views.window + 0x10, 0x2222222222222222ull);
  Write64(views.physical + 0x10, 0x1111111111111111ull);
  CHECK(Read64(views.physical + 0x10) == 0x1111111111111111ull);
  CHECK(Read64(views.window + 0x10) == 0x2222222222222222ull);
  CHECK(Read64(views.physical + kPageBytes + 0x10) == 0x2222222222222222ull);

  REQUIRE(rex::memory::UnmapFileView(mapping.handle, views.window, kViewBytes));
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, views.physical, kPhysicalViewBytes));
}

TEST_CASE("A view 4 KB into the file can be unmapped and mapped again", "[memory][e0view]") {
  if (!Supported()) {
    SKIP("this host cannot map a view at a 4 KB file offset");
  }
  TestMapping mapping(MappingBytes());
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  uint8_t* base = FreeRegion(kViewSpacing * 2);
  TwoViews views(mapping, base);

  Write64(views.window + 0x800, 0xA5A5A5A55A5A5A5Aull);
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, views.window, kViewBytes));

  // The address range is free again (no placeholder is left behind) and the memory is the file's.
  uint8_t* again = static_cast<uint8_t*>(rex::memory::MapFileView(
      mapping.handle, views.window, kViewBytes, PageAccess::kReadWrite, kPageBytes));
  REQUIRE(again == views.window);
  REQUIRE(rex::memory::AllocFixed(again, kViewBytes, AllocationType::kCommit,
                                  PageAccess::kReadWrite) == again);
  CHECK(Read64(again + 0x800) == 0xA5A5A5A55A5A5A5Aull);
  CHECK(Read64(views.physical + kPageBytes + 0x800) == 0xA5A5A5A55A5A5A5Aull);

  REQUIRE(rex::memory::UnmapFileView(mapping.handle, again, kViewBytes));
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, views.physical, kPhysicalViewBytes));
}

TEST_CASE("A file offset that is not page-aligned is refused", "[memory][e0view]") {
  if (!Supported()) {
    SKIP("this host cannot map a view at a 4 KB file offset");
  }
  TestMapping mapping(MappingBytes());
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  uint8_t* base = FreeRegion(kViewSpacing);
  CHECK(rex::memory::MapFileView(mapping.handle, base, kViewBytes, PageAccess::kReadWrite,
                                 kPageBytes / 2) == nullptr);
}

#if REX_PLATFORM_WIN32
TEST_CASE("A refused view 4 KB into the file leaves its address range free", "[memory][e0view]") {
  if (!Supported()) {
    SKIP("this host cannot map a view at a 4 KB file offset");
  }
  TestMapping mapping(MappingBytes());
  REQUIRE(mapping.handle != rex::memory::kFileMappingHandleInvalid);
  uint8_t* base = FreeRegion(kViewSpacing);

  // The range is taken: the placeholder cannot be made, as MapViewOfFileEx cannot map there either.
  REQUIRE(rex::memory::MapFileView(mapping.handle, base, kViewBytes, PageAccess::kReadWrite, 0) ==
          base);
  CHECK(rex::memory::MapFileView(mapping.handle, base, kViewBytes, PageAccess::kReadWrite,
                                 kPageBytes) == nullptr);
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, base, kViewBytes));

  // Nothing was left over: the same range takes the view now.
  CHECK(rex::memory::MapFileView(mapping.handle, base, kViewBytes, PageAccess::kReadWrite,
                                 kPageBytes) == base);
  REQUIRE(rex::memory::UnmapFileView(mapping.handle, base, kViewBytes));
}
#endif  // REX_PLATFORM_WIN32

TEST_CASE("GuestPtr and Memory::TranslateVirtual give the same host address", "[memory][e0view]") {
  // PhysicalHostOffset follows what Memory::Initialize mapped (the shared test memory uses the
  // rounded layout on Windows), so the two ways of reaching a guest address must agree.
  auto& memory = rex::testing::GetTestMemory();
  uint8_t* membase = memory.virtual_membase();
  const uint32_t addresses[] = {0x00010000, 0x40000000, 0x82000000, 0x90000000, 0xA0001000,
                                0xC0001000, 0xDFFFFFFC, 0xE0000000, 0xE0001000, 0xF0000000,
                                0xFFCFFFFC};
  for (uint32_t address : addresses) {
    INFO("guest address 0x" << std::hex << address);
    CHECK(rex::memory::GuestPtr(membase, address) == memory.TranslateVirtual(address));
  }
  CHECK(rex::memory::detail::PhysicalHostOffset(0xE0000000u) == memory.e0_host_offset());
  CHECK(rex::memory::detail::PhysicalHostOffset(0xDFFFFFFFu) == 0u);
}

}  // namespace
#endif  // REX_PLATFORM_WIN32 || REX_PLATFORM_LINUX
