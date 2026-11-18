/**
 * @file        hid/sdl3/joystick_update_gate.h
 * @brief       Decides when the SDL3 input driver reads the joysticks itself
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>

#include <rex/types.h>

namespace rex::hid::sdl3 {

/**
 * With SDL's own joystick update switched off, the driver reads the pads when the guest asks
 * (several calls land in the same millisecond: every user, capabilities and state) and from a slow
 * timer for hot-plug while the guest is not asking. This is the "is a read due?" part of both: pure,
 * so it can be tested without SDL.
 */
class JoystickUpdateGate {
 public:
  /**
   * Guest calls closer together than this share one read: the same bound as the 1 ms poll that
   * SDL's event loop used to run.
   */
  static constexpr u64 kGuestMinGapNs = 1'000'000;
  /** The idle timer reads only when nobody has for this long (it ticks every kIdleTimerMs). */
  static constexpr u64 kIdleMaxAgeNs = 400'000'000;
  static constexpr u32 kIdleTimerMs = 500;

  /**
   * Returns true for exactly one caller when no read happened in the last max_age_ns; that caller
   * is then the one who reads, and its now_ns is remembered.
   *
   * @param now_ns The current time in nanoseconds, above zero.
   * @param max_age_ns How recent a read must be to count.
   */
  bool TryBegin(u64 now_ns, u64 max_age_ns) {
    u64 last = last_ns_.load(std::memory_order_relaxed);
    for (;;) {
      // A later time recorded by another thread counts as a read just done.
      if (last != 0 && (now_ns < last || now_ns - last < max_age_ns)) {
        return false;
      }
      if (last_ns_.compare_exchange_weak(last, now_ns, std::memory_order_relaxed)) {
        return true;
      }
    }
  }

 private:
  std::atomic<u64> last_ns_{0}; /**< 0 means never read. */
};

}  // namespace rex::hid::sdl3
