/**
 * @file        kernel/kernel_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::kernel {

class REX_KERNEL_API KernelSettings final : public SettingOwner {
 public:
  REX_SETTING(bool, headless, false, "System dialogs answer at once without showing");
  REX_SETTING(u32, user_language, 1, "XLanguage ID, 0 follows the host language", kLockedInGame);
  REX_SETTING(u32, user_country, 103, "Console country code", kLockedInGame);
  REX_SETTING(u32, license_mask, 0, "Content license mask", kLockedInGame);
  REX_SETTING(std::string, user_name, "",
              "Profile name, the gamertag the title sees and the name of the save folder. Empty "
              "asks for one on first launch");
  REX_SETTING(std::string, command_line, "", "Arguments the title sees after default.xex",
              kLockedInGame);
  REX_SETTING(bool, count_critical_section_spins, false,
              "Count the critical section enters that spin (a spin count above 0) and how many of "
              "them take the section while spinning, and log the totals when the title ends, "
              "'critical section spins: ...'. For finding out whether a title ever reaches the "
              "spin loop. Off: nothing is counted and the enter path is unchanged",
              kDebugOnly);

  KernelSettings();
};

}  // namespace rex::kernel
