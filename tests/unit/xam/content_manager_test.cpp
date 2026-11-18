/**
 * @file        tests/unit/xam/content_manager_test.cpp
 * @brief       Unit tests for the kept copy of a saved game that is being rewritten.
 *
 * A saved game that a title rewrites from nothing is moved to Backups/<content type>/<name>
 * beside its package, with a <name>.inprogress marker, until the title closes the new package.
 * What an interrupted save leaves is put back when the next content manager is made, which the
 * tests that need a restart arrange by preparing the content folder before the runtime starts.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "rex_test.h"

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/content.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>

#include "kernel/kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

namespace fs = std::filesystem;
using rex::kernel::XCONTENT_AGGREGATE_DATA;
using rex::kernel::XContentType;

constexpr rex::u32 kTitleId = 0x545407F8;
constexpr std::string_view kTitleDir = "545407F8";
constexpr std::string_view kSavedGameDir = "00000001";
constexpr std::string_view kMarketplaceDir = "00000002";
constexpr std::string_view kSaveName = "mc4.sav";

XCONTENT_AGGREGATE_DATA MakeData(XContentType type, std::string_view name, rex::u64 xuid) {
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = type;
  data.SetFileName(name);
  data.xuid = xuid;
  data.title_id = kTitleId;
  return data;
}

// content_root/<profile dir>/title_id/content_type/name
fs::path PackagePath(const fs::path& root, std::string_view profile_dir, std::string_view type,
                     std::string_view name) {
  return root / profile_dir / kTitleDir / type / name;
}

// content_root/<profile dir>/title_id/Backups/content_type/name
fs::path BackupPath(const fs::path& root, std::string_view profile_dir, std::string_view type,
                    std::string_view name) {
  return root / profile_dir / kTitleDir / "Backups" / type / name;
}

fs::path MarkerOf(const fs::path& backup) {
  return fs::path(backup.string() + ".inprogress");
}

// The directory the content of every profile was filed under before profiles were named. A content
// manager made over it moves it to the name of the profile after it has put the saves back.
std::string LegacyProfileDir() {
  return std::format("{:016X}", rex::xam::UserProfile::kLegacyXuid);
}

void WriteSave(const fs::path& package, std::string_view content) {
  fs::create_directories(package);
  std::ofstream file(package / kSaveName, std::ios::binary);
  REQUIRE(file);
  file << content;
}

std::string ReadSave(const fs::path& package) {
  std::ifstream file(package / kSaveName, std::ios::binary);
  REQUIRE(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteMarker(const fs::path& backup) {
  std::ofstream marker(MarkerOf(backup), std::ios::binary);
  REQUIRE(marker);
}

}  // namespace

REX_TEST_CASE("a saved game that is rewritten is kept aside until the title closes the new one",
              "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const rex::u64 xuid = c.GetRuntime().GetUserProfile().GetXUID();
    const auto data = MakeData(XContentType::kSavedGame, kSaveName, xuid);
    const std::string profile = manager.ProfileDirName(xuid);
    const auto package = PackagePath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    const auto backup = BackupPath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    WriteSave(package, "old save");

    CHECK_FALSE(manager.HasUnclosedBackups());
    manager.KeepContentBackup(xuid, data);

    // The caller's delete has nothing left to delete; the title then writes the new save.
    CHECK_FALSE(manager.HasContent(xuid, data));
    CHECK_FALSE(fs::exists(package));
    CHECK(manager.HasUnclosedBackups());
    CHECK(ReadSave(backup) == "old save");
    CHECK(fs::exists(MarkerOf(backup)));

    // The title creates the package again, writes it and closes it: that completes the save.
    REQUIRE(manager.CreateContent("save", xuid, data));
    WriteSave(package, "new save");
    CHECK(manager.HasUnclosedBackups());
    REQUIRE(manager.CloseContent("save"));

    CHECK_FALSE(manager.HasUnclosedBackups());
    CHECK(ReadSave(package) == "new save");
    CHECK_FALSE(fs::exists(backup));
    CHECK_FALSE(fs::exists(MarkerOf(backup)));
  });
}

REX_TEST_CASE("a second rewrite replaces the copy of a save that was never closed",
              "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const rex::u64 xuid = c.GetRuntime().GetUserProfile().GetXUID();
    const auto data = MakeData(XContentType::kSavedGame, kSaveName, xuid);
    const std::string profile = manager.ProfileDirName(xuid);
    const auto package = PackagePath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    const auto backup = BackupPath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    WriteSave(package, "first");

    manager.KeepContentBackup(xuid, data);
    WriteSave(package, "second");
    manager.KeepContentBackup(xuid, data);
    CHECK(manager.HasUnclosedBackups());
    CHECK(ReadSave(backup) == "second");
    CHECK(fs::exists(MarkerOf(backup)));
  });
}

REX_TEST_CASE("only saved games are kept aside, and only when they exist", "[xam][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const rex::u64 xuid = c.GetRuntime().GetUserProfile().GetXUID();
    const std::string profile = manager.ProfileDirName(xuid);

    // Shared content (DLC) is filed under XUID 0 and is never moved.
    const auto dlc = MakeData(XContentType::kMarketplaceContent, "pack", 0);
    const auto dlc_package =
        PackagePath(fixture.GetRoot(), manager.ProfileDirName(0), kMarketplaceDir, "pack");
    WriteSave(dlc_package, "dlc");
    manager.KeepContentBackup(0, dlc);
    CHECK(fs::exists(dlc_package));

    manager.KeepContentBackup(xuid, MakeData(XContentType::kSavedGame, "missing.sav", xuid));
    CHECK_FALSE(manager.HasUnclosedBackups());
    CHECK_FALSE(fs::exists(fixture.GetRoot() / profile / kTitleDir / "Backups"));
    CHECK_FALSE(fs::exists(fixture.GetRoot() / manager.ProfileDirName(0) / kTitleDir / "Backups"));
  });
}

REX_TEST_CASE("a save that was cut short is put back at the next start", "[xam][content]") {
  // The state a process leaves when it ends while the title writes a rewritten save: the kept copy
  // and its marker, and a package that is half written. The content folder is the pre rename
  // layout, so this also checks that the save is put back before the folder is renamed.
  KernelFixture fixture([](const fs::path& root) {
    const std::string legacy = LegacyProfileDir();
    WriteSave(PackagePath(root, legacy, kSavedGameDir, kSaveName), "new sa");
    const auto backup = BackupPath(root, legacy, kSavedGameDir, kSaveName);
    WriteSave(backup, "old save");
    WriteMarker(backup);
  });
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const rex::u64 xuid = c.GetRuntime().GetUserProfile().GetXUID();
    const std::string profile = manager.ProfileDirName(xuid);
    const auto data = MakeData(XContentType::kSavedGame, kSaveName, xuid);
    const auto package = PackagePath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    const auto backup = BackupPath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);

    CHECK_FALSE(manager.HasUnclosedBackups());
    CHECK(manager.HasContent(xuid, data));
    CHECK(ReadSave(package) == "old save");
    CHECK_FALSE(fs::exists(backup));
    CHECK_FALSE(fs::exists(MarkerOf(backup)));
    CHECK_FALSE(fs::exists(fixture.GetRoot() / LegacyProfileDir()));
  });
}

REX_TEST_CASE("a kept copy without its marker is the leftover of a finished save",
              "[xam][content]") {
  KernelFixture fixture([](const fs::path& root) {
    const std::string legacy = LegacyProfileDir();
    WriteSave(PackagePath(root, legacy, kSavedGameDir, kSaveName), "latest");
    WriteSave(BackupPath(root, legacy, kSavedGameDir, kSaveName), "older");
  });
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    auto& manager = c.GetRuntime().GetContentManager();
    const rex::u64 xuid = c.GetRuntime().GetUserProfile().GetXUID();
    const std::string profile = manager.ProfileDirName(xuid);
    const auto package = PackagePath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);
    const auto backup = BackupPath(fixture.GetRoot(), profile, kSavedGameDir, kSaveName);

    CHECK(ReadSave(package) == "latest");
    CHECK_FALSE(fs::exists(backup));
    CHECK_FALSE(manager.HasUnclosedBackups());
  });
}
