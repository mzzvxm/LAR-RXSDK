/**
 * @file        content_manager_test.cpp
 * @brief       Unit tests for the kept copy of a saved game that is being rewritten.
 *
 * The close of a package needs a KernelState (it mounts a device), so these tests cover the
 * manager without one: the copy, and what the next start does with it.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/content_manager.h>

namespace {

namespace fs = std::filesystem;
using rex::system::XContentType;
using rex::system::xam::ContentManager;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;
constexpr uint32_t kTitleId = 0x545407F8;

class TempDirectory {
 public:
  TempDirectory() {
    static std::atomic<uint64_t> next_id{0};
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / ("rex_content_manager_" + std::to_string(suffix) + "_" +
                                         std::to_string(next_id++));
    fs::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

XCONTENT_AGGREGATE_DATA MakeData(XContentType type, std::string_view name) {
  XCONTENT_AGGREGATE_DATA data{};
  data.content_type = type;
  data.set_file_name(name);
  data.xuid = kXuid;
  data.title_id = kTitleId;
  return data;
}

// content_root/xuid/title_id/content_type/name
fs::path PackagePath(const fs::path& root, std::string_view type, std::string_view name) {
  return root / "B13EBABEBABEBABE" / "545407F8" / type / name;
}

// content_root/xuid/title_id/Backups/content_type/name
fs::path BackupPath(const fs::path& root, std::string_view type, std::string_view name) {
  return root / "B13EBABEBABEBABE" / "545407F8" / "Backups" / type / name;
}

void WriteSave(const fs::path& package, std::string_view content) {
  fs::create_directories(package);
  std::ofstream file(package / "mc4.sav", std::ios::binary);
  REQUIRE(file);
  file << content;
}

std::string ReadSave(const fs::path& package) {
  std::ifstream file(package / "mc4.sav", std::ios::binary);
  REQUIRE(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("a saved game that is rewritten is kept aside until the next start puts it back",
          "[content]") {
  TempDirectory temp;
  const auto root = temp.path();
  const auto data = MakeData(XContentType::kSavedGame, "mc4.sav");
  const auto package = PackagePath(root, "00000001", "mc4.sav");
  const auto backup = BackupPath(root, "00000001", "mc4.sav");
  WriteSave(package, "old save");

  {
    ContentManager manager(nullptr, root);
    CHECK_FALSE(manager.HasUnclosedBackups());
    manager.KeepContentBackup(kXuid, data);

    // The caller's delete has nothing left to delete; the title then writes the new save.
    CHECK_FALSE(manager.ContentExists(kXuid, data));
    CHECK(manager.HasUnclosedBackups());
    CHECK(ReadSave(backup) == "old save");
    CHECK(fs::exists(backup.string() + ".inprogress"));
    WriteSave(package, "new sa");  // the process ends here, with the new save half written
  }

  ContentManager restarted(nullptr, root);
  CHECK_FALSE(restarted.HasUnclosedBackups());
  CHECK(restarted.ContentExists(kXuid, data));
  CHECK(ReadSave(package) == "old save");
  CHECK_FALSE(fs::exists(backup));
  CHECK_FALSE(fs::exists(backup.string() + ".inprogress"));
}

TEST_CASE("a second rewrite replaces the copy of a save that was never closed", "[content]") {
  TempDirectory temp;
  const auto root = temp.path();
  const auto data = MakeData(XContentType::kSavedGame, "mc4.sav");
  const auto package = PackagePath(root, "00000001", "mc4.sav");
  const auto backup = BackupPath(root, "00000001", "mc4.sav");
  WriteSave(package, "first");

  {
    ContentManager manager(nullptr, root);
    manager.KeepContentBackup(kXuid, data);
    WriteSave(package, "second");
    manager.KeepContentBackup(kXuid, data);
    CHECK(ReadSave(backup) == "second");
    WriteSave(package, "third, cut short");
  }

  ContentManager restarted(nullptr, root);
  CHECK(ReadSave(package) == "second");
}

TEST_CASE("only saved games are kept aside, and only when they exist", "[content]") {
  TempDirectory temp;
  const auto root = temp.path();
  ContentManager manager(nullptr, root);

  const auto dlc = MakeData(XContentType::kMarketplaceContent, "pack");
  const auto dlc_package = PackagePath(root, "00000002", "pack");
  WriteSave(dlc_package, "dlc");
  manager.KeepContentBackup(kXuid, dlc);
  CHECK(fs::exists(dlc_package));

  manager.KeepContentBackup(kXuid, MakeData(XContentType::kSavedGame, "missing.sav"));
  CHECK_FALSE(manager.HasUnclosedBackups());
  CHECK_FALSE(fs::exists(root / "B13EBABEBABEBABE" / "545407F8" / "Backups"));
}

TEST_CASE("a kept copy without its marker is the leftover of a finished save", "[content]") {
  TempDirectory temp;
  const auto root = temp.path();
  const auto data = MakeData(XContentType::kSavedGame, "mc4.sav");
  const auto package = PackagePath(root, "00000001", "mc4.sav");
  const auto backup = BackupPath(root, "00000001", "mc4.sav");
  WriteSave(package, "latest");
  WriteSave(backup, "older");

  ContentManager restarted(nullptr, root);
  CHECK(ReadSave(package) == "latest");
  CHECK_FALSE(fs::exists(backup));
}
