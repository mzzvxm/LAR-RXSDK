/**
 * @file        tests/unit/gpu/resolve_rules_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>

#include <rex/setting.h>

#include "gpu/xenos/resolve_rules.h"

using namespace rex::gpu;

namespace {

class RulesOwner final : public rex::SettingOwner {
 public:
  RulesOwner() : SettingOwner("test_resolve_rules") {}
  REX_SETTING(std::string, rules, "", "Rules");
};

// A 512x512 8_8_8_8 resolve of a custom vinyl.
ResolveDescription Vinyl() {
  ResolveDescription resolve;
  resolve.written_address = 0x051A0000;
  resolve.written_length = 1048576;
  resolve.pitch = 512;
  resolve.height = 512;
  resolve.format = 6;
  resolve.source = 0;
  return resolve;
}

ResolveRule MustParse(std::string_view text) {
  rex::Result<ResolveRule> rule = ParseResolveRule(text);
  REQUIRE(rule.has_value());
  return *rule;
}

}  // namespace

REX_TEST_CASE("resolve rule: every key of a rule must hold", "[gpu][resolve_rules]") {
  const ResolveRule rule = MustParse("len=1048576,pitch=512,height=512,fmt=6");
  CHECK(rule.Matches(Vinyl()));

  ResolveDescription other = Vinyl();
  other.pitch = 256;
  CHECK_FALSE(rule.Matches(other));
  other = Vinyl();
  other.written_length = 1048572;
  CHECK_FALSE(rule.Matches(other));
  other = Vinyl();
  other.format = 7;
  CHECK_FALSE(rule.Matches(other));
  other = Vinyl();
  other.height = 511;
  CHECK_FALSE(rule.Matches(other));
}

REX_TEST_CASE("resolve rule: a key left out matches anything", "[gpu][resolve_rules]") {
  CHECK(MustParse("").Matches(Vinyl()));
  CHECK(MustParse("fmt=6").Matches(Vinyl()));
  CHECK_FALSE(MustParse("src=4").Matches(Vinyl()));
  ResolveDescription depth = Vinyl();
  depth.source = 4;
  CHECK(MustParse("src=4").Matches(depth));
}

REX_TEST_CASE("resolve rule: ranges take a value, both ends or one", "[gpu][resolve_rules]") {
  const ResolveRule rule = MustParse(" addr = 0x051A0000 - 0x051F7FFF , pitch=500- ");
  CHECK(rule.address.set);
  CHECK(rule.address.lo == 0x051A0000);
  CHECK(rule.address.hi == 0x051F7FFF);
  CHECK(rule.pitch.lo == 500);
  CHECK(rule.pitch.hi == ~rex::u64(0));
  CHECK(rule.Matches(Vinyl()));

  CHECK(MustParse("len=-1048576").Matches(Vinyl()));
  CHECK_FALSE(MustParse("len=-1048575").Matches(Vinyl()));
  CHECK(MustParse("len=1048576").length.lo == 1048576);
  CHECK(MustParse("len=1048576").length.hi == 1048576);
  CHECK(MustParse("len=0X100000").Matches(Vinyl()));
}

REX_TEST_CASE("resolve rule: the written memory only has to touch the address range",
              "[gpu][resolve_rules]") {
  const ResolveRule rule = MustParse("addr=0x051F7000-0x051F7FFF");
  // The resolve is 0x051A0000 to 0x052A0000: it covers the range.
  CHECK(rule.Matches(Vinyl()));

  ResolveDescription before = Vinyl();
  before.written_address = 0x051E0000;
  before.written_length = 0x1000;
  CHECK_FALSE(rule.Matches(before));

  ResolveDescription overlap = before;
  overlap.written_address = 0x051F7F00;
  CHECK(rule.Matches(overlap));

  ResolveDescription after = before;
  after.written_address = 0x051F8000;
  CHECK_FALSE(rule.Matches(after));
}

REX_TEST_CASE("resolve rule: the mode is full unless the rule names another",
              "[gpu][resolve_rules]") {
  CHECK(MustParse("len=1").mode == ReadbackResolveMode::kFull);
  CHECK(MustParse("mode=full").mode == ReadbackResolveMode::kFull);
  CHECK(MustParse("len=1,mode=fast").mode == ReadbackResolveMode::kFast);
  CHECK(MustParse("mode=some").mode == ReadbackResolveMode::kSome);
}

REX_TEST_CASE("resolve rule: a rule that is not valid says why", "[gpu][resolve_rules]") {
  for (const char* text :
       {"len", "len=", "len=abc", "len=1-2-3", "addr=-", "color=1", "mode=slow", "len=0x"}) {
    CAPTURE(text);
    rex::Result<ResolveRule> rule = ParseResolveRule(text);
    REQUIRE_FALSE(rule.has_value());
    CHECK_FALSE(rule.error().message.empty());
  }
  rex::Result<ResolveRule> phase = ParseResolveRule("phase=3");
  REQUIRE_FALSE(phase.has_value());
  CHECK(phase.error().category == rex::ErrorCategory::kNotImplemented);
}

REX_TEST_CASE("resolve rules: a list leaves out the rules that do not parse",
              "[gpu][resolve_rules]") {
  const std::vector<ResolveRule> rules =
      ParseResolveRules("len=1048576,fmt=6; bogus ;;phase=2; addr=0x051A0000-", "test");
  REQUIRE(rules.size() == 2);
  CHECK(rules[0].text == "len=1048576,fmt=6");
  CHECK(rules[1].text == "addr=0x051A0000-");

  CHECK(ParseResolveRules("", "test").empty());
  CHECK(ParseResolveRules("  ;; ", "test").empty());
  CHECK(ParseResolveRules("off", "test").empty());
}

REX_TEST_CASE("resolve rule setting: the rules follow the setting", "[gpu][resolve_rules]") {
  RulesOwner owner;
  ResolveRuleSetting setting(owner.rules);
  CHECK_FALSE(setting.HasRules());
  CHECK_FALSE(setting.Match(Vinyl()).has_value());

  REQUIRE(owner.rules.SetText(rex::SettingLayer::kRuntime,
                              "len=1048576,pitch=512,height=512,fmt=6,mode=fast;fmt=37"));
  CHECK(setting.HasRules());
  const std::optional<ResolveRule> match = setting.Match(Vinyl());
  REQUIRE(match.has_value());
  CHECK(match->mode == ReadbackResolveMode::kFast);

  ResolveDescription rain = Vinyl();
  rain.format = 37;
  rain.written_length = 131072;
  const std::optional<ResolveRule> second = setting.Match(rain);
  REQUIRE(second.has_value());
  CHECK(second->text == "fmt=37");
  rain.format = 38;
  CHECK_FALSE(setting.Match(rain).has_value());

  REQUIRE(owner.rules.SetText(rex::SettingLayer::kRuntime, "off"));
  CHECK_FALSE(setting.HasRules());
  CHECK_FALSE(setting.Match(Vinyl()).has_value());

  REQUIRE(owner.rules.SetText(rex::SettingLayer::kRuntime, "bogus"));
  CHECK_FALSE(setting.HasRules());
}

REX_TEST_CASE("resolve rule setting: a value set before it is built counts",
              "[gpu][resolve_rules]") {
  RulesOwner owner;
  REQUIRE(owner.rules.SetText(rex::SettingLayer::kTitle, "fmt=6"));
  ResolveRuleSetting setting(owner.rules);
  CHECK(setting.HasRules());
  CHECK(setting.Match(Vinyl()).has_value());
}

REX_TEST_CASE("resolve readback requests: a request asks for nothing without bytes or frames",
              "[gpu][resolve_rules]") {
  ResolveReadbackRequests requests;
  CHECK_FALSE(requests.Request(0x1000, 0, 8));
  CHECK_FALSE(requests.Request(0x1000, 0x1000, 0));
  CHECK_FALSE(requests.Request(0xFFFFF000, 0x2000, 8));
  CHECK_FALSE(requests.HasRequests());
  CHECK(requests.Request(0xFFFFF000, 0x1000, 8));
  CHECK(requests.HasRequests());
}

REX_TEST_CASE("resolve readback requests: a resolve that touches the range is found",
              "[gpu][resolve_rules]") {
  ResolveReadbackRequests requests;
  CHECK_FALSE(requests.Touches(0x2000, 0x1000));
  REQUIRE(requests.Request(0x4000, 0x2000, 8));
  CHECK(requests.Touches(0x4000, 0x2000));
  CHECK(requests.Touches(0x3000, 0x1001));
  CHECK_FALSE(requests.Touches(0x3000, 0x1000));
  CHECK(requests.Touches(0x5FFF, 4));
  CHECK_FALSE(requests.Touches(0x6000, 0x1000));
  CHECK_FALSE(requests.Touches(0x4000, 0));
}

REX_TEST_CASE("resolve readback requests: a request lasts the frames it asked for",
              "[gpu][resolve_rules]") {
  ResolveReadbackRequests requests;
  REQUIRE(requests.Request(0x4000, 0x1000, 3));
  requests.EndFrame();
  requests.EndFrame();
  CHECK(requests.HasRequests());
  CHECK(requests.Touches(0x4000, 0x1000));
  requests.EndFrame();
  CHECK_FALSE(requests.HasRequests());
  CHECK_FALSE(requests.Touches(0x4000, 0x1000));
}

REX_TEST_CASE("resolve readback requests: asking again takes the longer lifetime",
              "[gpu][resolve_rules]") {
  ResolveReadbackRequests requests;
  REQUIRE(requests.Request(0x4000, 0x1000, 2));
  requests.EndFrame();
  REQUIRE(requests.Request(0x4000, 0x1000, 2));
  requests.EndFrame();
  CHECK(requests.HasRequests());
  // A shorter ask does not cut the lifetime short.
  REQUIRE(requests.Request(0x4000, 0x1000, 1));
  requests.EndFrame();
  CHECK_FALSE(requests.HasRequests());
}

REX_TEST_CASE("resolve readback requests: a full list drops the request closest to its end",
              "[gpu][resolve_rules]") {
  ResolveReadbackRequests requests;
  REQUIRE(requests.Request(0x1000, 0x1000, 1));
  for (rex::u32 i = 1; i < ResolveReadbackRequests::kMaxRequests; ++i) {
    REQUIRE(requests.Request(0x1000 + i * 0x1000, 0x1000, 5));
  }
  CHECK(requests.Touches(0x1000, 0x1000));
  REQUIRE(requests.Request(0x100000, 0x1000, 5));
  CHECK_FALSE(requests.Touches(0x1000, 0x1000));
  CHECK(requests.Touches(0x2000, 0x1000));
  CHECK(requests.Touches(0x100000, 0x1000));
}
