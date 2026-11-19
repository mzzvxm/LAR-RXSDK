/**
 * @file        tests/unit/test_title_settings.cpp
 * @brief       Every test case starts with the title and runtime settings at their defaults
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>

#include <rex/setting.h>

#include "kernel/settings_access.h"
#include "rex_test.h"

namespace {

/**
 * The title paths and the runtime settings are objects of the process, not of an app: a value a
 * test sets stays when its app ends. ToolApp::Open sets game_root, entry and imports and nothing
 * puts them back, so a later test that launches a title found an empty entry, or a game folder
 * that was already deleted. CTest runs each case in its own process and never saw it, a run of the
 * whole binary does.
 */
class TitleSettingsListener : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseStarting(const Catch::TestCaseInfo&) override {
    ResetOwner(rex::kernel::GetTitlePaths());
    ResetOwner(rex::kernel::GetRuntimeSettings());
  }

 private:
  static void ResetOwner(rex::SettingOwner& owner) {
    for (rex::SettingBase* setting : owner.GetOwnedSettings()) {
      [[maybe_unused]] auto reset = setting->Reset();
    }
  }
};

const auto kListenerRegistrar =
    rex::test::RegisterListener<TitleSettingsListener>("TitleSettingsListener"_catch_sr);

}  // namespace
