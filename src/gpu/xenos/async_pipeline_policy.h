/**
 * @file        gpu/xenos/async_pipeline_policy.h
 * @brief       How long draws wait for pipelines that are being created, and when a frame that
 *              was left incomplete for them is not presented
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <optional>

#include <rex/types.h>

namespace rex::gpu::async_pipeline {

/** How long a draw inside an occlusion query waits for its pipeline, outside the budget. */
inline constexpr u64 kOcclusionQueryWaitUs = 500'000;

/** How long a draw waits for its pipeline when the budget has no limit. */
inline constexpr u64 kUnlimitedWaitUs = 60'000'000;

/**
 * Returns how long a draw whose pipeline is still being created may wait for it, which is what is
 * left of the frame's budget.
 *
 * @param budget_ms The most a frame's draws wait in total, in milliseconds: 0 leaves every draw out
 *        at once, -1 has no limit.
 * @param waited_us What the earlier draws of the frame have waited.
 * @return The time in microseconds, 0 when the draw is to be left out without waiting.
 */
constexpr u64 GetBudgetedWaitUs(i32 budget_ms, u64 waited_us) {
  if (budget_ms == 0) {
    return 0;
  }
  if (budget_ms < 0) {
    return kUnlimitedWaitUs;
  }
  const u64 budget_us = static_cast<u64>(budget_ms) * 1000;
  return waited_us >= budget_us ? 0 : budget_us - waited_us;
}

/**
 * Decides whether a frame's present is skipped, so the previous picture stays, because draws were
 * left out of it for pipelines that were not ready (a presented frame shows the objects of those
 * draws missing, which flashes). A run of skipped frames ends after max_ms, so a slow pipeline
 * creation shows as objects missing for a moment rather than a picture held still.
 *
 * Call it once per frame, at its swap.
 *
 * @param frame_incomplete Whether a draw of the frame was left out for a pipeline that was not
 *        ready (one that failed to be created does not count: the frame would never be complete).
 * @param now The time of the swap.
 * @param max_ms How long a run of skipped presents lasts at most, 0 for as long as it takes.
 * @param skipped_since When the run of skipped presents started, none outside a run. Updated: a
 *        complete frame ends the run, the first incomplete frame starts one.
 * @return True when the present is to be skipped.
 */
inline bool ShouldSkipPresent(bool frame_incomplete, std::chrono::steady_clock::time_point now,
                              u32 max_ms,
                              std::optional<std::chrono::steady_clock::time_point>& skipped_since) {
  if (!frame_incomplete) {
    skipped_since.reset();
    return false;
  }
  if (!skipped_since) {
    skipped_since = now;
    return true;
  }
  // Skipped for long enough: show what there is until a frame is complete.
  return !max_ms || now - *skipped_since < std::chrono::milliseconds(max_ms);
}

}  // namespace rex::gpu::async_pipeline
