/**
 * @file        tests/unit/xam/user_profile_test.cpp
 * @brief       The profile name and XUID helpers the content folders and the front end rely on
 *
 * The signed in profile is named by the kernel.user_name setting. The name is the gamertag the
 * title sees and the folder the saves live in, so it is cleaned to what every host accepts as a
 * folder name, and the XUID is per profile instead of the one every profile used to share.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>

#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/xam/user_profile.h>

#include "base/app_harness.h"

using rex::SettingLayer;
using rex::test::HarnessApp;
using rex::xam::UserProfile;

REX_TEST_CASE("SanitizeName drops what a folder name cannot hold", "[xam][profile]") {
  CHECK(UserProfile::SanitizeName("Renan") == "Renan");
  CHECK(UserProfile::SanitizeName("a<b>c:d\"e/f\\g|h?i*j") == "abcdefghij");
  CHECK(UserProfile::SanitizeName(std::string("tab\there\x7F") + "end") == "tabhereend");
  CHECK(UserProfile::SanitizeName("<>:|?*").empty());
  CHECK(UserProfile::SanitizeName("").empty());
}

REX_TEST_CASE("SanitizeName trims the ends a Windows folder cannot have", "[xam][profile]") {
  CHECK(UserProfile::SanitizeName("  Player") == "Player");
  CHECK(UserProfile::SanitizeName("Player.. ") == "Player");
  CHECK(UserProfile::SanitizeName("  Player<1>. ") == "Player1");
  CHECK(UserProfile::SanitizeName("...").empty());
}

REX_TEST_CASE("SanitizeName keeps a gamertag within kMaxNameLength", "[xam][profile]") {
  static_assert(UserProfile::kMaxNameLength == 15);
  const std::string clipped = UserProfile::SanitizeName("ABCDEFGHIJKLMNOPQRST");
  CHECK(clipped == "ABCDEFGHIJKLMNO");
  CHECK(clipped.size() == UserProfile::kMaxNameLength);
}

REX_TEST_CASE("SanitizeName keeps a reserved DOS device name out of the folder names",
              "[xam][profile]") {
  CHECK(UserProfile::SanitizeName("CON") == "CON_");
  CHECK(UserProfile::SanitizeName("lpt1") == "lpt1_");
  CHECK(UserProfile::SanitizeName("COM10") == "COM10");
}

REX_TEST_CASE("A generated XUID is valid, per profile and looks like the legacy one",
              "[xam][profile]") {
  CHECK_FALSE(UserProfile::IsValidXuid(0));
  CHECK_FALSE(UserProfile::IsValidXuid(~0ull));
  // A title reads bits 54 and 55 as no permission.
  CHECK_FALSE(UserProfile::IsValidXuid(UserProfile::kLegacyXuid | 0x0040000000000000ull));
  CHECK_FALSE(UserProfile::IsValidXuid(UserProfile::kLegacyXuid | 0x0080000000000000ull));
  CHECK(UserProfile::IsValidXuid(UserProfile::kLegacyXuid));

  constexpr rex::u64 kHighBits = 0xFFFF000000000000ull;
  for (int i = 0; i < 64; ++i) {
    const rex::u64 xuid = UserProfile::GenerateXuid();
    CHECK(UserProfile::IsValidXuid(xuid));
    CHECK(xuid != UserProfile::kLegacyXuid);
    CHECK((xuid & kHighBits) == (UserProfile::kLegacyXuid & kHighBits));
  }
}

// The app loads the default modules, so the kernel module has registered kernel.user_name before
// the test body runs; the test sets it by name instead of registering a second owner.
REX_TEST_CASE("The configured name follows kernel.user_name", "[xam][profile]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REQUIRE(settings.Find("kernel.user_name"));
    // Nothing chosen yet: the front end asks for a name, and until then the default stands.
    CHECK(UserProfile::ConfiguredName() == UserProfile::kDefaultName);
    {
      UserProfile profile;
      CHECK(profile.GetName() == UserProfile::kDefaultName);
      CHECK(profile.GetXUID() == UserProfile::kLegacyXuid);  // until the content manager loads one
      profile.set_xuid(0xB13E123456789ABCull);
      CHECK(profile.GetXUID() == 0xB13E123456789ABCull);
    }

    REX_TRY_VOID(settings.SetText(SettingLayer::kRuntime, "kernel.user_name", "  Player<One>. "));
    CHECK(UserProfile::ConfiguredName() == "PlayerOne");
    UserProfile named;
    CHECK(named.GetName() == "PlayerOne");

    // A name that sanitizes to nothing falls back to the default.
    REX_TRY_VOID(settings.SetText(SettingLayer::kRuntime, "kernel.user_name", "<>?"));
    CHECK(UserProfile::ConfiguredName() == UserProfile::kDefaultName);
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}
