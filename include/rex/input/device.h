#pragma once
/**
 * @file        rex/input/device.h
 * @brief       Input device identity shared by drivers and device assignment.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

namespace rex::input {

constexpr uint32_t kMaxGuestUsers = 4;

/// Driver-scoped device handle. Never reused within a process run, so a handle
/// that outlives its device resolves to nothing rather than aliasing whichever
/// device took the freed slot.
enum class DeviceId : uint64_t { kInvalid = 0 };

/// Controller family, for button glyphs. kGeneric is anything the driver cannot tell.
enum class PadKind : uint8_t { kGeneric = 0, kXbox, kPlayStation, kSwitch, kSteamDeck };

/// The USB vendor decides first (Sony, Nintendo, Valve, Microsoft); a driver's own guess
/// (SDL's gamepad type) only settles pads from other vendors, such as a DualShock clone.
constexpr PadKind ClassifyPad(uint16_t vendor_id, PadKind driver_hint) {
  switch (vendor_id) {
    case 0x054C:
      return PadKind::kPlayStation;
    case 0x057E:
      return PadKind::kSwitch;
    case 0x28DE:
      return PadKind::kSteamDeck;
    case 0x045E:
      return PadKind::kXbox;
    default:
      return driver_hint;
  }
}

struct DeviceInfo {
  DeviceId id = DeviceId::kInvalid;
  uint32_t ordinal = 0;  // connection order, assigned by InputSystem
  std::string name;
  std::string guid;
  bool synthetic = false;  // keyboard/mouse emulation or the NOP stand-in
  uint16_t vendor_id = 0;  // USB ids; 0 when the driver does not know them
  uint16_t product_id = 0;
  PadKind pad_kind = PadKind::kGeneric;
};

}  // namespace rex::input
