/**
 * @file        tests/unit/kernel/zero_delay_policy_test.cpp
 * @brief       Which guest threads sleep on a delay of zero length and which only yield
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/types.h>

#include "kernel/zero_delay_policy.h"

namespace {

using rex::i32;
using rex::kernel::ZeroDelaySleeps;

REX_TEST_CASE("A thread of normal priority yields on a zero delay", "[kernel][thread][delay]") {
  // 0 is what a thread holds until the guest sets a priority.
  CHECK_FALSE(ZeroDelaySleeps(0));
}

REX_TEST_CASE("A thread above normal priority yields on a zero delay", "[kernel][thread][delay]") {
  for (const i32 increment : {1, 2, 0x11, 0x12, 0x22, 0x23, 31}) {
    CAPTURE(increment);
    CHECK_FALSE(ZeroDelaySleeps(increment));
  }
}

REX_TEST_CASE("A thread below normal priority sleeps on a zero delay", "[kernel][thread][delay]") {
  for (const i32 increment : {-1, -2, -0x11, -0x12, -0x22, -0x23}) {
    CAPTURE(increment);
    CHECK(ZeroDelaySleeps(increment));
  }
}

REX_TEST_CASE("The decision reads the guest's scale, not a host priority value",
              "[kernel][thread][delay]") {
  // 8, 16, 24 and 32 are the POSIX host priorities of below normal, normal, above normal and
  // highest: as guest increments they are all above normal, so none of them may sleep.
  for (const i32 increment : {1, 8, 16, 24, 32}) {
    CAPTURE(increment);
    CHECK_FALSE(ZeroDelaySleeps(increment));
  }
}

}  // namespace
