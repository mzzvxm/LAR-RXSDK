/**
 * @file        gpu/xenos/register_blocks.h
 * @brief       Which registers a range write can copy at once
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <utility>

#include <rex/types.h>
#include <rex/xenos/register.h>

namespace rex::gpu {

/**
 * Reports whether a write to the register does more than update the register file: the scratch
 * writeback, COHER_STATUS_HOST and the gamma ramp (DC_LUT_RW_INDEX through DC_LUT_30_COLOR).
 * Branch-free, so an ordinary write skips CommandProcessor::HandleSpecialRegisterWrite.
 */
constexpr bool IsSpecialRegister(u32 index) {
  return (static_cast<u32>((index - std::to_underlying(xenos::Register::SCRATCH_REG0)) < 8) |
          static_cast<u32>(index == std::to_underlying(xenos::Register::COHER_STATUS_HOST)) |
          static_cast<u32>((index - std::to_underlying(xenos::Register::DC_LUT_RW_INDEX)) <=
                           (std::to_underlying(xenos::Register::DC_LUT_30_COLOR) -
                            std::to_underlying(xenos::Register::DC_LUT_RW_INDEX)))) != 0;
}

/**
 * Reports whether any constant from first_constant to last_constant, inclusive, is set in a usage
 * bitmap of 64 constants per word, such as the constants the current shaders read.
 */
inline bool RangeHasAnyConstantUsage(const u64* usage_map, u32 first_constant, u32 last_constant) {
  if (first_constant > last_constant) {
    return false;
  }
  u32 first_word = first_constant >> 6;
  u32 last_word = last_constant >> 6;
  u32 first_bit = first_constant & 63;
  u32 last_bit = last_constant & 63;
  if (first_word == last_word) {
    u32 bit_count = last_bit - first_bit + 1;
    u64 mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
    return (usage_map[first_word] & mask) != 0;
  }
  if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
    return true;
  }
  for (u32 word = first_word + 1; word < last_word; ++word) {
    if (usage_map[word]) {
      return true;
    }
  }
  u64 last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
  return (usage_map[last_word] & last_mask) != 0;
}

/**
 * Returns the first register after index that needs more than a store: a special register (the
 * scratch registers, COHER_STATUS_HOST, the gamma ramp), a shader constant block, or the end of the
 * register file. UINT32_MAX when there is none (index is at or past the end).
 */
inline u32 NextNonOrdinaryRegister(u32 index) {
  constexpr u32 kBoundaries[] = {
      std::to_underlying(xenos::Register::SCRATCH_REG0),
      std::to_underlying(xenos::Register::COHER_STATUS_HOST),
      std::to_underlying(xenos::Register::DC_LUT_RW_INDEX),
      std::to_underlying(xenos::Register::SHADER_CONSTANT_000_X),
      std::to_underlying(xenos::Register::SHADER_CONSTANT_FETCH_00_0),
      std::to_underlying(xenos::Register::SHADER_CONSTANT_BOOL_000_031),
      static_cast<u32>(xenos::RegisterFile::kRegisterCount),
  };
  static_assert(kBoundaries[0] < kBoundaries[1] && kBoundaries[1] < kBoundaries[2] &&
                    kBoundaries[2] < kBoundaries[3] && kBoundaries[3] < kBoundaries[4] &&
                    kBoundaries[4] < kBoundaries[5] && kBoundaries[5] < kBoundaries[6],
                "the boundaries are in ascending order");
  for (u32 boundary : kBoundaries) {
    if (boundary > index) {
      return boundary;
    }
  }
  return UINT32_MAX;
}

}  // namespace rex::gpu
