/**
 * @file        gpu/xenos/resolve_rules.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "gpu/xenos/resolve_rules.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

namespace rex::gpu {

namespace {

std::string_view Trim(std::string_view text) {
  constexpr std::string_view kBlank = " \t";
  const size_t first = text.find_first_not_of(kBlank);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(kBlank) - first + 1);
}

// Decimal, or hex with a 0x prefix.
std::optional<u64> ParseNumber(std::string_view text) {
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  u64 value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// "N", "LO-HI", "LO-" or "-HI".
std::optional<ResolveRuleRange> ParseRange(std::string_view text) {
  ResolveRuleRange range;
  range.set = true;
  const size_t dash = text.find('-');
  if (dash == std::string_view::npos) {
    const std::optional<u64> value = ParseNumber(text);
    if (!value) {
      return std::nullopt;
    }
    range.lo = range.hi = *value;
    return range;
  }
  const std::string_view left = Trim(text.substr(0, dash));
  const std::string_view right = Trim(text.substr(dash + 1));
  if (left.empty() && right.empty()) {
    return std::nullopt;
  }
  if (!left.empty()) {
    const std::optional<u64> value = ParseNumber(left);
    if (!value) {
      return std::nullopt;
    }
    range.lo = *value;
  }
  if (!right.empty()) {
    const std::optional<u64> value = ParseNumber(right);
    if (!value) {
      return std::nullopt;
    }
    range.hi = *value;
  }
  return range;
}

// Splits at a separator. An empty piece is skipped.
std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> pieces;
  while (!text.empty()) {
    const size_t at = text.find(separator);
    const std::string_view piece = Trim(text.substr(0, at));
    if (!piece.empty()) {
      pieces.push_back(piece);
    }
    if (at == std::string_view::npos) {
      break;
    }
    text.remove_prefix(at + 1);
  }
  return pieces;
}

}  // namespace

bool ResolveRule::Matches(const ResolveDescription& resolve) const {
  if (address.set) {
    const u64 first = resolve.written_address;
    const u64 last = first + (resolve.written_length ? resolve.written_length - 1 : 0);
    if (last < address.lo || first > address.hi) {
      return false;
    }
  }
  return length.Contains(resolve.written_length) && pitch.Contains(resolve.pitch) &&
         height.Contains(resolve.height) && format.Contains(resolve.format) &&
         source.Contains(resolve.source);
}

Result<ResolveRule> ParseResolveRule(std::string_view text) {
  ResolveRule rule;
  rule.text = std::string(Trim(text));
  for (const std::string_view item : Split(text, ',')) {
    const size_t equals = item.find('=');
    if (equals == std::string_view::npos) {
      return Err<ResolveRule>(ErrorCategory::kConfig, std::format("\"{}\" is not key=value", item));
    }
    const std::string_view key = Trim(item.substr(0, equals));
    const std::string_view value = Trim(item.substr(equals + 1));
    if (key == "mode") {
      if (value == "full") {
        rule.mode = ReadbackResolveMode::kFull;
      } else if (value == "fast") {
        rule.mode = ReadbackResolveMode::kFast;
      } else if (value == "some") {
        rule.mode = ReadbackResolveMode::kSome;
      } else {
        return Err<ResolveRule>(ErrorCategory::kConfig,
                                std::format("mode \"{}\" is not full, fast or some", value));
      }
      continue;
    }
    ResolveRuleRange* range = nullptr;
    if (key == "addr") {
      range = &rule.address;
    } else if (key == "len") {
      range = &rule.length;
    } else if (key == "pitch") {
      range = &rule.pitch;
    } else if (key == "height") {
      range = &rule.height;
    } else if (key == "fmt") {
      range = &rule.format;
    } else if (key == "src") {
      range = &rule.source;
    } else if (key == "phase") {
      // phase= keys on the title's render phase markers, which the SDK has no source for.
      return Err<ResolveRule>(ErrorCategory::kNotImplemented,
                              "phase= is not supported here, there are no guest render markers");
    } else {
      return Err<ResolveRule>(ErrorCategory::kConfig, std::format("unknown key \"{}\"", key));
    }
    const std::optional<ResolveRuleRange> parsed = ParseRange(value);
    if (!parsed) {
      return Err<ResolveRule>(ErrorCategory::kConfig,
                              std::format("\"{}\" is not a number or a range", value));
    }
    *range = *parsed;
  }
  return rule;
}

std::vector<ResolveRule> ParseResolveRules(std::string_view text, std::string_view setting_name) {
  std::vector<ResolveRule> rules;
  if (Trim(text) == "off") {
    return rules;
  }
  for (const std::string_view rule_text : Split(text, ';')) {
    Result<ResolveRule> rule = ParseResolveRule(rule_text);
    if (rule) {
      rules.push_back(std::move(*rule));
    } else {
      REXGPU_WARN("{}: the rule \"{}\" is left out, {}", setting_name, rule_text,
                  rule.error().GetDescription());
    }
  }
  return rules;
}

ResolveRuleSetting::ResolveRuleSetting(Setting<std::string>& setting) : setting_(setting) {
  // The watch comes first, so a change between the watch and the first read is not missed.
  watch_ = setting_.OnChange([this](const std::string&) { Assign(); });
  Assign();
}

std::optional<ResolveRule> ResolveRuleSetting::Match(const ResolveDescription& resolve) const {
  if (!HasRules()) {
    return std::nullopt;
  }
  std::shared_ptr<const std::vector<ResolveRule>> rules;
  {
    std::lock_guard lock(mutex_);
    rules = rules_;
  }
  if (!rules) {
    return std::nullopt;
  }
  for (const ResolveRule& rule : *rules) {
    if (rule.Matches(resolve)) {
      return rule;
    }
  }
  return std::nullopt;
}

void ResolveRuleSetting::Assign() {
  // Every assignment reads the setting again under the lock, so the last one to run holds the
  // newest text even when two changes race.
  std::lock_guard lock(mutex_);
  const std::string text = setting_.Get();
  auto rules =
      std::make_shared<const std::vector<ResolveRule>>(ParseResolveRules(text, setting_.GetName()));
  if (!text.empty()) {
    REXGPU_INFO("{}: {} rule(s) from \"{}\"", setting_.GetName(), rules->size(), text);
  }
  rule_count_.store(static_cast<u32>(rules->size()), std::memory_order_relaxed);
  rules_ = std::move(rules);
}

bool ResolveReadbackRequests::Request(u32 address, u32 length, u32 frames) {
  if (!length || !frames) {
    return false;
  }
  const u64 first = address;
  const u64 last = first + length - 1;
  if (last > UINT32_MAX) {
    return false;
  }
  std::lock_guard lock(mutex_);
  for (Entry& entry : entries_) {
    if (entry.first == first && entry.last == last) {
      entry.frames_left = std::max(entry.frames_left, frames);
      return true;
    }
  }
  if (entries_.size() >= kMaxRequests) {
    // Make room by dropping the request that has the fewest frames left.
    entries_.erase(std::min_element(
        entries_.begin(), entries_.end(),
        [](const Entry& a, const Entry& b) { return a.frames_left < b.frames_left; }));
  }
  entries_.push_back({first, last, frames, false});
  count_.store(static_cast<u32>(entries_.size()), std::memory_order_relaxed);
  return true;
}

bool ResolveReadbackRequests::Touches(u32 written_address, u32 written_length) {
  if (!HasRequests() || !written_length) {
    return false;
  }
  const u64 first = written_address;
  const u64 last = first + written_length - 1;
  std::lock_guard lock(mutex_);
  bool touches = false;
  for (Entry& entry : entries_) {
    if (last >= entry.first && first <= entry.last) {
      entry.touched = true;
      touches = true;
    }
  }
  return touches;
}

void ResolveReadbackRequests::EndFrame() {
  if (!HasRequests()) {
    return;
  }
  std::lock_guard lock(mutex_);
  for (Entry& entry : entries_) {
    if (--entry.frames_left == 0 && !entry.touched) {
      REXGPU_INFO("resolve readback request for 0x{:08X}-0x{:08X} ran out with no resolve into it",
                  static_cast<u32>(entry.first), static_cast<u32>(entry.last));
    }
  }
  std::erase_if(entries_, [](const Entry& entry) { return entry.frames_left == 0; });
  count_.store(static_cast<u32>(entries_.size()), std::memory_order_relaxed);
}

}  // namespace rex::gpu
