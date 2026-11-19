/**
 * @file        gpu/xenos/resolve_rules.h
 * @brief       Rules that pick which resolves get special treatment
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

enum class ReadbackResolveMode : u8 {
  kDisabled,
  kFast,
  kSome,
  kFull,
};

/** An inclusive range of a rule key. A bound left out is 0 or the maximum. */
struct ResolveRuleRange {
  bool set = false;
  u64 lo = 0;
  u64 hi = ~u64(0);

  bool Contains(u64 value) const { return !set || (value >= lo && value <= hi); }
};

/** One resolve that has been made, as a rule sees it. */
struct ResolveDescription {
  u32 written_address = 0; /**< Guest physical address of the first byte the resolve wrote. */
  u32 written_length = 0;  /**< Bytes written. */
  u32 pitch = 0;           /**< The destination's pitch, in pixels. */
  u32 height = 0;          /**< The destination's height, in pixels. */
  u32 format = 0;          /**< The destination's xenos::ColorFormat. */
  u32 source = 0;          /**< RB_COPY_CONTROL copy_src_select, 4 is depth. */
};

/** One rule. All of its keys must hold for a resolve. */
struct ResolveRule {
  ResolveRuleRange address; /**< The written memory touches this range. */
  ResolveRuleRange length;
  ResolveRuleRange pitch;
  ResolveRuleRange height;
  ResolveRuleRange format;
  ResolveRuleRange source;
  ReadbackResolveMode mode = ReadbackResolveMode::kFull;
  std::string text;

  bool Matches(const ResolveDescription& resolve) const;
};

/**
 * Parses one rule, key=value pairs separated by ',': addr=LO-HI (the written memory touches that
 * range), len=, pitch=, height= (the destination's, in pixels), fmt= (its colour format), src= (the
 * copy's source: colour target 0-3, depth 4) and mode=full|fast|some. A range is N, LO-HI, LO- or
 * -HI, in decimal or 0x hex.
 *
 * @return The rule, or the reason the text is not one.
 */
[[nodiscard]] REX_GPU_XENOS_API Result<ResolveRule> ParseResolveRule(std::string_view text);

/**
 * Parses rules separated by ';'. A rule that does not parse is left out with a warning that names
 * the setting. The text "off" is no rules.
 *
 * @param setting_name The setting the text came from, for the warnings.
 */
[[nodiscard]] REX_GPU_XENOS_API std::vector<ResolveRule> ParseResolveRules(
    std::string_view text, std::string_view setting_name);

/**
 * The rules a string setting holds. The rules follow the setting as it changes during play: the
 * thread that changes it parses them, and any thread can ask which one a resolve matches.
 */
class REX_GPU_XENOS_API ResolveRuleSetting {
 public:
  /**
   * @param setting The setting that holds the rules. It must outlive this object.
   */
  explicit ResolveRuleSetting(Setting<std::string>& setting);

  /** Reports whether the setting holds a rule. Any thread, no lock. */
  bool HasRules() const { return rule_count_.load(std::memory_order_relaxed) != 0; }

  /**
   * Finds the first rule that a resolve matches.
   *
   * @return A copy of the rule, or nothing.
   */
  std::optional<ResolveRule> Match(const ResolveDescription& resolve) const;

 private:
  void Assign();

  Setting<std::string>& setting_;
  mutable std::mutex mutex_;
  std::shared_ptr<const std::vector<ResolveRule>> rules_;
  std::atomic<u32> rule_count_{0};
  ScopeGuard watch_;
};

/**
 * Requests, made while the title runs, to read back the resolves that write a range of guest
 * memory for a number of frames: a screenshot the title reads from its front buffer on the CPU, for
 * instance. A frame is a swap the command processor has run, and the command processor may run a
 * few frames behind the title, so a request is best repeated every frame it is needed.
 */
class REX_GPU_XENOS_API ResolveReadbackRequests {
 public:
  /** The most ranges that wait at once. */
  static constexpr size_t kMaxRequests = 16;

  /**
   * Asks for the resolves that write a range to be read back. A range asked for again takes the
   * longer of the two lifetimes. Any thread.
   *
   * @param address Guest physical address of the range.
   * @param length Bytes in the range.
   * @param frames How many swaps the request lasts.
   * @return False when the request asks for nothing (no bytes, no frames, or a range past the end
   *         of the 32 bit address space).
   */
  bool Request(u32 address, u32 length, u32 frames);

  /** Reports whether any request is waiting. Any thread, no lock. */
  bool HasRequests() const { return count_.load(std::memory_order_relaxed) != 0; }

  /**
   * Reports whether the bytes a resolve wrote touch a request. Any thread.
   */
  bool Touches(u32 written_address, u32 written_length);

  /** Counts a frame and drops the requests that have run out. Command processor thread. */
  void EndFrame();

 private:
  struct Entry {
    u64 first = 0; /**< The first byte of the range. */
    u64 last = 0;  /**< The last byte of the range. */
    u32 frames_left = 0;
    bool touched = false; /**< A resolve has written into the range. */
  };

  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::atomic<u32> count_{0};
};

}  // namespace rex::gpu
