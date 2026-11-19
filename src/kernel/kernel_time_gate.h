/**
 * @file        kernel/kernel_time_gate.h
 * @brief       When a thread's CPU time is written to its KTHREAD again
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
 * Rate limit for KernelThread::WriteKernelTime. Reading the CPU time of a thread is a system call
 * (GetThreadTimes on Windows), and every wait, delay and yield of a guest thread makes it, up to
 * hundreds of thousands of times a second across the threads of a title. The KTHREAD field it
 * fills counts milliseconds, and the thread's CPU time cannot advance faster than the clock, so a
 * value read less than a millisecond ago is within one unit of the field: the title cannot tell
 * the two apart. The gate lets one write through per interval and skips the rest.
 *
 * @param last_tick The tick of the last write that went through, 0 before the first. Updated when
 *        this returns true. Owned by one thread, so it needs no lock.
 * @param now_tick The host tick count now.
 * @param interval_ticks The shortest time between two writes, in the same ticks. 0 lets every
 *        call through.
 * @return True when the write should be made now.
 */
inline bool KernelTimeDue(u64& last_tick, u64 now_tick, u64 interval_ticks) noexcept {
  // A clock that moved back makes the difference huge, so that case writes at once too.
  if (interval_ticks == 0 || last_tick == 0 || now_tick - last_tick >= interval_ticks) {
    last_tick = now_tick;
    return true;
  }
  return false;
}

}  // namespace rex::kernel
