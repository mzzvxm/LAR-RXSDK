/**
 * @file        tests/unit/xam/relaunch_test.cpp
 * @brief       A title that starts itself again through XamLoaderLaunchTitle
 *
 * The export keeps the launch flags and the launch data in a sidecar file next to the executable
 * and reports TitleExitReason::kRelaunch, so the app starts the process again; the next title
 * attach reads the sidecar back, once. These tests cover both halves.
 *
 * The sidecar path is the folder of the test executable, which no test can change. A test process
 * that runs while another one attaches a title could see the file, so the tests keep the time
 * between writing and reading it as short as they can.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "rex_test.h"

#include <rex/kernel/title_exit.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/file.h>

#include "kernel/kernel_fixture.h"
#include "xam/xam_relaunch.h"
#include "xam/xam_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;
using rex::xam::XAMLoaderState;

namespace {

// CHECK: mirrors RelaunchSidecarPath in src/xam/xam_info.cpp; the name or the folder changing there
// has to change here.
std::filesystem::path Sidecar() {
  return rex::platform::GetExecutableFolder() / ".rex_relaunch";
}

void RemoveSidecar() {
  std::error_code ec;
  std::filesystem::remove(Sidecar(), ec);
}

}  // namespace

REX_TEST_CASE("XAM: a title that starts itself again reports a relaunch",
              "[kernel][xam][relaunch]") {
  static std::atomic<int> g_reported;
  static std::atomic<bool> g_returned;
  g_reported = -1;
  g_returned = false;
  RemoveSidecar();
  KernelFixture fixture;
  auto exit_guard = fixture.GetRuntime().OnTitleExit([](rex::kernel::TitleExitReason reason) {
    g_reported = static_cast<int>(reason);
  });
  fixture.RunUntilGuestThreadEnds([](Caller& c) {
    const rex::u32 data = c.AllocString("boot twice");
    c.XAM("XamLoaderSetLaunchData", {data, 10});
    c.XAM("XamLoaderLaunchTitle", {0, 0x2A});
    g_returned = true;
  });
  CHECK_FALSE(g_returned);
  CHECK(g_reported == static_cast<int>(rex::kernel::TitleExitReason::kRelaunch));
  RemoveSidecar();
}

REX_TEST_CASE("The launch data of a title that starts itself again survives the reboot",
              "[xam][relaunch]") {
  RemoveSidecar();
  XAMLoaderState before;
  before.launch_data_present = true;
  before.launch_flags = 0x2A;
  before.launch_data = {1, 2, 3, 4, 5};
  rex::xam::StoreRelaunchData(before);

  XAMLoaderState after;
  rex::xam::RestoreRelaunchData(after);
  CHECK(after.launch_data_present);
  CHECK(after.launch_flags == 0x2A);
  CHECK(after.launch_data == before.launch_data);

  // A relaunch happens at most once: the sidecar is gone after the first read.
  CHECK_FALSE(std::filesystem::exists(Sidecar()));
  XAMLoaderState again;
  rex::xam::RestoreRelaunchData(again);
  CHECK_FALSE(again.launch_data_present);
  CHECK(again.launch_data.empty());
}

REX_TEST_CASE("A title that started itself again with no launch data still finds launch data",
              "[xam][relaunch]") {
  RemoveSidecar();
  XAMLoaderState before;
  before.launch_flags = 7;
  rex::xam::StoreRelaunchData(before);

  XAMLoaderState after;
  rex::xam::RestoreRelaunchData(after);
  // The title did start itself again, so XamLoaderGetLaunchData answers even for 0 bytes.
  CHECK(after.launch_data_present);
  CHECK(after.launch_flags == 7);
  CHECK(after.launch_data.empty());
}

REX_TEST_CASE("Without a sidecar the launch state stays as it is", "[xam][relaunch]") {
  RemoveSidecar();
  XAMLoaderState state;
  state.launch_flags = 3;
  rex::xam::RestoreRelaunchData(state);
  CHECK_FALSE(state.launch_data_present);
  CHECK(state.launch_flags == 3);
}

REX_TEST_CASE("A malformed sidecar is ignored and removed", "[xam][relaunch]") {
  RemoveSidecar();
  {
    std::ofstream out(Sidecar(), std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << "xyz";  // shorter than the flags and the size
  }
  XAMLoaderState state;
  rex::xam::RestoreRelaunchData(state);
  CHECK_FALSE(state.launch_data_present);
  CHECK_FALSE(std::filesystem::exists(Sidecar()));
}
