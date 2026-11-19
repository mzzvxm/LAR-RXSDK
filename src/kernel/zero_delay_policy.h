/**
 * @file        kernel/zero_delay_policy.h
 * @brief       What a guest delay of zero length does on the host
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::kernel {

/**
 * The guest's priority increment below which a thread counts as below normal. The increment is the
 * value KeSetBasePriorityThread gave the thread: 0 for a normal thread and negative for a lower
 * one, on every host. It is not a host priority: rex::platform::ThreadPriority::kBelowNormal is -1
 * on Windows and 8 on POSIX, so comparing the guest's value with it only means "below normal" on
 * Windows.
 */
inline constexpr i32 kGuestPriorityBelowNormal = -1;

/**
 * Decides how KeDelayExecutionThread serves a non-alertable delay of zero length. A thread below
 * normal priority sleeps a moment so that the threads above it run first; any other thread only
 * offers its time slice to them.
 *
 * @param guest_priority The thread's priority increment as the guest set it (0 until it does).
 * @return True to sleep for a short time, false to yield the time slice.
 */
inline constexpr bool ZeroDelaySleeps(i32 guest_priority) noexcept {
  return guest_priority <= kGuestPriorityBelowNormal;
}

}  // namespace rex::kernel
