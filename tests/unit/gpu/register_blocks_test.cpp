/**
 * @file        tests/unit/gpu/register_blocks_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <utility>

#include "gpu/xenos/register_blocks.h"

using namespace rex::gpu;

namespace {

constexpr rex::u32 R(rex::xenos::Register reg) {
  return std::to_underlying(reg);
}

bool IsConstantBlockRegister(rex::u32 index) {
  return (index >= R(rex::xenos::Register::SHADER_CONSTANT_000_X) &&
          index <= R(rex::xenos::Register::SHADER_CONSTANT_511_W)) ||
         (index >= R(rex::xenos::Register::SHADER_CONSTANT_FETCH_00_0) &&
          index <= R(rex::xenos::Register::SHADER_CONSTANT_FETCH_31_5)) ||
         (index >= R(rex::xenos::Register::SHADER_CONSTANT_BOOL_000_031) &&
          index <= R(rex::xenos::Register::SHADER_CONSTANT_LOOP_31));
}

}  // namespace

REX_TEST_CASE("IsSpecialRegister names the registers whose writes do more than store",
              "[gpu][registers]") {
  const rex::u32 scratch0 = R(rex::xenos::Register::SCRATCH_REG0);
  for (rex::u32 i = 0; i < 8; ++i) {
    CHECK(IsSpecialRegister(scratch0 + i));
  }
  CHECK_FALSE(IsSpecialRegister(scratch0 - 1));
  CHECK_FALSE(IsSpecialRegister(scratch0 + 8));

  const rex::u32 coher = R(rex::xenos::Register::COHER_STATUS_HOST);
  CHECK(IsSpecialRegister(coher));
  CHECK_FALSE(IsSpecialRegister(coher - 1));
  CHECK_FALSE(IsSpecialRegister(coher + 1));

  CHECK(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_RW_INDEX)));
  CHECK(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_SEQ_COLOR)));
  CHECK(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_PWL_DATA)));
  CHECK(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_30_COLOR)));
  CHECK_FALSE(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_RW_MODE)));
  CHECK_FALSE(IsSpecialRegister(R(rex::xenos::Register::DC_LUT_WRITE_EN_MASK)));

  CHECK_FALSE(IsSpecialRegister(0));
  CHECK_FALSE(IsSpecialRegister(R(rex::xenos::Register::SCRATCH_ADDR)));
  CHECK_FALSE(IsSpecialRegister(R(rex::xenos::Register::SCRATCH_UMSK)));
}

REX_TEST_CASE("NextNonOrdinaryRegister stops at every register that needs more than a store",
              "[gpu][registers]") {
  const rex::u32 count = static_cast<rex::u32>(rex::xenos::RegisterFile::kRegisterCount);
  CHECK(NextNonOrdinaryRegister(0) == R(rex::xenos::Register::SCRATCH_REG0));
  CHECK(NextNonOrdinaryRegister(R(rex::xenos::Register::SCRATCH_REG0)) ==
        R(rex::xenos::Register::COHER_STATUS_HOST));
  CHECK(NextNonOrdinaryRegister(R(rex::xenos::Register::SHADER_CONSTANT_BOOL_000_031)) == count);
  CHECK(NextNonOrdinaryRegister(count - 1) == count);
  CHECK(NextNonOrdinaryRegister(count) == UINT32_MAX);

  // Every register the walk would copy at once, from an ordinary one up to the answer, is
  // ordinary, and the answer itself is not (or is the end of the register file).
  for (rex::u32 index = 0; index < count; ++index) {
    if (IsSpecialRegister(index) || IsConstantBlockRegister(index)) {
      continue;
    }
    const rex::u32 next = NextNonOrdinaryRegister(index);
    REQUIRE(next > index);
    REQUIRE(next <= count);
    for (rex::u32 other = index; other < next; ++other) {
      REQUIRE_FALSE(IsSpecialRegister(other));
      REQUIRE_FALSE(IsConstantBlockRegister(other));
    }
    if (next < count) {
      REQUIRE((IsSpecialRegister(next) || IsConstantBlockRegister(next)));
    }
    // The answer is the same for the whole stretch: no need to ask again inside it.
    index = next - 1;
  }
}

REX_TEST_CASE("RangeHasAnyConstantUsage looks at the bits of the range only", "[gpu][registers]") {
  std::array<rex::u64, 4> map{};
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 0, 255));

  map[0] = rex::u64(1) << 5;
  CHECK(RangeHasAnyConstantUsage(map.data(), 5, 5));
  CHECK(RangeHasAnyConstantUsage(map.data(), 0, 63));
  CHECK(RangeHasAnyConstantUsage(map.data(), 3, 100));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 6, 63));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 0, 4));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 64, 255));
  // An empty range (first after last) has no usage.
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 6, 5));

  // The last bit of a word and the first of the next.
  map = {};
  map[0] = rex::u64(1) << 63;
  CHECK(RangeHasAnyConstantUsage(map.data(), 63, 64));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 64, 127));
  map = {};
  map[1] = 1;
  CHECK(RangeHasAnyConstantUsage(map.data(), 63, 64));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 0, 63));

  // A word in the middle of a range that spans several.
  map = {};
  map[2] = rex::u64(1) << 17;
  CHECK(RangeHasAnyConstantUsage(map.data(), 10, 200));
  CHECK(RangeHasAnyConstantUsage(map.data(), 128 + 17, 128 + 17));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 128 + 18, 255));
  CHECK_FALSE(RangeHasAnyConstantUsage(map.data(), 0, 128 + 16));
}
