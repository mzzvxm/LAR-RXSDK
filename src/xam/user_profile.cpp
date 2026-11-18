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
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

#include <format>

#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>

namespace rex::xam {

namespace {

// The setting that names the profile (the gamertag, and the name of the save folder), declared in
// KernelSettings. Empty means not chosen yet, and the front end asks for one.
constexpr std::string_view kUserNameSetting = "kernel.user_name";

std::string ReadUserNameSetting() {
  auto found = REX_SETTINGS().Find(kUserNameSetting);
  if (!found || (*found)->GetType() != SettingType::kString) {
    return {};
  }
  return static_cast<Setting<std::string>*>(*found)->Get();
}

}  // namespace

std::string UserProfile::SanitizeName(const std::string_view raw) {
  std::string name;
  name.reserve(kMaxNameLength);
  for (char c : raw) {
    if (name.size() >= kMaxNameLength) {
      break;
    }
    const auto uc = static_cast<unsigned char>(c);
    // Strip anything that cannot live in a path component on any host, plus control characters the
    // title would render as garbage.
    if (uc < 0x20 || uc == 0x7F) {
      continue;
    }
    if (std::strchr("<>:\"/\\|?*", c) != nullptr) {
      continue;
    }
    name.push_back(c);
  }

  // Trailing dots and spaces are not addressable on Windows.
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }
  while (!name.empty() && name.front() == ' ') {
    name.erase(name.begin());
  }

  // Reserved DOS device names cannot be directory names on Windows.
  static const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  for (const char* reserved : kReserved) {
    if (upper == reserved) {
      name.push_back('_');
      break;
    }
  }

  return name;
}

u64 UserProfile::GenerateXuid() {
  std::random_device rd;
  u64 xuid;
  do {
    const u64 bits = (static_cast<u64>(rd()) << 32) | rd();
    xuid = (kLegacyXuid & 0xFFFF000000000000ull) | (bits & 0x0000FFFFFFFFFFFFull);
  } while (xuid == kLegacyXuid || !IsValidXuid(xuid));
  return xuid;
}

bool UserProfile::IsValidXuid(u64 xuid) {
  return xuid != 0 && xuid != ~0ull && (xuid & 0x00C0000000000000ull) == 0;
}

std::string UserProfile::ConfiguredName() {
  auto name = SanitizeName(ReadUserNameSetting());
  return name.empty() ? std::string(kDefaultName) : name;
}

UserProfile::UserProfile() {
  // A title rejects any XUID with bits 54 or 55 set (mask 0x00C0000000000000).
  //
  // This is a placeholder until the content manager loads, or creates, the profile's own XUID,
  // which lives in the profile's content directory and so follows renames.
  xuid_ = kLegacyXuid;
  name_ = ConfiguredName();

  // XPROFILE_GAMER_YAXIS_INVERSION
  AddSetting(std::make_unique<Int32Setting>(0x10040002, 0));
  // XPROFILE_OPTION_CONTROLLER_VIBRATION
  AddSetting(std::make_unique<Int32Setting>(0x10040003, 3));
  // XPROFILE_GAMERCARD_ZONE
  AddSetting(std::make_unique<Int32Setting>(0x10040004, 0));
  // XPROFILE_GAMERCARD_REGION
  AddSetting(std::make_unique<Int32Setting>(0x10040005, 0));
  // XPROFILE_GAMERCARD_CRED
  AddSetting(std::make_unique<Int32Setting>(0x10040006, 0xFA));
  // XPROFILE_GAMERCARD_REP
  AddSetting(std::make_unique<FloatSetting>(0x5004000B, 0.0f));
  // XPROFILE_OPTION_VOICE_MUTED
  AddSetting(std::make_unique<Int32Setting>(0x1004000C, 0));
  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  AddSetting(std::make_unique<Int32Setting>(0x1004000D, 0));
  // XPROFILE_OPTION_VOICE_VOLUME
  AddSetting(std::make_unique<Int32Setting>(0x1004000E, 0x64));
  // XPROFILE_GAMERCARD_MOTTO
  AddSetting(std::make_unique<UnicodeSetting>(0x402C0011, u""));
  // XPROFILE_GAMERCARD_TITLES_PLAYED
  AddSetting(std::make_unique<Int32Setting>(0x10040012, 1));
  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040013, 0));
  // XPROFILE_GAMER_DIFFICULTY
  AddSetting(std::make_unique<Int32Setting>(0x10040015, 0));
  // XPROFILE_GAMER_CONTROL_SENSITIVITY
  AddSetting(std::make_unique<Int32Setting>(0x10040018, 0));
  // Preferred color 1
  AddSetting(std::make_unique<Int32Setting>(0x1004001D, 0xFFFF0000u));
  // Preferred color 2
  AddSetting(std::make_unique<Int32Setting>(0x1004001E, 0xFF00FF00u));
  // XPROFILE_GAMER_ACTION_AUTO_AIM
  AddSetting(std::make_unique<Int32Setting>(0x10040022, 1));
  // XPROFILE_GAMER_ACTION_AUTO_CENTER
  AddSetting(std::make_unique<Int32Setting>(0x10040023, 0));
  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040024, 0));
  // XPROFILE_GAMER_RACE_TRANSMISSION
  AddSetting(std::make_unique<Int32Setting>(0x10040026, 0));
  // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  AddSetting(std::make_unique<Int32Setting>(0x10040027, 0));
  // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040028, 0));
  // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040029, 0));
  // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040038, 0));
  // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040039, 0));

  // XPROFILE_GAMERCARD_PICTURE_KEY
  AddSetting(std::make_unique<UnicodeSetting>(0x4064000F, u"gamercard_picture_key"));

  // XPROFILE_TITLE_SPECIFIC1
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFF));
  // XPROFILE_TITLE_SPECIFIC2
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFE));
  // XPROFILE_TITLE_SPECIFIC3
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFD));
}

void UserProfile::AddSetting(std::unique_ptr<Setting> setting) {
  Setting* previous_setting = setting.get();
  std::swap(settings_[setting->setting_id], previous_setting);

  if (setting->is_set && setting->IsTitleSpecific()) {
    SaveSetting(setting.get());
  }

  if (previous_setting) {
    for (auto vec_it = setting_list_.begin(); vec_it != setting_list_.end(); ++vec_it) {
      if (vec_it->get() == previous_setting) {
        vec_it->swap(setting);
        break;
      }
    }
  } else {
    setting_list_.push_back(std::move(setting));
  }
}

UserProfile::Setting* UserProfile::GetSetting(u32 setting_id) {
  const auto& it = settings_.find(setting_id);
  if (it == settings_.end()) {
    return nullptr;
  }
  UserProfile::Setting* setting = it->second;
  if (setting->IsTitleSpecific()) {
    if (title_id_() != setting->loaded_title_id) {
      LoadSetting(setting);
    }
  }
  return setting;
}

void UserProfile::LoadSetting(UserProfile::Setting* setting) {
  if (setting->IsTitleSpecific()) {
    auto content_dir = content_->ResolveGameUserContentPath();
    auto setting_id = std::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    if (auto serialized_data = rex::platform::ReadHostFile(file_path)) {
      setting->Deserialize(*serialized_data);
      setting->loaded_title_id = title_id_();
    }
  } else {
    REXSYS_WARN("Attempting to load unsupported profile setting from disk");
  }
}

void UserProfile::SaveSetting(UserProfile::Setting* setting) {
  if (setting->IsTitleSpecific()) {
    auto serialized_setting = setting->Serialize();
    auto content_dir = content_->ResolveGameUserContentPath();
    std::filesystem::create_directories(content_dir);
    auto setting_id = std::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    if (auto written = rex::platform::WriteHostFile(file_path, serialized_setting); !written) {
      REXSYS_WARN("profile setting is not saved, {}", written.error().message);
    }
  } else {
    REXSYS_WARN("Attempting to save unsupported profile setting to disk");
  }
}

}  // namespace rex::xam
