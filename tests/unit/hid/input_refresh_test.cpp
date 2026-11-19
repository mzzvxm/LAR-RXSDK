/**
 * @file        tests/unit/hid/input_refresh_test.cpp
 * @brief       The input system lists the devices of its drivers only after a driver's revision
 *              moves, not on every call the title makes.
 *
 * The title asks about every user several times a frame (capabilities, state, vibration, keystroke),
 * and a device list almost never changes. Listing the devices means asking every driver, and the
 * SDL driver takes SDL's global joystick lock for each pad, so doing it on every call cost the
 * session real time (about eight listings a frame on the v0.10 line, which the held
 * perf/input-refresh-on-change branch removed). v0.11 has the same rule in its own form: a driver
 * answers IInputDriver::GetDeviceRevision, and InputSystem lists the devices when a revision moved.
 * These tests keep that rule from slipping back to a listing per call.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "input_harness.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <rex/hid/pad.h>

using namespace rex::test_input;

namespace {

constexpr rex::hid::KeystrokeSource kAllSources =
    rex::hid::KeystrokeSource::kPad | rex::hid::KeystrokeSource::kKeyboard;

/** Plugs a pad into a fake driver and moves the driver's revision, as a real driver does. */
void Plug(int driver, u64 id, u16 buttons = 0) {
  g_fake[driver].devices.push_back(MakePad(id, -1));
  g_fake[driver].states[id] = Buttons(buttons);
  g_fake[driver].revision++;
}

void Unplug(int driver, u64 id) {
  std::erase_if(g_fake[driver].devices,
                [&](const rex::hid::PadDevice& device) { return device.id.value == id; });
  g_fake[driver].states.erase(id);
  g_fake[driver].revision++;
}

/** What the title does each frame: ask about every user through every call it has. */
void GuestFrame(rex::hid::IInputSystem& input) {
  for (u32 user = 0; user < rex::hid::IInputSystem::kMaxUsers; ++user) {
    [[maybe_unused]] auto capabilities = input.GetCapabilities(user);
    [[maybe_unused]] auto reading = input.GetState(user);
    [[maybe_unused]] auto vibration = input.SetVibration(user, {});
    [[maybe_unused]] auto keystroke = input.GetKeystroke(user, kAllSources);
  }
}

}  // namespace

REX_TEST_CASE("a driver is listed only after its revision moves", "[hid][refresh]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    Plug(0, 1);

    GuestFrame(input);
    REQUIRE(g_fake[0].enumerations == 1);  // the first call has to look

    for (int frame = 0; frame < 100; ++frame) {
      GuestFrame(input);
    }
    CHECK(g_fake[0].enumerations == 1);  // 1600 more calls, no change, no listing

    Plug(0, 2);
    GuestFrame(input);
    CHECK(g_fake[0].enumerations == 2);  // one look for the change, not one per call
    GuestFrame(input);
    CHECK(g_fake[0].enumerations == 2);
  });
}

REX_TEST_CASE("every driver stays unlisted while none of their revisions moves", "[hid][refresh]") {
  Run("fake_a,fake_b", [](rex::hid::IInputSystem& input) {
    Plug(0, 1);
    Plug(1, 7);
    GuestFrame(input);
    REQUIRE(g_fake[0].enumerations == 1);
    REQUIRE(g_fake[1].enumerations == 1);

    for (int frame = 0; frame < 100; ++frame) {
      GuestFrame(input);
    }
    CHECK(g_fake[0].enumerations == 1);
    CHECK(g_fake[1].enumerations == 1);

    // A pad that the second driver adds is the third user's, found by the next call.
    Plug(1, 8, rex::hid::kPadA);
    auto third = input.GetState(2);
    REQUIRE(third.has_value());
    CHECK(third->state.buttons == rex::hid::kPadA);
  });
}

REX_TEST_CASE("a pad that comes or goes is seen on the very next call", "[hid][refresh]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    Plug(0, 1);
    REQUIRE(input.GetState(0).has_value());
    CHECK_FALSE(input.GetState(1).has_value());

    Plug(0, 2, rex::hid::kPadA);
    auto second = input.GetState(1);
    REQUIRE(second.has_value());
    CHECK(second->state.buttons == rex::hid::kPadA);

    Unplug(0, 2);
    CHECK_FALSE(input.GetState(1).has_value());

    Unplug(0, 1);
    // User 0 always has a pad, the neutral one, once the real pad is gone.
    auto idle = input.GetState(0);
    REQUIRE(idle.has_value());
    CHECK(idle->state.buttons == 0);
  });
}

REX_TEST_CASE("a pad's input changes do not list the devices", "[hid][refresh]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    Plug(0, 1);
    REQUIRE(input.GetState(0).has_value());
    const u32 baseline = g_fake[0].enumerations;

    g_fake[0].states[1] = Buttons(rex::hid::kPadA);
    auto reading = input.GetState(0);
    REQUIRE(reading.has_value());
    CHECK(reading->state.buttons == rex::hid::kPadA);
    CHECK(g_fake[0].enumerations == baseline);
  });
}

REX_TEST_CASE("a revision that moves with the same pads is one listing and no event",
              "[hid][refresh]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    std::vector<std::pair<u32, bool>> events;
    auto watch = input.OnConnectionChange(
        [&](u32 user, bool connected) { events.emplace_back(user, connected); });
    Plug(0, 1);
    REQUIRE(input.GetState(0).has_value());
    REQUIRE(events.size() == 1);
    const u32 baseline = g_fake[0].enumerations;

    g_fake[0].revision++;  // moves, and the list is the same
    GuestFrame(input);
    GuestFrame(input);
    CHECK(g_fake[0].enumerations == baseline + 1);
    CHECK(events.size() == 1);
  });
}

REX_TEST_CASE("ordinals follow connection order when changes arrive one at a time",
              "[hid][refresh]") {
  Run("fake_a", [](rex::hid::IInputSystem& input) {
    Plug(0, 1, rex::hid::kPadA);
    REQUIRE(input.GetState(0).has_value());
    Plug(0, 2, rex::hid::kPadB);
    auto second = input.GetState(1);
    REQUIRE(second.has_value());
    CHECK(second->state.buttons == rex::hid::kPadB);

    // Pad 1 leaves: pad 2 keeps user 1, and user 0 falls back to the neutral pad.
    Unplug(0, 1);
    auto neutral = input.GetState(0);
    REQUIRE(neutral.has_value());
    CHECK(neutral->state.buttons == 0);
    auto kept = input.GetState(1);
    REQUIRE(kept.has_value());
    CHECK(kept->state.buttons == rex::hid::kPadB);

    // The next pad takes the lowest free user, 0.
    Plug(0, 3, rex::hid::kPadX);
    auto first = input.GetState(0);
    REQUIRE(first.has_value());
    CHECK(first->state.buttons == rex::hid::kPadX);
  });
}
