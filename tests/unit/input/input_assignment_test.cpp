#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified, so
// FakeDriver cannot match the base class signatures without them.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::ClassifyPad;
using rex::input::DeviceAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::PadKind;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> For(const DeviceAssignment& a, uint32_t user) {
  std::vector<DeviceId> out;
  a.DevicesForUser(user, out);
  return out;
}

/// Exercises assignment with no SDL, window, or OS behind it.
class FakeDriver final : public InputDriver {
 public:
  FakeDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Add(uint64_t id, bool synthetic = false) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  /// What a real driver would put in the DeviceInfo it enumerates.
  void Describe(uint64_t id, uint16_t vendor, uint16_t product, PadKind kind) {
    for (auto& info : devices_) {
      if (info.id == static_cast<DeviceId>(id)) {
        info.vendor_id = vendor;
        info.product_id = product;
        info.pad_kind = kind;
      }
    }
  }

  void Remove(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(uint64_t id, uint16_t buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void QueueKeystroke(uint64_t id, uint16_t vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices_; }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    // Stamp the id so the test can tell which device answered.
    out->sub_type = static_cast<uint8_t>(id);
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return states_.count(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return X_ERROR_EMPTY;
    }
    *out = it->second;
    return X_ERROR_SUCCESS;
  }

 private:
  std::vector<DeviceInfo> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
};

std::unique_ptr<InputSystem> SharedSystem(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

}  // namespace

TEST_CASE("SharedAssignment puts every device on user 0", "[input]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(For(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(For(shared, 1).empty());
}

TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[input]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(For(slots, 0).empty());
  REQUIRE(For(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

TEST_CASE("Both controllers reach one guest user", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("Unplugging one controller leaves the other working", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(1);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Capabilities follow the controller in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(caps.sub_type == 2);
}

TEST_CASE("A keystroke from the second controller reaches the guest", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(ks.virtual_key) == 0x5800);
}

TEST_CASE("A reconnected pad comes back to guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // Replugging arrives as a new device id, not as the old one coming back.
  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A synthetic device does not push the first pad off guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The NOP stand-in is always present and enumerates before any pad connects.
  driver->Add(1, true);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("The USB vendor decides the controller family, the driver's guess settles the rest",
          "[input]") {
  REQUIRE(ClassifyPad(0x054C, PadKind::kGeneric) == PadKind::kPlayStation);  // Sony
  REQUIRE(ClassifyPad(0x057E, PadKind::kGeneric) == PadKind::kSwitch);       // Nintendo
  REQUIRE(ClassifyPad(0x28DE, PadKind::kXbox) == PadKind::kSteamDeck);  // Valve, even as XInput
  REQUIRE(ClassifyPad(0x045E, PadKind::kGeneric) == PadKind::kXbox);    // Microsoft
  // Sony hardware that reports another type is still a Sony pad.
  REQUIRE(ClassifyPad(0x054C, PadKind::kXbox) == PadKind::kPlayStation);
  // A clone from another vendor keeps what the driver recognised.
  REQUIRE(ClassifyPad(0x0F0D, PadKind::kPlayStation) == PadKind::kPlayStation);
  REQUIRE(ClassifyPad(0x0F0D, PadKind::kSwitch) == PadKind::kSwitch);
  REQUIRE(ClassifyPad(0x0F0D, PadKind::kGeneric) == PadKind::kGeneric);
  REQUIRE(ClassifyPad(0, PadKind::kXbox) == PadKind::kXbox);  // vendor unknown
}

TEST_CASE("The device generation moves when a device is added or removed, not otherwise",
          "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  X_INPUT_STATE state = {};

  const uint32_t start = system->device_generation();
  REQUIRE(system->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(system->device_generation() == start);  // nothing there, nothing changed

  driver->Add(1);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  const uint32_t one_pad = system->device_generation();
  REQUIRE(one_pad != start);

  // Input, capabilities and more polls leave it alone.
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(system->device_generation() == one_pad);

  driver->Add(2);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  const uint32_t two_pads = system->device_generation();
  REQUIRE(two_pads != one_pad);

  driver->Remove(1);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  const uint32_t after_remove = system->device_generation();
  REQUIRE(after_remove != two_pads);

  // A swap that leaves the count alone is still a change.
  driver->Remove(2);
  driver->Add(3);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->device_generation() != after_remove);
}

TEST_CASE("Devices() carries the vendor, product and family the driver reported", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Describe(1, 0x054C, 0x0CE6, PadKind::kPlayStation);
  driver->Add(2);  // a driver that knows nothing

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  const std::vector<DeviceInfo> devices = system->Devices();
  REQUIRE(devices.size() == 2);
  REQUIRE(devices[0].vendor_id == 0x054C);
  REQUIRE(devices[0].product_id == 0x0CE6);
  REQUIRE(devices[0].pad_kind == PadKind::kPlayStation);
  REQUIRE(devices[1].vendor_id == 0);
  REQUIRE(devices[1].pad_kind == PadKind::kGeneric);
}

TEST_CASE("ActiveDevice follows the last device with real input", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Describe(1, 0x045E, 0x028E, PadKind::kXbox);
  driver->Add(2);
  driver->Describe(2, 0x054C, 0x0CE6, PadKind::kPlayStation);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0) == nullptr);  // nobody has touched anything yet

  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0) != nullptr);
  REQUIRE(system->ActiveDevice(0)->id == static_cast<DeviceId>(1));
  REQUIRE(system->ActiveDevice(0)->pad_kind == PadKind::kXbox);

  // The other pad is picked up: it takes over, with its own family.
  driver->SetButtons(1, 0);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0)->id == static_cast<DeviceId>(2));
  REQUIRE(system->ActiveDevice(0)->pad_kind == PadKind::kPlayStation);

  // Letting go does not hand it back: neutral never takes over.
  driver->SetButtons(2, 0);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0)->id == static_cast<DeviceId>(2));

  // Other users and out-of-range users have none.
  REQUIRE(system->ActiveDevice(1) == nullptr);
  REQUIRE(system->ActiveDevice(rex::input::kMaxGuestUsers) == nullptr);
}

TEST_CASE("ActiveDevice is cleared when its device is unplugged", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0)->id == static_cast<DeviceId>(2));

  driver->Remove(2);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(system->ActiveDevice(0) == nullptr);  // the survivor is idle: no claim to the slot
  REQUIRE(system->Devices().size() == 1);
}
