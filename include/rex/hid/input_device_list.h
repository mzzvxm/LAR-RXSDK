/**
 * @file        hid/input_device_list.h
 * @brief       The connected pads, with their names, and a counter that moves when they change
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <rex/hid/pad.h>
#include <rex/module.h>
#include <rex/types.h>

namespace rex::hid {

/** The pad family, for button glyphs. kGeneric is any pad the other families do not claim. */
enum class PadKind : u8 {
  kGeneric,
  kXbox,
  kPlayStation,
  kNintendo,
  kSteamDeck,
};

/**
 * Returns the family of a pad. The USB vendor decides first (Sony, Nintendo, Valve, Microsoft). The
 * type a driver recognised only settles pads of other vendors, such as a DualShock clone.
 *
 * @param vendor The USB vendor, 0 when unknown.
 * @param type The type the driver reported, kUnknown when none.
 */
constexpr PadKind ClassifyPad(u16 vendor, PadType type) {
  switch (vendor) {
    case 0x054C:
      return PadKind::kPlayStation;
    case 0x057E:
      return PadKind::kNintendo;
    case 0x28DE:
      return PadKind::kSteamDeck;
    case 0x045E:
      return PadKind::kXbox;
    default:
      break;
  }
  switch (type) {
    case PadType::kX360:
    case PadType::kXOne:
      return PadKind::kXbox;
    case PadType::kPS3:
    case PadType::kPS4:
    case PadType::kPS5:
      return PadKind::kPlayStation;
    case PadType::kSwitchPro:
    case PadType::kSwitchJoyCon:
      return PadKind::kNintendo;
    case PadType::kUnknown:
    case PadType::kStandard:
      break;
  }
  return PadKind::kGeneric;
}

/** One connected pad. */
struct InputDeviceInfo {
  /** The user value of a pad that no user holds. */
  static constexpr u32 kNoUser = 0xFF;

  std::string name;   /**< The name the driver reports, empty when it reports none. */
  u16 vendor_id = 0;  /**< USB vendor, 0 when unknown. */
  u16 product_id = 0; /**< USB product, 0 when unknown. */
  PadKind pad_kind = PadKind::kGeneric;
  u32 user = kNoUser; /**< The user the pad feeds, 0 to 3, or kNoUser. */

  friend bool operator==(const InputDeviceInfo&, const InputDeviceInfo&) = default;
};

/**
 * Lists the pads the input system merged, for a caller that shows pad names or button glyphs. It
 * is served by the same library as IInputSystem and finds the same pads. Any thread.
 *
 * The list is kept current by the drivers' own connect and disconnect events: nothing here polls
 * a device. A call reads the devices the way every IInputSystem call does, so a change is found
 * when a caller asks, and a connection listener of IInputSystem may run on the calling thread.
 */
REX_DECLARE_INTERFACE(IInputDeviceList, 1)
/**
 * Returns a counter that moves when the list of Devices() changes: a pad connects or
 * disconnects, or a pad's user changes. It moves for a second pad that joins a user, as it does
 * with hid.shared_pads on, which IInputSystem::OnConnectionChange does not report. It does not
 * move for input, for the keyboard and mouse pad or for a driver event that leaves the list as
 * it was. The first value is 0, the list of a system with no pad.
 *
 * To follow the list, read this first and Devices() second, and read Devices() again when the
 * value differs from the one kept. A change between the two calls then shows at the next read.
 */
  [[nodiscard]] virtual u32 DeviceGeneration() = 0;

  /**
   * Returns the connected pads in connection order. The keyboard and mouse pad and the idle pad are
   * not pads of the player and are left out.
   *
   * @return A pad per entry, empty when none is connected.
   */
  [[nodiscard]] virtual std::vector<InputDeviceInfo> Devices() = 0;
};

}  // namespace rex::hid
