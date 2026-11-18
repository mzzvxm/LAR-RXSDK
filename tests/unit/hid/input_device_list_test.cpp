/**
 * @file        tests/unit/hid/input_device_list_test.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "input_harness.h"

#include <utility>
#include <vector>

#include <rex/hid/input_device_list.h>

using namespace rex::test_input;

namespace {

using rex::hid::ClassifyPad;
using rex::hid::InputDeviceInfo;
using rex::hid::PadKind;
using rex::hid::PadType;

rex::hid::IInputDeviceList& List() {
  rex::hid::IInputDeviceList* list = REX_INTERFACE(rex::hid::IInputDeviceList);
  REQUIRE(list != nullptr);
  return *list;
}

/** What a driver puts in the PadDevice it enumerates. */
rex::hid::PadDevice Pad(u64 id, i32 xinput_slot, const char* name, u16 vendor = 0, u16 product = 0,
                        PadType type = PadType::kUnknown) {
  rex::hid::PadDevice device = MakePad(id, xinput_slot, name);
  device.info.vendor = vendor;
  device.info.product = product;
  device.info.type = type;
  return device;
}

/** Gives a pad a state, as a driver does for a pad it can read. */
void Plug(int driver, rex::hid::PadDevice device) {
  g_fake[driver].states[device.id.value] = Buttons(0);
  g_fake[driver].devices.push_back(std::move(device));
  g_fake[driver].revision++;
}

void Unplug(int driver, u64 id) {
  std::erase_if(g_fake[driver].devices,
                [&](const rex::hid::PadDevice& device) { return device.id.value == id; });
  g_fake[driver].states.erase(id);
  g_fake[driver].revision++;
}

}  // namespace

REX_TEST_CASE("the device list is served beside the input system while the app runs",
              "[hid][devices]") {
  g_fake = {};
  InputApp app("none");
  CHECK(REX_INTERFACE(rex::hid::IInputDeviceList) == nullptr);
  REQUIRE(app.Start());
  CHECK(REX_INTERFACE(rex::hid::IInputDeviceList) != nullptr);
  CHECK(REX_INTERFACE(rex::hid::IInputSystem) == &app.GetInput());
  app.Finish();
  CHECK(REX_INTERFACE(rex::hid::IInputDeviceList) == nullptr);
}

REX_TEST_CASE("the family follows the USB vendor first and the driver's type second",
              "[hid][devices]") {
  CHECK(ClassifyPad(0x054C, PadType::kUnknown) == PadKind::kPlayStation);  // Sony
  CHECK(ClassifyPad(0x057E, PadType::kUnknown) == PadKind::kNintendo);     // Nintendo
  CHECK(ClassifyPad(0x28DE, PadType::kX360) == PadKind::kSteamDeck);       // Valve, even as XInput
  CHECK(ClassifyPad(0x045E, PadType::kUnknown) == PadKind::kXbox);         // Microsoft
  // Hardware of one vendor stays that vendor's, whatever the driver guessed.
  CHECK(ClassifyPad(0x054C, PadType::kX360) == PadKind::kPlayStation);
  // A pad from another vendor keeps what the driver recognised.
  CHECK(ClassifyPad(0x0F0D, PadType::kPS3) == PadKind::kPlayStation);
  CHECK(ClassifyPad(0x0F0D, PadType::kPS4) == PadKind::kPlayStation);
  CHECK(ClassifyPad(0x0F0D, PadType::kPS5) == PadKind::kPlayStation);
  CHECK(ClassifyPad(0x0F0D, PadType::kSwitchPro) == PadKind::kNintendo);
  CHECK(ClassifyPad(0x0F0D, PadType::kSwitchJoyCon) == PadKind::kNintendo);
  CHECK(ClassifyPad(0x0F0D, PadType::kX360) == PadKind::kXbox);
  CHECK(ClassifyPad(0x0F0D, PadType::kXOne) == PadKind::kXbox);
  CHECK(ClassifyPad(0, PadType::kX360) == PadKind::kXbox);  // the XInput driver knows no vendor
  CHECK(ClassifyPad(0x0F0D, PadType::kStandard) == PadKind::kGeneric);
  CHECK(ClassifyPad(0x0F0D, PadType::kUnknown) == PadKind::kGeneric);
  CHECK(ClassifyPad(0, PadType::kUnknown) == PadKind::kGeneric);
}

REX_TEST_CASE("with no pad the list is empty and reading the pads moves nothing",
              "[hid][devices]") {
  Run("none", [](rex::hid::IInputSystem& input) {
    rex::hid::IInputDeviceList& list = List();
    CHECK(list.DeviceGeneration() == 0);
    CHECK(list.Devices().empty());
    REQUIRE(input.GetState(0).has_value());  // the idle pad answers, and is not listed
    CHECK(list.DeviceGeneration() == 0);
    CHECK(list.Devices().empty());
  });
}

REX_TEST_CASE("the generation moves on connect, disconnect and a swap, and on nothing else",
              "[hid][devices]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    rex::hid::IInputDeviceList& list = List();
    const u32 start = list.DeviceGeneration();

    Plug(0, Pad(1, -1, "One"));
    const u32 one_pad = list.DeviceGeneration();
    CHECK(one_pad != start);

    // Input, capabilities and more reads leave it alone.
    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    REQUIRE(input.GetState(0).has_value());
    REQUIRE(input.GetCapabilities(0).has_value());
    CHECK(list.DeviceGeneration() == one_pad);

    // A driver event that leaves the pads as they were moves nothing.
    g_fake[0].revision++;
    CHECK(list.DeviceGeneration() == one_pad);

    Plug(0, Pad(2, -1, "Two"));
    const u32 two_pads = list.DeviceGeneration();
    CHECK(two_pads != one_pad);

    Unplug(0, 1);
    const u32 after_unplug = list.DeviceGeneration();
    CHECK(after_unplug != two_pads);

    // A swap that leaves the count alone is still a change.
    Unplug(0, 2);
    Plug(0, Pad(3, -1, "Three"));
    const u32 after_swap = list.DeviceGeneration();
    CHECK(after_swap != after_unplug);
    REQUIRE(list.Devices().size() == 1);
    CHECK(list.Devices()[0].name == "Three");

    Unplug(0, 3);
    CHECK(list.DeviceGeneration() != after_swap);
    CHECK(list.Devices().empty());
  });
}

REX_TEST_CASE("Devices carries the name, the USB ids, the family and the user", "[hid][devices]") {
  Run("fake_a", [](rex::hid::IInputSystem&) {
    Plug(0, Pad(1, -1, "DualSense Wireless Controller", 0x054C, 0x0CE6, PadType::kPS5));
    Plug(0, Pad(2, -1, "Xbox Wireless Controller", 0x045E, 0x02FD, PadType::kXOne));
    Plug(0, Pad(3, -1, "Some Pad", 0x1234, 0x5678));

    const std::vector<InputDeviceInfo> pads = List().Devices();
    REQUIRE(pads.size() == 3);

    CHECK(pads[0].name == "DualSense Wireless Controller");
    CHECK(pads[0].vendor_id == 0x054C);
    CHECK(pads[0].product_id == 0x0CE6);
    CHECK(pads[0].pad_kind == PadKind::kPlayStation);
    CHECK(pads[0].user == 0);

    CHECK(pads[1].name == "Xbox Wireless Controller");
    CHECK(pads[1].vendor_id == 0x045E);
    CHECK(pads[1].product_id == 0x02FD);
    CHECK(pads[1].pad_kind == PadKind::kXbox);
    CHECK(pads[1].user == 1);

    CHECK(pads[2].name == "Some Pad");
    CHECK(pads[2].pad_kind == PadKind::kGeneric);
    CHECK(pads[2].user == 2);
  });
}

REX_TEST_CASE("a fifth pad is listed with no user", "[hid][devices]") {
  Run("fake_a", [](rex::hid::IInputSystem&) {
    for (u64 id = 1; id <= 5; ++id) {
      Plug(0, Pad(id, -1, "Pad"));
    }
    const std::vector<InputDeviceInfo> pads = List().Devices();
    REQUIRE(pads.size() == 5);
    for (u32 user = 0; user < 4; ++user) {
      CHECK(pads[user].user == user);
    }
    CHECK(pads[4].user == InputDeviceInfo::kNoUser);
  });
}

REX_TEST_CASE("a second pad that joins user 0 moves the generation without a connection event",
              "[hid][devices]") {
  Run(
      "fake_a",
      [](rex::hid::IInputSystem& input) {
        std::vector<std::pair<u32, bool>> events;
        auto watch = input.OnConnectionChange(
            [&](u32 user, bool connected) { events.emplace_back(user, connected); });
        rex::hid::IInputDeviceList& list = List();

        Plug(0, Pad(1, -1, "One"));
        const u32 one_pad = list.DeviceGeneration();
        REQUIRE(events.size() == 1);
        CHECK(events[0] == std::pair<u32, bool>{0, true});

        Plug(0, Pad(2, -1, "Two"));
        const u32 two_pads = list.DeviceGeneration();
        CHECK(two_pads != one_pad);
        CHECK(events.size() == 1);  // user 0 was connected already: no event for the second pad

        const std::vector<InputDeviceInfo> pads = list.Devices();
        REQUIRE(pads.size() == 2);
        CHECK(pads[0].user == 0);
        CHECK(pads[1].user == 0);

        Unplug(0, 2);
        CHECK(list.DeviceGeneration() != two_pads);
        CHECK(events.size() == 1);
      },
      true);
}

REX_TEST_CASE("when the driver that served a pad loses it the other driver's copy takes over",
              "[hid][devices]") {
  Run("fake_a,fake_b", [](rex::hid::IInputSystem&) {
    rex::hid::IInputDeviceList& list = List();
    Plug(0, Pad(1, 0, "Pad from the first driver"));
    Plug(1, Pad(7, 0, "Pad from the second driver"));  // the same XInput slot: one physical pad

    std::vector<InputDeviceInfo> pads = list.Devices();
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].name == "Pad from the first driver");
    const u32 before = list.DeviceGeneration();

    Unplug(0, 1);
    pads = list.Devices();
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].name == "Pad from the second driver");
    CHECK(pads[0].user == 0);
    CHECK(list.DeviceGeneration() != before);
  });
}

REX_TEST_CASE("the keyboard and mouse pad is not listed and does not move the generation",
              "[hid][devices]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    rex::hid::IInputDeviceList& list = List();
    Plug(0, Pad(1, -1, "One"));
    const u32 before = list.DeviceGeneration();
    REQUIRE(list.Devices().size() == 1);

    REQUIRE(input.GetSettings().mnk_mode.Set(true));
    CHECK(list.DeviceGeneration() == before);
    CHECK(list.Devices().size() == 1);

    REQUIRE(input.GetSettings().mnk_passthrough.Set(true));
    CHECK(list.DeviceGeneration() == before);
    CHECK(list.Devices().size() == 1);
  });
}
