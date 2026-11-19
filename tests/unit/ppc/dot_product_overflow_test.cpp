/**
 * @file        ppc/dot_product_overflow_test.cpp
 * @brief       Tests for vmsum3fp128 / vmsum4fp128 (rex::ppc::Vmsum3fp128 / Vmsum4fp128)
 *
 * A dot product of finite inputs that overflows float32 is a QNaN on the console, where DPPS gives
 * an infinity. Everything else stays what DPPS gives: finite results bit for bit, infinities and
 * NaNs that come from the inputs, and the element vmsum3fp128 leaves out.
 *
 * Vectors are written in host lane order: lane 0 is the element vmsum3fp128 leaves out.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>

#include <rex/ppc/dot_product.h>

namespace {

using Lanes = std::array<uint32_t, 4>;

constexpr uint32_t kQNaN = 0x7FC00000u;
constexpr uint32_t kPlusInf = 0x7F800000u;
constexpr uint32_t kMinusInf = 0xFF800000u;
constexpr uint32_t kMaxFloat = 0x7F7FFFFFu;  // FLT_MAX
constexpr uint32_t kMinusMax = 0xFF7FFFFFu;  // -FLT_MAX
constexpr uint32_t kTwoPow64 = 0x5F800000u;  // 2^64
constexpr uint32_t kTwoPow63 = 0x5F000000u;  // 2^63
constexpr uint32_t kOne = 0x3F800000u;
constexpr uint32_t kTwo = 0x40000000u;

simde__m128 Load(const Lanes& lanes) {
  return simde_mm_castsi128_ps(
      simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(lanes.data())));
}

Lanes Store(simde__m128 v) {
  Lanes lanes{};
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(lanes.data()), simde_mm_castps_si128(v));
  return lanes;
}

Lanes Splat(uint32_t bits) {
  return {bits, bits, bits, bits};
}

bool IsNaNBits(uint32_t bits) {
  return (bits & 0x7FFFFFFFu) > kPlusInf;
}

float AsFloat(uint32_t bits) {
  return std::bit_cast<float>(bits);
}

uint32_t Bits(float f) {
  return std::bit_cast<uint32_t>(f);
}

// What the generator wrote before: the plain dot product
Lanes Plain3(const Lanes& a, const Lanes& b) {
  return Store(simde_mm_dp_ps(Load(a), Load(b), 0xEF));
}
Lanes Plain4(const Lanes& a, const Lanes& b) {
  return Store(simde_mm_dp_ps(Load(a), Load(b), 0xFF));
}

Lanes Vmsum3(const Lanes& a, const Lanes& b) {
  return Store(rex::ppc::Vmsum3fp128(Load(a), Load(b)));
}
Lanes Vmsum4(const Lanes& a, const Lanes& b) {
  return Store(rex::ppc::Vmsum4fp128(Load(a), Load(b)));
}

}  // namespace

TEST_CASE("vmsum: a finite result is what the plain dot product gives, bit for bit",
          "[ppc][vmsum]") {
  const std::array<Lanes, 8> values = {{
      {Bits(1.0f), Bits(1.5f), Bits(1.1f), Bits(1.9f)},
      {Bits(-3.25f), Bits(0.0f), Bits(7.0f), Bits(-0.0f)},
      {Bits(1.0e-30f), Bits(2.0e-30f), Bits(-3.0e-30f), Bits(4.0e-30f)},
      {Bits(1.0e18f), Bits(-2.0e18f), Bits(3.0e18f), Bits(4.0e18f)},
      {kMaxFloat, 0u, 0u, 0u},
      {0u, kMaxFloat, 0u, kMinusMax},
      {Bits(123456.789f), Bits(-0.001f), Bits(42.0f), Bits(1.0f)},
      {kTwoPow63, kTwoPow63, kOne, kTwo},
  }};
  for (const Lanes& a : values) {
    for (const Lanes& b : values) {
      const Lanes plain3 = Plain3(a, b);
      const Lanes plain4 = Plain4(a, b);
      // The fixture only checks the rule where the plain result is finite; anything else is the
      // subject of the cases below.
      if (!IsNaNBits(plain3[0]) && (plain3[0] & 0x7F800000u) != kPlusInf) {
        CHECK(Vmsum3(a, b) == plain3);
      }
      if (!IsNaNBits(plain4[0]) && (plain4[0] & 0x7F800000u) != kPlusInf) {
        CHECK(Vmsum4(a, b) == plain4);
      }
    }
  }
}

TEST_CASE("vmsum3fp128: overflow of finite inputs is a QNaN in every lane", "[ppc][vmsum]") {
  // upstream rexglue issue 491: v = [-FLT_MAX x3, 0], v . v
  const Lanes v = {0u, kMinusMax, kMinusMax, kMinusMax};
  REQUIRE(Plain3(v, v) == Splat(kPlusInf));  // what DPPS gave
  CHECK(Vmsum3(v, v) == Splat(kQNaN));
}

TEST_CASE("vmsum3fp128: overflow towards minus infinity is the same QNaN", "[ppc][vmsum]") {
  const Lanes a = {0u, kMinusMax, kMinusMax, kOne};
  const Lanes b = {0u, kMaxFloat, kMaxFloat, kOne};
  REQUIRE(Plain3(a, b)[0] == kMinusInf);
  CHECK(Vmsum3(a, b) == Splat(kQNaN));
}

TEST_CASE("vmsum3fp128: three finite products whose sum overflows", "[ppc][vmsum]") {
  // 2^64 * 2^63 = 2^127 three times: every product is finite, the sum (1.5 * 2^128) is not
  const Lanes a = Splat(kTwoPow64);
  const Lanes b = Splat(kTwoPow63);
  REQUIRE(Plain3(a, b) == Splat(kPlusInf));
  CHECK(Vmsum3(a, b) == Splat(kQNaN));
}

TEST_CASE("vmsum3fp128: products that overflow but cancel give the finite sum", "[ppc][vmsum]") {
  // [FLT_MAX, -FLT_MAX, 1, (unused)] . [2, 2, 0, (unused)]: the float32 products are +inf and
  // -inf, the exact sum is 0
  const Lanes a = {0u, kMaxFloat, kMinusMax, kOne};
  const Lanes b = {0u, kTwo, kTwo, 0u};
  CHECK(Vmsum3(a, b) == Splat(0u));
}

TEST_CASE("vmsum3fp128: an infinity from the inputs passes through", "[ppc][vmsum]") {
  const Lanes a = {0u, kPlusInf, kOne, kOne};
  const Lanes b = {0u, kOne, kOne, kOne};
  REQUIRE(Plain3(a, b) == Splat(kPlusInf));
  CHECK(Vmsum3(a, b) == Splat(kPlusInf));

  const Lanes negative = {0u, kMinusInf, kOne, kOne};
  CHECK(Vmsum3(negative, b) == Splat(kMinusInf));
}

TEST_CASE("vmsum3fp128: a NaN or infinity times zero from the inputs stays a NaN", "[ppc][vmsum]") {
  const Lanes nan_in = {0u, 0x7FC11111u, kOne, kOne};
  const Lanes ones = {0u, kOne, kOne, kOne};
  for (uint32_t lane : Vmsum3(nan_in, ones))
    CHECK(IsNaNBits(lane));

  const Lanes inf_times_zero_a = {0u, kPlusInf, kOne, kOne};
  const Lanes inf_times_zero_b = {0u, 0u, kOne, kOne};
  for (uint32_t lane : Vmsum3(inf_times_zero_a, inf_times_zero_b))
    CHECK(IsNaNBits(lane));
}

TEST_CASE("vmsum3fp128: the element it leaves out does not count", "[ppc][vmsum]") {
  // lane 0 holds a NaN, and then an overflowing product: neither reaches the result
  const Lanes a = {0x7FC12345u, kOne, kOne, kOne};
  const Lanes b = {kOne, kOne, kOne, kOne};
  CHECK(Vmsum3(a, b) == Splat(Bits(3.0f)));

  const Lanes big_a = {kMaxFloat, kOne, kOne, kOne};
  const Lanes big_b = {kMaxFloat, kOne, kOne, kOne};
  CHECK(Vmsum3(big_a, big_b) == Splat(Bits(3.0f)));
}

TEST_CASE("vmsum4fp128: all four elements count", "[ppc][vmsum]") {
  const Lanes v = {kMinusMax, kMinusMax, kMinusMax, kMinusMax};
  REQUIRE(Plain4(v, v) == Splat(kPlusInf));
  CHECK(Vmsum4(v, v) == Splat(kQNaN));

  // The same overflow in the element vmsum3fp128 leaves out does count here
  const Lanes big = {kMaxFloat, 0u, 0u, 0u};
  CHECK(Vmsum4(big, big) == Splat(kQNaN));
  CHECK(Vmsum3(big, big) == Splat(0u));

  const Lanes a = {kOne, kOne, kOne, kOne};
  const Lanes b = {kTwo, kTwo, kTwo, kTwo};
  CHECK(Vmsum4(a, b) == Splat(Bits(8.0f)));
  CHECK(AsFloat(Vmsum4(a, b)[0]) == 8.0f);
}

TEST_CASE("vmsum4fp128: an infinity from the inputs passes through, a NaN stays one",
          "[ppc][vmsum]") {
  const Lanes a = {kOne, kOne, kOne, kMinusInf};
  const Lanes b = {kOne, kOne, kOne, kOne};
  CHECK(Vmsum4(a, b) == Splat(kMinusInf));

  const Lanes nan_in = {kOne, 0x7FC00001u, kOne, kOne};
  for (uint32_t lane : Vmsum4(nan_in, b))
    CHECK(IsNaNBits(lane));
}
