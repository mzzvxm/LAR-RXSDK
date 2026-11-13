/**
 * @file        kernel/xboxkrnl/critical_section_spin.h
 * @brief       The spin phase of RtlEnterCriticalSection
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/platform.h>
#include <rex/types.h>

#if REX_ARCH_AMD64
#include <emmintrin.h>  // _mm_pause
#endif

namespace rex::kernel::xboxkrnl {

/**
 * Tells the CPU that this thread is busy-waiting: PAUSE on x86-64, YIELD on ARM64.
 *
 * The other hyper-thread of the core gets the execution units, and the loop stops hammering the
 * cache line with back-to-back locked instructions.
 */
inline void SpinPause() {
#if REX_ARCH_AMD64
  _mm_pause();
#elif REX_ARCH_ARM64
#if REX_COMPILER_MSVC
  __yield();
#else
  __asm__ volatile("yield" ::: "memory");
#endif
#endif
}

/**
 * Spin phase of RtlEnterCriticalSection: tries up to `spin_count` times to take a free lock word
 * (lock_count -1 becomes 0).
 *
 * Test and test and set: the locked compare exchange only runs when a plain read sees the lock
 * free. While another thread holds it, the spinners share the cache line read only instead of
 * bouncing it between cores, and every failed try pauses the CPU. A spin count of 0 does not
 * touch the word.
 *
 * @param lock_count The lock word of the critical section.
 * @param spin_count How many tries to make.
 * @return True when the lock was taken, false when it was still held after all the tries (the
 *         caller then queues as a waiter).
 */
inline bool SpinAcquire(std::atomic_ref<i32> lock_count, u32 spin_count) {
  while (spin_count--) {
    i32 expected = -1;
    if (lock_count.load(std::memory_order_relaxed) == -1 &&
        lock_count.compare_exchange_strong(expected, 0)) {
      return true;
    }
    SpinPause();
  }
  return false;
}

}  // namespace rex::kernel::xboxkrnl
