/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <format>

#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include <rex/filesystem/stfs.h>
#include <rex/kernel/types/content.h>
#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <rex/kernel/module.h>
#include <rex/string/text.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/device.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object.h>

#include "xam/content_manager.h"
#include <rex/kernel/types/xex.h>
#include <rex/xam/title_lifecycle.h>
#include <rex/filesystem/file_system_factory.h>

namespace rex::xam {

ContentPackage::ContentPackage(filesystem::IFileSystem& file_system, u32 device_index,
                               const std::string_view root_name,
                               const kernel::XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : file_system_(file_system), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = std::format("\\Device\\Content\\{0}\\", device_index);
  content_data_ = data;

  auto* fs = &file_system_;
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(package_path, ec);
  filesystem::MountEntry entry;
  entry.path = device_path_;
  entry.type = folder ? filesystem::ProviderType::kHostDirectory : filesystem::ProviderType::kSTFS;
  entry.source = package_path;
  entry.flags = folder ? filesystem::MountFlags::kWritable : filesystem::MountFlags::kNone;
  if (auto mounted = fs->Mount(entry); !mounted) {
    REXSYS_WARN("content package '{}' is not mounted, {}", rex::string::PathToUTF8(package_path),
                mounted.error().message);
  }
  if (auto linked = fs->Link(root_name_ + ":", device_path_); !linked) {
    REXSYS_WARN("content root '{}:' is not linked, {}", root_name_, linked.error().message);
  }
}

ContentPackage::~ContentPackage() {
  log::NoThrow(log::system, [this] {
    auto* fs = &file_system_;
    if (auto unlinked = fs->Unlink(root_name_ + ":"); !unlinked) {
      REXSYS_DEBUG("{}", unlinked.error().message);
    }
    if (auto unmounted = fs->Unmount(device_path_); !unmounted) {
      REXSYS_DEBUG("{}", unmounted.error().message);
    }
  });
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path& header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  std::ifstream file(header_path, std::ios::binary);
  if (!file) {
    return;
  }

  std::error_code ec;
  auto file_size = std::filesystem::file_size(header_path, ec);
  if (ec || file_size < sizeof(kernel::XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    return;
  }

  file.seekg(sizeof(kernel::XCONTENT_AGGREGATE_DATA));
  file.read(reinterpret_cast<char*>(&license_), sizeof(license_));
}

namespace {

constexpr std::string_view kGameUserContentDirName = "profile";

constexpr std::string_view kGameContentBackupDirName = "Backups";

constexpr std::string_view kBackupMarkerSuffix = ".inprogress";

// Records the directory name the content root is currently keyed under, so a later profile rename
// knows what to move.
constexpr std::string_view kProfileDirMarkerName = ".profile";

// The profile's XUID, as 16 hex digits, inside the profile's content directory.
constexpr std::string_view kProfileXuidFileName = "profile.xuid";

// Package path:  content_root/xuid/title_id/content_type/name
// Backup path:   content_root/xuid/title_id/Backups/content_type/name
std::filesystem::path BackupPathFor(const std::filesystem::path& package_path) {
  const std::filesystem::path type_dir = package_path.parent_path();
  if (package_path.filename().empty() || type_dir.filename().empty()) {
    return {};
  }
  return type_dir.parent_path() / kGameContentBackupDirName / type_dir.filename() /
         package_path.filename();
}

// An empty file beside a kept package: while it is there, the package is of a save that is not
// complete.
std::filesystem::path BackupMarkerFor(std::filesystem::path backup_path) {
  backup_path += kBackupMarkerSuffix;
  return backup_path;
}

bool WriteBackupMarker(const std::filesystem::path& marker) {
  return platform::WriteHostFile(marker, std::string_view{}).has_value();
}

// The marker goes first: without it nothing is put back.
void RemoveBackup(const std::filesystem::path& backup_path) {
  std::error_code marker_ec;
  std::filesystem::remove(BackupMarkerFor(backup_path), marker_ec);
  std::error_code ec;
  std::filesystem::remove_all(backup_path, ec);
  if (marker_ec || ec) {
    REXSYS_WARN("content: could not remove the kept copy '{}': {}",
                rex::string::PathToUTF8(backup_path), (marker_ec ? marker_ec : ec).message());
  }
}

// The folders above a package that hold a title are named with 8 hex digits.
bool IsHexName(const std::filesystem::path& name, size_t digits) {
  const auto& text = name.native();
  return text.size() == digits && std::all_of(text.begin(), text.end(), [](auto c) {
           return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
         });
}

// The title may name a package in a different case than the folder it finds on disk.
bool IsSamePackage(const std::filesystem::path& left, const std::filesystem::path& right) {
  return rex::string::utf8_equal_case(rex::string::PathToUTF8(left),
                                      rex::string::PathToUTF8(right));
}

std::vector<std::filesystem::path> Subdirectories(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> result;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    std::error_code type_ec;
    if (entry.is_directory(type_ec)) {
      result.push_back(entry.path());
    }
  }
  return result;
}

// A kept package with its marker is of a save that never reached its close: the process ended (a
// crash, a kill) while the title was writing it, so the package is whatever that left, and the kept
// one goes back in its place. One without a marker is the leftover of a save that did finish (its
// removal failed): it is not the latest, and is deleted.
void RestoreInterruptedSave(const std::filesystem::path& backup_path,
                            const std::filesystem::path& package_path) {
  const std::string name = rex::string::PathToUTF8(package_path.filename());
  std::error_code ec;
  if (!std::filesystem::exists(BackupMarkerFor(backup_path), ec)) {
    std::filesystem::remove_all(backup_path, ec);
    return;
  }
  std::filesystem::remove_all(package_path, ec);
  if (!ec) {
    std::filesystem::create_directories(package_path.parent_path(), ec);
  }
  if (!ec) {
    std::filesystem::rename(backup_path, package_path, ec);
  }
  if (ec) {
    REXSYS_ERROR("content: could not put back saved game '{}' from '{}': {}", name,
                 rex::string::PathToUTF8(backup_path), ec.message());
    return;
  }
  std::filesystem::remove(BackupMarkerFor(backup_path), ec);
  REXSYS_WARN(
      "content: saved game '{}' was being rewritten when the game ended; its previous contents are "
      "back",
      name);
}

}  // namespace

ContentManager::ContentManager(const XAMTitle& title, rex::xam::UserProfile& user_profile)
    : title_(title), root_path_(title.user_data_root), user_profile_(user_profile) {
  if (root_path_.empty()) {
    return;
  }
  // Nothing has reached the content yet: put back the saves the last run left half written. This
  // comes before the profile directory moves, so a save is put back under the name it was kept
  // under.
  RestoreInterruptedSaves();
  MigrateProfileDirectory();
  LoadProfileXuid();
}

ContentManager::~ContentManager() = default;

std::string ContentManager::ProfileDirName(u64 xuid) const {
  // Shared content (DLC and anything explicitly filed under XUID 0) keeps the canonical all zero
  // directory, and other XUIDs are not the local profile. The legacy shared XUID still means the
  // local profile: content headers written before per profile XUIDs carry it, and the title hands
  // it back on open.
  if (xuid != 0 && (xuid == user_profile_.GetXUID() || xuid == UserProfile::kLegacyXuid)) {
    std::string name = UserProfile::SanitizeName(user_profile_.GetName());
    if (!name.empty()) {
      return name;
    }
  }
  return std::format("{:016X}", xuid);
}

void ContentManager::LoadProfileXuid() {
  const std::filesystem::path profile_dir =
      root_path_ / rex::string::to_path(ProfileDirName(user_profile_.GetXUID()));
  const std::filesystem::path xuid_path = profile_dir / kProfileXuidFileName;

  if (auto bytes = platform::ReadHostFile(xuid_path, 0, 31)) {
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    char* end = nullptr;
    const u64 xuid = std::strtoull(text.c_str(), &end, 16);
    if (end != text.c_str() && UserProfile::IsValidXuid(xuid)) {
      user_profile_.set_xuid(xuid);
      REXSYS_INFO("Profile XUID {:016X}", xuid);
      return;
    }
    REXSYS_WARN("Ignoring unreadable profile XUID file '{}'; creating a new XUID",
                rex::string::PathToUTF8(xuid_path));
  }

  const u64 xuid = UserProfile::GenerateXuid();
  user_profile_.set_xuid(xuid);
  if (auto written = platform::WriteHostFile(xuid_path, std::format("{:016X}\n", xuid)); written) {
    REXSYS_INFO("Created profile XUID {:016X}", xuid);
  } else {
    REXSYS_ERROR("Failed to save profile XUID to '{}'; it will change next launch, {}",
                 rex::string::PathToUTF8(xuid_path), written.error().message);
  }
}

void ContentManager::MigrateProfileDirectory() {
  const std::string current = ProfileDirName(user_profile_.GetXUID());
  // The pre rename layout keyed the directory by the XUID every profile then shared, not by
  // whatever XUID this profile will load.
  const std::string legacy = std::format("{:016X}", UserProfile::kLegacyXuid);

  std::error_code ec;
  const std::filesystem::path marker_path = root_path_ / kProfileDirMarkerName;
  if (auto bytes = platform::ReadHostFile(marker_path, 0, 255)) {
    previous_dir_name_.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    while (!previous_dir_name_.empty() &&
           (previous_dir_name_.back() == '\n' || previous_dir_name_.back() == '\r')) {
      previous_dir_name_.pop_back();
    }
  }

  // Without a marker the only thing that can be on disk is the pre rename layout, which used the
  // hex XUID.
  const std::string source = previous_dir_name_.empty() ? legacy : previous_dir_name_;

  if (source != current && std::filesystem::exists(root_path_ / rex::string::to_path(source), ec)) {
    if (std::filesystem::exists(root_path_ / rex::string::to_path(current), ec)) {
      REXSYS_WARN(
          "Profile content directory '{}' already exists; leaving old directory '{}' in place. "
          "Merge it manually to keep those saves.",
          current, source);
    } else {
      std::filesystem::rename(root_path_ / rex::string::to_path(source),
                              root_path_ / rex::string::to_path(current), ec);
      if (ec) {
        REXSYS_ERROR("Failed to migrate profile content '{}' -> '{}': {}", source, current,
                     ec.message());
      } else {
        REXSYS_INFO("Migrated profile content directory '{}' -> '{}'", source, current);
      }
    }
  }

  // Record the current name even when nothing moved, so the next rename knows where to look.
  if (previous_dir_name_ != current) {
    if (auto written = platform::WriteHostFile(marker_path, current); !written) {
      REXSYS_WARN("The profile directory name is not recorded, {}", written.error().message);
    }
  }
}

std::filesystem::path ContentManager::ResolvePackageRoot(u64 xuid,
                                                         rex::kernel::XContentType content_type,
                                                         u32 title_id) const {
  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }
  const std::string xuid_str = ProfileDirName(xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));

  // Layout is content_root/xuid/title_id/content_type/
  return detail::ResolveChain(root_path_, xuid_str, title_id_str, content_type_str);
}

std::filesystem::path ContentManager::ResolvePackagePath(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  if (data.content_type == rex::kernel::XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return rex::platform::ResolveEntryCase(package_root, data.GetFileName());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(
    const std::string_view file_name, u64 xuid, u32 title_id,
    rex::kernel::XContentType content_type) const {
  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }

  if (content_type == rex::kernel::XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  const std::string xuid_str = ProfileDirName(xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Layout is content_root/xuid/title_id/Headers/content_type/filename.header
  return detail::ResolveChain(root_path_, xuid_str, title_id_str, detail::kGameContentHeaderDirName,
                              content_type_str, final_name);
}

std::filesystem::path ContentManager::ResolveHeaderPathFor(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  const u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;
  return ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);
}

std::vector<kernel::XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(
    u32 device_id, u64 xuid, rex::kernel::XContentType content_type, u32 title_id) {
  std::vector<kernel::XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }

  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(package_root, ec)) {
    if (!child.is_directory(ec)) {
      continue;
    }
    const std::filesystem::path name = child.path().filename();
    if (!IsContentEnabled(title_id, content_type, rex::string::PathToUTF8(name))) {
      continue;
    }

    kernel::XCONTENT_AGGREGATE_DATA content_data{};
    if (auto header =
            ReadContentHeaderFile(rex::string::PathToUTF8(name), xuid, title_id, content_type)) {
      result.emplace_back(*header);
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.SetDisplayName(rex::string::PathToUTF16(name));
      content_data.SetFileName(rex::string::PathToUTF8(name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(content_data);
    }
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }
  if (const std::optional<std::filesystem::path> source = FindInPlaceSource(xuid, data)) {
    if (!std::filesystem::exists(*source)) {
      return nullptr;
    }
    package_path = *source;
  }
  auto package = std::make_unique<ContentPackage>(title_.file_system, ++device_count_, root_name,
                                                  data, package_path);
  return package;
}

bool ContentManager::HasContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);
  if (!IsContentEnabled(title_id, data.content_type, data.GetFileName())) {
    return false;
  }
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

Result<void> ContentManager::WriteContentHeaderFile(u64 xuid, kernel::XCONTENT_AGGREGATE_DATA data,
                                                    u32 license_mask) {
  if (data.title_id == static_cast<u32>(-1)) {
    data.title_id = this->GetTitleID();
  }
  if (data.xuid == static_cast<u64>(-1)) {
    data.xuid = xuid;
  }
  auto header_path = ResolveHeaderPathFor(xuid, data);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
  }

  std::vector<u8> header(sizeof(kernel::XCONTENT_AGGREGATE_DATA) +
                         (license_mask != 0 ? sizeof(license_mask) : 0));
  std::memcpy(header.data(), &data, sizeof(kernel::XCONTENT_AGGREGATE_DATA));
  if (license_mask != 0) {
    std::memcpy(header.data() + sizeof(kernel::XCONTENT_AGGREGATE_DATA), &license_mask,
                sizeof(license_mask));
  }
  if (!platform::WriteHostFile(header_path, header)) {
    return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  return Ok();
}

Result<kernel::XCONTENT_AGGREGATE_DATA> ContentManager::ReadContentHeaderFile(
    const std::string_view file_name, u64 xuid, u32 title_id,
    rex::kernel::XContentType content_type) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr u32 kHeaderSize = sizeof(kernel::XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  std::ifstream file(header_file_path, std::ios::binary);
  if (!file) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  std::array<u8, kHeaderSize> buffer;
  if (!file.read(reinterpret_cast<char*>(buffer.data()), kHeaderSize)) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  kernel::XCONTENT_AGGREGATE_DATA data;
  std::memcpy(&data, buffer.data(), buffer.size());
  // A header found under the local profile's directory belongs to this profile, whatever owner it
  // names: the legacy shared XUID (written before per profile XUIDs) or a XUID the profile had on
  // another PC (a front end may derive the XUID from a key kept outside the save folder). Report
  // the current one, or opening it would look under that XUID's hex directory.
  if (data.xuid != 0 && data.xuid != static_cast<u64>(-1) &&
      (xuid == user_profile_.GetXUID() || xuid == UserProfile::kLegacyXuid)) {
    data.xuid = user_profile_.GetXUID();
  }
  return data;
}

Result<void> ContentManager::CreateContent(const std::string_view root_name, u64 xuid,
                                           const kernel::XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
  }
  if (!std::filesystem::create_directories(package_path)) {
    return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  REX_ASSERT(package != nullptr);

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string::StringKeyIgnoreCase::Create(root_name), std::move(package));
  }
  return Ok();
}

Result<u32> ContentManager::OpenContent(const std::string_view root_name, u64 xuid,
                                        const kernel::XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err<u32>(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
  }

  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path) ||
      !IsContentEnabled(title_id, data.content_type, data.GetFileName())) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  if (!package) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  package->LoadPackageLicenseMask(ResolveHeaderPathFor(xuid, data));
  const u32 license = package->GetPackageLicense();

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err<u32>(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string::StringKeyIgnoreCase::Create(root_name), std::move(package));
  }
  return license;
}

Result<void> ContentManager::CloseContent(const std::string_view root_name) {
  std::string open_root_name;
  {
    std::lock_guard lock(mutex_);
    auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
    if (it == open_packages_.end()) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
    open_root_name = it->first.GetView();
  }
  CloseOpenedFilesFromContent(open_root_name);
  if (std::unique_ptr<ContentPackage> package = DetachPackage(open_root_name)) {
    const std::filesystem::path package_path = package->GetPackagePath();
    package.reset();
    // The title closed the package, which is when a saved game is complete: what KeepContentBackup
    // kept for it is no longer needed.
    DropKeptBackup(package_path);
  }
  return Ok();
}

Result<std::vector<u8>> ContentManager::GetContentThumbnail(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / detail::kThumbnailFileName;
  std::lock_guard lock(mutex_);
  if (std::filesystem::exists(thumb_path)) {
    return platform::ReadHostFile(thumb_path);
  } else {
    return Err<std::vector<u8>>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::SetContentThumbnail(u64 xuid,
                                                 const kernel::XCONTENT_AGGREGATE_DATA& data,
                                                 std::vector<u8> buffer) {
  auto package_path = ResolvePackagePath(xuid, data);
  std::lock_guard lock(mutex_);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / detail::kThumbnailFileName;
    if (!platform::WriteHostFile(thumb_path, buffer)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
    return Ok();
  } else {
    return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::DeleteContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);
  const std::filesystem::path manifest_path = ResolveManifestPath(used_xuid, data);
  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);

  {
    std::lock_guard lock(mutex_);

    if (IsContentOpen(data)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }

    std::error_code ec;
    auto dir_removed = std::filesystem::remove_all(package_path, ec);
    if (ec ||
        std::filesystem::status(package_path, ec).type() != std::filesystem::file_type::not_found) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }

    std::error_code ec2;
    bool header_removed = std::filesystem::remove(header_path, ec2);
    std::filesystem::remove(manifest_path, ec2);

    std::vector<EnableEntry>& entries = LoadEnableEntriesLocked();
    const std::string file_name = data.GetFileName();
    if (std::erase_if(entries, [&](const EnableEntry& entry) {
          return entry.title_id == title_id && entry.content_type == data.content_type &&
                 entry.file_name == file_name;
        }) != 0) {
      SaveEnableEntriesLocked();
    }

    if (dir_removed == 0 && !header_removed) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
  }
  NotifyContentChanged();
  return Ok();
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = std::format("{:08X}", this->GetTitleID());
  auto user_name = rex::string::to_path(UserProfile::SanitizeName(user_profile_.GetName()));

  // Per game per profile data location: content_root/title_id/profile/user_name
  std::filesystem::path path = root_path_ / title_id / kGameUserContentDirName / user_name;
  MigrateGameUserContentPath(path);
  return path;
}

void ContentManager::MigrateGameUserContentPath(const std::filesystem::path& current_path) {
  std::error_code ec;
  if (std::filesystem::exists(current_path, ec)) {
    return;
  }

  // Candidates, in order: the name recorded in the marker file, then the default name used before
  // profile names were selectable.
  const std::filesystem::path profile_root = current_path.parent_path();
  for (const std::string& candidate :
       {previous_dir_name_, std::string(UserProfile::kDefaultName)}) {
    if (candidate.empty()) {
      continue;
    }
    const std::filesystem::path source = profile_root / rex::string::to_path(candidate);
    if (source == current_path || !std::filesystem::exists(source, ec)) {
      continue;
    }
    std::filesystem::create_directories(profile_root, ec);
    std::filesystem::rename(source, current_path, ec);
    if (ec) {
      REXSYS_ERROR("Failed to migrate profile settings '{}' -> '{}': {}",
                   rex::string::PathToUTF8(source), rex::string::PathToUTF8(current_path),
                   ec.message());
    } else {
      REXSYS_INFO("Migrated profile settings directory '{}' -> '{}'",
                  rex::string::PathToUTF8(source), rex::string::PathToUTF8(current_path));
      return;
    }
  }
}

void ContentManager::KeepContentBackup(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  // CHECK: that a save reaches XamContentClose for the package it created, after writing it. If it
  // never closes, the copy stays and the previous save comes back at the next start (the log's
  // "is being rewritten" line without a "closed" line after it shows that).
  if (data.content_type != kernel::XContentType::kSavedGame) {
    return;
  }
  // The paths first: resolving the running title takes the object table's locks, which the leaf
  // mutex must not be held across.
  const std::filesystem::path package_path = ResolvePackagePath(xuid, data);
  const std::filesystem::path backup_path = BackupPathFor(package_path);

  std::lock_guard lock(mutex_);
  if (IsContentOpen(data)) {
    return;  // DeleteContent refuses it as well
  }
  std::error_code ec;
  if (backup_path.empty() || !std::filesystem::exists(package_path, ec)) {
    return;
  }
  const std::filesystem::path marker = BackupMarkerFor(backup_path);
  const std::string name = rex::string::PathToUTF8(package_path.filename());

  // A copy kept earlier is of a save the title never closed before asking for this one.
  RemoveBackup(backup_path);
  std::filesystem::create_directories(backup_path.parent_path(), ec);
  if (!ec && !WriteBackupMarker(marker)) {
    ec = std::make_error_code(std::errc::io_error);
  }
  if (!ec) {
    std::filesystem::rename(package_path, backup_path, ec);
  }
  if (ec) {
    // The package cannot be moved (a file of it held open by a scanner or a sync client, say): copy
    // it, and the caller deletes the original as it always did. The marker comes after the copy
    // here, so that a copy cut short is never put back over the package, which is still whole.
    RemoveBackup(backup_path);
    std::filesystem::copy(package_path, backup_path, std::filesystem::copy_options::recursive, ec);
    if (!ec && !WriteBackupMarker(marker)) {
      ec = std::make_error_code(std::errc::io_error);
    }
  }
  if (ec) {
    REXSYS_WARN("content: no copy of the saved game '{}' kept before it is rewritten: {}", name,
                ec.message());
    RemoveBackup(backup_path);
    return;
  }
  std::erase_if(kept_backups_, [&](const KeptBackup& kept) {
    return IsSamePackage(kept.package_path, package_path);
  });
  kept_backups_.push_back({package_path, backup_path});
  REXSYS_INFO(
      "content: saved game '{}' is being rewritten; its previous contents are kept until it is "
      "closed",
      name);
}

bool ContentManager::HasUnclosedBackups() const {
  std::lock_guard lock(mutex_);
  return !kept_backups_.empty();
}

void ContentManager::DropKeptBackup(const std::filesystem::path& package_path) {
  // The lock is held to the end, so that HasUnclosedBackups stays true until the marker is gone: a
  // title that waits for it must not end between the two, or the next start would put the old
  // contents back over the finished save.
  std::lock_guard lock(mutex_);
  const auto kept = std::ranges::find_if(kept_backups_, [&](const KeptBackup& candidate) {
    return IsSamePackage(candidate.package_path, package_path);
  });
  if (kept == kept_backups_.end()) {
    return;
  }
  const std::filesystem::path backup_path = kept->backup_path;
  kept_backups_.erase(kept);
  RemoveBackup(backup_path);
  REXSYS_INFO("content: saved game '{}' closed; its previous contents are no longer kept",
              rex::string::PathToUTF8(package_path.filename()));
}

void ContentManager::RestoreInterruptedSaves() {
  // content_root/xuid/title_id/Backups/content_type/name
  // The first level is whatever the profile's content directory is called: its name now, a hex XUID
  // from before profiles were named, or the name a rename is about to move away from.
  for (const std::filesystem::path& xuid_dir : Subdirectories(root_path_)) {
    for (const std::filesystem::path& title_dir : Subdirectories(xuid_dir)) {
      if (!IsHexName(title_dir.filename(), 8)) {
        continue;
      }
      for (const std::filesystem::path& type_dir :
           Subdirectories(title_dir / kGameContentBackupDirName)) {
        for (const std::filesystem::path& backup_path : Subdirectories(type_dir)) {
          RestoreInterruptedSave(backup_path,
                                 title_dir / type_dir.filename() / backup_path.filename());
        }
      }
    }
  }
}

std::unique_ptr<ContentPackage> ContentManager::DetachPackage(const std::string_view root_name) {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
  if (it == open_packages_.end()) {
    return nullptr;
  }
  std::unique_ptr<ContentPackage> package = std::move(it->second);
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  const std::optional<std::string> resolved_path =
      title_.file_system.FindLink(std::string(root_name) + ':');
  if (!resolved_path || resolved_path->empty()) {
    return;
  }

  const auto is_separator = [](char c) { return c == '\\' || c == '/'; };
  const std::string_view root = *resolved_path;
  const std::vector<kernel::Pinned<kernel::IOpenedFile>> all_files_handles =
      title_.objects.All<kernel::IOpenedFile>();
  for (const kernel::Pinned<kernel::IOpenedFile>& file : all_files_handles) {
    const std::string file_path = file->GetAbsolutePath();
    if (!rex::string::utf8_starts_with_case(file_path, root)) {
      continue;
    }
    const std::string_view rest = std::string_view(file_path).substr(root.size());
    if (rest.empty() || is_separator(rest.front()) || is_separator(root.back())) {
      if (file.GetHandle()) {
        kernel::CloseOwned(title_.objects, file.GetHandle());
      }
    }
  }
}

Result<u32> ContentManager::GetContentDeviceID(const std::string_view root_name) const {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
  if (it == open_packages_.end()) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  return it->second->GetPackageContentData().device_id;
}

u32 ContentManager::GetTitleID() const {
  auto module =
      title_.objects.Get<kernel::ILoadedModule>(title_.modules.GetExecutable().value_or(0));
  REX_ASSERT(module);

  kernel::XEXOptExecutionInfo* exec_info = 0;
  module->OptHeader(kernel::kXEXHeaderExecutionInfo, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> ContentManager::GetTitleXDBF() const {
  auto module =
      title_.objects.Get<kernel::ILoadedModule>(title_.modules.GetExecutable().value_or(0));
  REX_ASSERT(module);
  return GetModuleXDBF(*module);
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> ContentManager::GetModuleXDBF(
    const kernel::ILoadedModule& exec_module) const {
  u32 resource_data = 0;
  u32 resource_size = 0;
  if (kernel::XSUCCEEDED(exec_module.GetSection(std::format("{:08X}", exec_module.GetTitleID()),
                                                &resource_data, &resource_size))) {
    return title_.file_system_factory.OpenXDBF(title_.membase.TranslateVirtual(resource_data),
                                               resource_size);
  }
  return title_.file_system_factory.OpenXDBF(nullptr, resource_size);
}

}  // namespace rex::xam
