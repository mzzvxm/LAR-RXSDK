/**
 * @file        tests/unit/base/file_mapping_test.cpp
 * @brief       Named file mappings are exclusive to their creator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>
#include <format>
#include <random>
#include <string>

#include <rex/memory/virtual_memory.h>
#include <rex/platform.h>

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using rex::memory::CloseFileMappingHandle;
using rex::memory::CreateFileMappingHandle;
using rex::memory::kFileMappingHandleInvalid;
using rex::memory::PageAccess;

#if REX_PLATFORM_WIN32

// POSIX drops the name as soon as the object exists, so only Windows keeps it taken.
REX_TEST_CASE("Creating a file mapping under a name that exists fails", "[base][file_mapping]") {
  const std::filesystem::path name =
      std::format("rex_file_mapping_test_{:08x}", static_cast<rex::u32>(std::random_device{}()));
  const auto first = CreateFileMappingHandle(name, 0x10000, PageAccess::kReadWrite, false);
  REQUIRE(first != kFileMappingHandleInvalid);

  const auto second = CreateFileMappingHandle(name, 0x10000, PageAccess::kReadWrite, false);
  CHECK(second == kFileMappingHandleInvalid);
  if (second != kFileMappingHandleInvalid) {
    CloseFileMappingHandle(second, name);
  }
  CloseFileMappingHandle(first, name);
}

#endif

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID

namespace {

rex::u32 ReapedProcessID() {
  const pid_t child = fork();
  if (child == 0) {
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  return static_cast<rex::u32>(child);
}

bool SharedMemoryExists(const std::string& name) {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    return false;
  }
  close(fd);
  return true;
}

/** Leaves a named object in /dev/shm, as a run that was killed before it closed its mapping did. */
bool LeaveSharedMemory(const std::string& name) {
  const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return false;
  }
  close(fd);
  return true;
}

}  // namespace

REX_TEST_CASE("A file mapping's name is gone from /dev/shm as soon as it is created",
              "[base][file_mapping]") {
  const std::filesystem::path name = rex::memory::CreateFileMappingName("rex_unlink_test_");
  const auto handle = CreateFileMappingHandle(name, 0x10000, PageAccess::kReadWrite, false);
  REQUIRE(handle != kFileMappingHandleInvalid);

  CHECK_FALSE(SharedMemoryExists("/" + name.string()));

  // The descriptor keeps the object, and views map from it.
  void* view = rex::memory::MapFileView(handle, nullptr, 0x10000, PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  *static_cast<volatile rex::u8*>(view) = 0x5A;
  CHECK(*static_cast<volatile rex::u8*>(view) == 0x5A);
  CHECK(rex::memory::UnmapFileView(handle, view, 0x10000));

  CloseFileMappingHandle(handle, name);
}

REX_TEST_CASE("Creating a file mapping name puts the process ID after the prefix",
              "[base][file_mapping]") {
  const std::string name = rex::memory::CreateFileMappingName("rex_name_test_").string();
  CHECK(name.starts_with(std::format("rex_name_test_{}_", static_cast<int>(getpid()))));
  CHECK(name != rex::memory::CreateFileMappingName("rex_name_test_").string());
}

REX_TEST_CASE("Removing stale file mappings unlinks names of dead processes and keeps live ones",
              "[base][file_mapping]") {
  const std::string prefix =
      std::format("rex_stale_test_{:08x}_", static_cast<rex::u32>(std::random_device{}()));
  const std::filesystem::path dead_name = std::format("{}{}_dead", prefix, ReapedProcessID());
  const std::filesystem::path live_name = rex::memory::CreateFileMappingName(prefix);
  const std::filesystem::path other_name = std::format("rex_other_test_{}_x", ReapedProcessID());

  // CreateFileMappingHandle drops the name at once, so leave the objects the way an older run did.
  REQUIRE(LeaveSharedMemory("/" + dead_name.string()));
  REQUIRE(LeaveSharedMemory("/" + live_name.string()));
  REQUIRE(LeaveSharedMemory("/" + other_name.string()));

  rex::memory::RemoveStaleFileMappings(prefix);

  CHECK_FALSE(SharedMemoryExists("/" + dead_name.string()));
  CHECK(SharedMemoryExists("/" + live_name.string()));
  CHECK(SharedMemoryExists("/" + other_name.string()));

  shm_unlink(("/" + dead_name.string()).c_str());
  shm_unlink(("/" + live_name.string()).c_str());
  shm_unlink(("/" + other_name.string()).c_str());
}

#endif
