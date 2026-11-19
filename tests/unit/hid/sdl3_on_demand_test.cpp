/**
 * @file        tests/unit/hid/sdl3_on_demand_test.cpp
 * @brief       The SDL3 input driver reads the joysticks from the title's calls, not from SDL's
 *              event loop on the UI thread.
 *
 * With SDL's own joystick update on, SDL wakes the UI thread every millisecond for as long as a pad
 * is open (SDL_events.c, EVENT_POLL_INTERVAL_NS), to serve a title that looks at the pad once a
 * frame. The driver switches that update off (SDL_HINT_AUTO_UPDATE_JOYSTICKS) and calls
 * SDL_UpdateJoysticks on the calling thread, at most once a millisecond. These tests drive the
 * driver with an SDL virtual gamepad and nothing that pumps SDL's events: whatever the driver
 * reads, it reads from its own calls.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/utility.h>

#include "base/module_group.h"
#include "base/module_registration.h"

namespace {

constexpr const char* kVirtualPadName = "ReXGlue on-demand test pad";

/** Runs the UI thread's work on the calling thread and keeps what was posted for the test to see. */
class TrackingContext final : public rex::IDriverContext {
 public:
  rex::ISettingsRegistry& GetSettingsRegistry() override { return REX_SETTINGS(); }
  rex::ILogger& GetLogger() override { return REX_LOGGER(); }
  rex::Result<void> RunOnUIThread(rex::MoveOnlyFunction<void()> fn) override {
    fn();
    return rex::Ok();
  }
  void PostToUIThread(rex::MoveOnlyFunction<void()> fn) override {
    posted.push_back(std::move(fn));
  }

  void RunPosted() {
    std::vector<rex::MoveOnlyFunction<void()>> run;
    run.swap(posted);
    for (auto& fn : run) {
      fn();
    }
  }

  std::vector<rex::MoveOnlyFunction<void()>> posted;
};

struct Fixture {
  Fixture() {
    REQUIRE(group.Load("rexhid-sdl3").has_value());
    REQUIRE(group.Connect().has_value());
  }

  std::unique_ptr<rex::hid::IInputDriver> Create() {
    for (const rex::DriverRegistration* registration :
         rex::test::GetRegistrations(group, rex::DriverType::kInput)) {
      if (registration->name == "sdl3") {
        std::unique_ptr<rex::IDriver> driver = registration->create();
        return std::unique_ptr<rex::hid::IInputDriver>(
            static_cast<rex::hid::IInputDriver*>(driver.release()));
      }
    }
    return nullptr;
  }

  rex::ModuleGroup group;
};

/** A gamepad that SDL makes up, with the south button on the first button. Detached on the way out. */
struct VirtualPad {
  VirtualPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = kVirtualPadName;
    id = SDL_AttachVirtualJoystick(&desc);
  }

  ~VirtualPad() {
    if (stick) {
      SDL_CloseJoystick(stick);
    }
    if (id) {
      SDL_DetachVirtualJoystick(id);
    }
  }

  /** Presses or releases the south button; SDL applies it at its next joystick update. */
  bool SetSouth(bool down) {
    if (!stick) {
      stick = SDL_OpenJoystick(id);
    }
    return stick && SDL_SetJoystickVirtualButton(stick, 0, down);
  }

  SDL_JoystickID id = 0;
  SDL_Joystick* stick = nullptr;
};

/** Puts the hint back to SDL's default when the test is over, whatever it set. */
struct HintReset {
  ~HintReset() { SDL_ResetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS); }
};

bool FindVirtualPad(rex::hid::IInputDriver& driver, rex::hid::PadDevice& out) {
  std::vector<rex::hid::PadDevice> devices;
  driver.EnumerateDevices(devices);
  const auto found = std::ranges::find_if(
      devices, [](const rex::hid::PadDevice& device) {
        return device.name.find(kVirtualPadName) != std::string::npos;
      });
  if (found == devices.end()) {
    return false;
  }
  out = *found;
  return true;
}

/** Longer than the one millisecond the driver lets pass between two reads of the joysticks. */
void LetTheReadGapPass() {
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "sdl3 reads a pad from the title's calls and posts nothing to the UI",
                     "[hid][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  TrackingContext context;
  REQUIRE(driver->Initialize(context).has_value());
  HintReset hint_reset;

  // SDL_AUTO_UPDATE_JOYSTICKS in the environment outranks the driver's hint, and macOS keeps SDL's
  // own update: in both the driver reads the old way, and the other test covers that.
  if (SDL_GetHintBoolean(SDL_HINT_AUTO_UPDATE_JOYSTICKS, true)) {
    driver->Shutdown();
    context.RunPosted();
    SKIP("SDL's own joystick update is on: SDL_AUTO_UPDATE_JOYSTICKS is set, or this is macOS");
  }

  const u64 before = driver->GetDeviceRevision();
  {
    VirtualPad pad;
    REQUIRE(pad.id != 0);

    // The arrival reached the driver without anyone pumping SDL's events.
    CHECK(driver->GetDeviceRevision() > before);
    rex::hid::PadDevice listed;
    REQUIRE(FindVirtualPad(*driver, listed));

    CHECK(driver->GetDeviceCapabilities(listed.id).has_value());
    REQUIRE(pad.SetSouth(true));
    LetTheReadGapPass();
    auto pressed = driver->GetDeviceState(listed.id);
    REQUIRE(pressed.has_value());
    CHECK((pressed->buttons & rex::hid::kPadA) != 0);

    REQUIRE(pad.SetSouth(false));
    LetTheReadGapPass();
    auto released = driver->GetDeviceState(listed.id);
    REQUIRE(released.has_value());
    CHECK((released->buttons & rex::hid::kPadA) == 0);
  }

  // The removal is seen too, and the driver let go of the pad.
  rex::hid::PadDevice gone;
  CHECK_FALSE(FindVirtualPad(*driver, gone));

  // Not one function was handed to the UI thread for all of that.
  CHECK(context.posted.empty());

  driver->Shutdown();
  context.RunPosted();
}

REX_TEST_CASE_METHOD(Fixture, "sdl3 keeps SDL's own update when something outranks its hint",
                     "[hid][sdl3]") {
  // The environment variable SDL_AUTO_UPDATE_JOYSTICKS=1 outranks the driver's hint in the same
  // way (SDL's rule for hints). It is the "before" side of a measurement in the same build.
  HintReset hint_reset;
  REQUIRE(SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1", SDL_HINT_OVERRIDE));

  auto driver = Create();
  REQUIRE(driver);
  TrackingContext context;
  REQUIRE(driver->Initialize(context).has_value());
  CHECK(SDL_GetHintBoolean(SDL_HINT_AUTO_UPDATE_JOYSTICKS, false));

  // The old way: each call asks the UI thread to pump SDL, one pending request at a time.
  [[maybe_unused]] const u64 revision = driver->GetDeviceRevision();
  CHECK(context.posted.size() == 1);
  [[maybe_unused]] const u64 again = driver->GetDeviceRevision();
  CHECK(context.posted.size() == 1);
  context.RunPosted();
  [[maybe_unused]] const u64 next = driver->GetDeviceRevision();
  CHECK(context.posted.size() == 1);

  driver->Shutdown();
  context.RunPosted();
}
