/**
 * @file        ppc/dot_product.h
 * @brief       vmsum3fp128 / vmsum4fp128: a dot product whose overflow gives a QNaN
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The code generator used to write simde_mm_dp_ps(a, b, 0xEF) / (a, b, 0xFF) for
 *              these two instructions. DPPS gives +-infinity when the dot product of finite
 *              inputs overflows float32; the console gives the QNaN 0x7FC00000 in every lane
 *              (upstream rexglue issue 491: a collision test that starts from -FLT_MAX took the
 *              wrong branch). The rule is xenia-canary's (DOT_PRODUCT_3 / DOT_PRODUCT_4 in
 *              src/xenia/cpu/backend/x64/x64_sequences.cc): when the result is not finite and every
 *              input element that counts is finite, the products are summed in float64 and the
 *              result is that sum as a float32, or the QNaN if that overflows. An infinity or a
 *              NaN that comes from the inputs passes through exactly as DPPS gives it.
 *
 *              A finite result costs one exponent test on top of DPPS. The rest is out of line.
 */

#pragma once

#include <cstdint>
#include <cstring>

#include <simde/x86/sse.h>
#include <simde/x86/sse2.h>
#include <simde/x86/sse4.1.h>

namespace rex::ppc {

namespace detail {

constexpr uint32_t kDotProductQNaN = 0x7FC00000u;
constexpr uint32_t kFloatExponentMask = 0x7F800000u;

inline bool IsFiniteFloatBits(uint32_t bits) {
  return (bits & kFloatExponentMask) != kFloatExponentMask;
}

// `r` is the DPPS result for `imm8` and has an infinite or NaN lane.
__attribute__((noinline)) inline simde__m128 DotProductNonFinite(simde__m128 a, simde__m128 b,
                                                                 simde__m128 r, int imm8) {
  uint32_t a_bits[4];
  uint32_t b_bits[4];
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(a_bits), simde_mm_castps_si128(a));
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(b_bits), simde_mm_castps_si128(b));

  double products[4] = {};
  for (int i = 0; i < 4; ++i) {
    if (((imm8 >> (4 + i)) & 1) == 0)
      continue;  // this element is not part of the sum
    if (!IsFiniteFloatBits(a_bits[i]) || !IsFiniteFloatBits(b_bits[i]))
      return r;  // an infinity or NaN from the inputs: DPPS's answer stands
    float a_value;
    float b_value;
    std::memcpy(&a_value, &a_bits[i], sizeof(a_value));
    std::memcpy(&b_value, &b_bits[i], sizeof(b_value));
    products[i] = static_cast<double>(a_value) * static_cast<double>(b_value);  // exact
  }

  // Four exact products cannot overflow a double, so the sum is finite. xenia's grouping: elements
  // 0 and 2 together, 1 and 3 together, then the two sums (the same pairs in either lane order).
  const float sum = static_cast<float>((products[1] + products[3]) + (products[0] + products[2]));
  uint32_t sum_bits;
  std::memcpy(&sum_bits, &sum, sizeof(sum_bits));
  if (!IsFiniteFloatBits(sum_bits))
    sum_bits = kDotProductQNaN;  // overflow of finite inputs

  uint32_t out[4];
  for (int i = 0; i < 4; ++i)
    out[i] = ((imm8 >> i) & 1) != 0 ? sum_bits : 0u;
  return simde_mm_castsi128_ps(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(out)));
}

// simde_mm_dp_ps(a, b, kImm8), with the overflow rule in the file comment.
template <int kImm8>
__attribute__((always_inline)) inline simde__m128 DotProduct(simde__m128 a, simde__m128 b) {
  const simde__m128 r = simde_mm_dp_ps(a, b, kImm8);
  const simde__m128i exponent_mask = simde_mm_set1_epi32(static_cast<int>(kFloatExponentMask));
  const simde__m128i exponent = simde_mm_and_si128(simde_mm_castps_si128(r), exponent_mask);
  if (simde_mm_movemask_epi8(simde_mm_cmpeq_epi32(exponent, exponent_mask)) != 0) [[unlikely]]
    return DotProductNonFinite(a, b, r, kImm8);
  return r;
}

}  // namespace detail

/// vmsum3fp128: the dot product of the guest's first three elements (host lanes 3, 2, 1), in all
/// four lanes.
[[nodiscard]] __attribute__((always_inline)) inline simde__m128 Vmsum3fp128(
    simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProduct<0xEF>(a, b);
}

/// vmsum4fp128: the dot product of all four elements, in all four lanes.
[[nodiscard]] __attribute__((always_inline)) inline simde__m128 Vmsum4fp128(
    simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProduct<0xFF>(a, b);
}

}  // namespace rex::ppc
