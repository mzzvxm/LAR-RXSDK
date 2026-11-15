#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

/// A driver that counts how often InputSystem enumerates it. With `tracks_changes` it reports a
/// generation the way the SDL driver does (moved by Plug/Unplug after the list is updated);
/// without, it gives no answer, like the XInput driver that asks the OS inside EnumerateDevices.
class CountingDriver final : public InputDriver {
 public:
  explicit CountingDriver(bool tracks_changes) : InputDriver(nullptr, 0), tracks_(tracks_changes) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Plug(uint64_t id, bool synthetic = false) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    devices_.push_back(info);
    buttons_.push_back({id, 0});
    generation_++;
  }

  void Unplug(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    std::erase_if(buttons_, [&](const auto& b) { return b.first == id; });
    generation_++;
  }

  /// Changes a pad's input only: no list change, so no generation move.
  void Press(uint64_t id, uint16_t buttons) {
    for (auto& b : buttons_) {
      if (b.first == id) {
        b.second = buttons;
      }
    }
  }

  /// A change the driver cannot report (a list edit without a generation move), to prove that a
  /// tracking driver is trusted: InputSystem must not look.
  void PlugWithoutAnnouncing(uint64_t id) {
    const uint64_t before = generation_;
    Plug(id);
    generation_ = before;
  }

  int enumerations() const { return enumerations_; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    enumerations_++;
    out = devices_;
  }

  std::optional<uint64_t> DeviceListGeneration() const override {
    return tracks_ ? std::optional<uint64_t>(generation_) : std::nullopt;
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    for (const auto& b : buttons_) {
      if (static_cast<DeviceId>(b.first) == id) {
        *out_state = {};
        out_state->gamepad.buttons = b.second;
        return X_ERROR_SUCCESS;
      }
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    X_INPUT_STATE unused = {};
    if (GetDeviceState(id, &unused) != X_ERROR_SUCCESS) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }

  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }

 private:
  bool tracks_;
  uint64_t generation_ = 0;
  int enumerations_ = 0;
  std::vector<DeviceInfo> devices_;
  std::vector<std::pair<uint64_t, uint16_t>> buttons_;
};

struct Rig {
  CountingDriver* driver = nullptr;
  std::unique_ptr<InputSystem> system;
};

Rig MakeRig(bool tracks_changes) {
  Rig rig;
  auto driver = std::make_unique<CountingDriver>(tracks_changes);
  rig.driver = driver.get();
  rig.system = std::make_unique<InputSystem>(nullptr);
  rig.system->AddDriver(std::move(driver));
  rig.system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return rig;
}

/// What the guest does each frame: ask about every user.
void GuestFrame(InputSystem& system) {
  X_INPUT_STATE state = {};
  X_INPUT_CAPABILITIES caps = {};
  for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; user++) {
    system.GetCapabilities(user, 0, &caps);
    system.GetState(user, &state);
  }
}

}  // namespace

TEST_CASE("A driver that reports its list changes is enumerated only after a change", "[input]") {
  Rig rig = MakeRig(true);
  rig.driver->Plug(1);

  GuestFrame(*rig.system);
  REQUIRE(rig.driver->enumerations() == 1);  // the first call has to look

  for (int frame = 0; frame < 100; frame++) {
    GuestFrame(*rig.system);
  }
  REQUIRE(rig.driver->enumerations() == 1);  // 800 more calls, no change, no enumeration

  rig.driver->Plug(2);
  GuestFrame(*rig.system);
  REQUIRE(rig.driver->enumerations() == 2);  // one look for the change, not one per call
  GuestFrame(*rig.system);
  REQUIRE(rig.driver->enumerations() == 2);
}

TEST_CASE("The new device is seen on the very next call after it is announced", "[input]") {
  Rig rig = MakeRig(true);
  rig.driver->Plug(1);
  X_INPUT_STATE state = {};
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);

  rig.driver->Plug(2);
  rig.driver->Press(2, X_INPUT_GAMEPAD_A);
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);

  rig.driver->Unplug(2);
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == 0);  // pad 2 is gone, only pad 1 is left

  rig.driver->Unplug(1);
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("Input changes do not make InputSystem enumerate", "[input]") {
  Rig rig = MakeRig(true);
  rig.driver->Plug(1);
  X_INPUT_STATE state = {};
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  const int baseline = rig.driver->enumerations();

  rig.driver->Press(1, X_INPUT_GAMEPAD_A);
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(rig.driver->enumerations() == baseline);
}

TEST_CASE("A driver that cannot report changes is enumerated on every call, as before", "[input]") {
  Rig rig = MakeRig(false);
  rig.driver->Plug(1);

  GuestFrame(*rig.system);
  const int after_first_frame = rig.driver->enumerations();
  REQUIRE(after_first_frame == static_cast<int>(2 * rex::input::kMaxGuestUsers));

  // A list change with no announcement is still found at once.
  rig.driver->PlugWithoutAnnouncing(2);
  rig.driver->Press(2, X_INPUT_GAMEPAD_A);
  X_INPUT_STATE state = {};
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(rig.driver->enumerations() == after_first_frame + 1);
}

TEST_CASE("One silent driver keeps the whole system enumerating", "[input]") {
  auto tracking = std::make_unique<CountingDriver>(true);
  auto silent = std::make_unique<CountingDriver>(false);
  CountingDriver* tracking_ptr = tracking.get();
  CountingDriver* silent_ptr = silent.get();
  InputSystem system(nullptr);
  system.AddDriver(std::move(tracking));
  system.AddDriver(std::move(silent));
  system.SetDeviceAssignment(std::make_unique<SharedAssignment>());
  tracking_ptr->Plug(1);

  X_INPUT_STATE state = {};
  for (int call = 0; call < 10; call++) {
    system.GetState(0, &state);
  }
  // Both are asked each time, because the silent one has to be.
  REQUIRE(silent_ptr->enumerations() == 10);
  REQUIRE(tracking_ptr->enumerations() == 10);
}

TEST_CASE("Adding a driver after polling started makes the next call look again", "[input]") {
  Rig rig = MakeRig(true);
  rig.driver->Plug(1);
  X_INPUT_STATE state = {};
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(rig.driver->enumerations() == 1);

  auto late = std::make_unique<CountingDriver>(true);
  CountingDriver* late_ptr = late.get();
  late_ptr->Plug(7);
  late_ptr->Press(7, X_INPUT_GAMEPAD_A);
  rig.system->AddDriver(std::move(late));
  // The shared assignment puts the new pad on user 0 with the first one.
  REQUIRE(rig.system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(late_ptr->enumerations() == 1);
}

TEST_CASE("Ordinals still follow connection order when changes arrive one at a time", "[input]") {
  auto driver = std::make_unique<CountingDriver>(true);
  CountingDriver* d = driver.get();
  InputSystem system(nullptr);
  system.AddDriver(std::move(driver));
  system.SetDeviceAssignment(std::make_unique<SlotAssignment>());

  X_INPUT_STATE state = {};
  d->Plug(1);
  d->Press(1, X_INPUT_GAMEPAD_A);
  REQUIRE(system.GetState(0, &state) == X_ERROR_SUCCESS);
  d->Plug(2);
  d->Press(2, X_INPUT_GAMEPAD_A);
  REQUIRE(system.GetState(1, &state) == X_ERROR_SUCCESS);

  // Pad 1 leaves: pad 2 keeps guest user 1, and user 0 is empty.
  d->Unplug(1);
  REQUIRE(system.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(system.GetState(1, &state) == X_ERROR_SUCCESS);

  // The next pad takes the lowest free ordinal, user 0.
  d->Plug(3);
  d->Press(3, X_INPUT_GAMEPAD_A);
  REQUIRE(system.GetState(0, &state) == X_ERROR_SUCCESS);
}
