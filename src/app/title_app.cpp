/**
 * @remarks     Derived from Xenia's ui/windowed_app (Ben Vanik, 2020).
 */

#include <rex/app/title_app.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <rex/logging/log_category.h>
#include <rex/path_settings.h>
#include <rex/platform.h>
#include <rex/platform/file.h>
#include <rex/platform/process.h>
#include <rex/settings_registry.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/title_exit.h>
#include <rex/ui/ui_system.h>
#include <rex/xam/content_manager.h>

#include "app/lifecycle.h"
#include "ui/ui_control.h"

#if REX_PLATFORM_WIN32
#include <objbase.h>
#include <shellapi.h>
#endif

namespace rex {
namespace app::detail {

#if REX_PLATFORM_WIN32
bool InitializeCOM() {
  return SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

void ShutdownCOM() {
  CoUninitialize();
}
#else
bool InitializeCOM() {
  return false;
}

void ShutdownCOM() {}
#endif

}  // namespace app::detail
}  // namespace rex

namespace rex::app {

namespace {

std::string ExitMessage(rex::kernel::TitleExitReason reason) {
  switch (reason) {
    case rex::kernel::TitleExitReason::kNormalExit:
      return "The title has exited.";
    case rex::kernel::TitleExitReason::kHardExit:
      return "The title returned to the dashboard.";
    case rex::kernel::TitleExitReason::kTitleDataError:
      return "The title could not read its data from the disc.";
    case rex::kernel::TitleExitReason::kBugCheck:
      return "The title stopped with a system error.";
    case rex::kernel::TitleExitReason::kRelaunch:
      return "The title started itself again.";
  }
  return "The title has exited.";
}

/** The longest profile name, the same as rex::xam::UserProfile::kMaxNameLength. */
constexpr u32 kUserNameMaxLength = 15;

/**
 * Keeps what a profile name can hold: no control characters and none of the characters a folder
 * name cannot have on any host, no leading spaces, no trailing dots or spaces, at most
 * kUserNameMaxLength characters. The XAM module sanitizes the name again when it uses it.
 */
std::string CleanUserName(std::string_view raw) {
  std::string name;
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F || std::strchr("<>:\"/\\|?*", c) != nullptr) {
      continue;
    }
    if (name.empty() && c == ' ') {
      continue;
    }
    if (name.size() >= kUserNameMaxLength) {
      break;
    }
    name.push_back(c);
  }
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }
  return name;
}

/** What one profile name prompt answered. Shared with the prompt, which may outlive the call. */
struct NamePrompt {
  bool done = false;
  rex::ui::PromptResult result;
};

/**
 * CHECK: not run with a real window. Only an app that turns AsksForUserName on gets here, because
 * the prompt waits for an answer. It is drawn only if the graphics system, or the program's own
 * renderer when the program needs no driver, draws the overlays while no title runs (the kRunning
 * phase).
 *
 * First launch: asks for the profile name that the title sees and that names the save folder. It
 * runs before the title loads, because loading creates the profile and its content folders. Every
 * frame of the app keeps running while the prompt is up.
 *
 * @return False when the app is shutting down before a name was chosen.
 */
bool AskForUserName(TitleApp& app, rex::kernel::TitleRuntime& title) {
  rex::kernel::KernelSettings& settings = title.GetKernelSettings();
  rex::ui::IUISystem& ui = app.GetUI();
  if (!settings.user_name.Get().empty() || settings.headless.Get() || !ui.HasWindow()) {
    return true;
  }
  std::string note;
  for (;;) {
    auto prompt = std::make_shared<NamePrompt>();
    ui.ShowPrompt(
        {.title = "Choose a profile name",
         .body = std::string("This name is shown in-game and names your save folder. You can "
                             "change it later in Settings; existing saves move with it.") +
                 note,
         .buttons = {"Continue"},
         .text_input = true,
         .default_text = {},
         .max_length = kUserNameMaxLength},
        [prompt](rex::ui::PromptResult result) {
          prompt->result = std::move(result);
          prompt->done = true;
        });
    while (!prompt->done && app.RunFrame()) {}
    if (!prompt->done || prompt->result.canceled) {
      return false;
    }
    const std::string name = CleanUserName(prompt->result.text);
    if (name.empty()) {
      note = "\n\nEnter at least one usable character.";
      continue;
    }
    if (auto set = settings.user_name.SetText(rex::SettingLayer::kRuntime, name); !set) {
      REXLOG_WARN("the profile name could not be set, {}", set.error().GetDescription());
      return true;
    }
    if (const std::filesystem::path config = REX_PATHS().config_file.Get(); !config.empty()) {
      if (auto saved = REX_SETTINGS().Save(config); !saved) {
        REXLOG_WARN("the profile name was not saved to the config file, {}",
                    saved.error().GetDescription());
      }
    }
    REXLOG_INFO("Profile name set to '{}'", name);
    return true;
  }
}

}  // namespace

struct TitleApp::State {
  explicit State(const ppc::PPCImage& image) : runtime(image) {}

  rex::kernel::TitleRuntime runtime;
  ScopeGuard exit_guard;
  bool com_initialized = false;
  bool unloading = false;
  bool stopped = false;
};

TitleApp::TitleApp(std::string_view app_name, const ppc::PPCImage& image)
    : App(app_name), state_(std::make_unique<State>(image)) {
  state_->com_initialized = ::rex::app::detail::InitializeCOM();
}

TitleApp::~TitleApp() {
  if (state_->com_initialized) {
    ::rex::app::detail::ShutdownCOM();
  }
}

rex::kernel::TitleRuntime& TitleApp::GetTitleRuntime() {
  return state_->runtime;
}

AppSysRequirements TitleApp::GetRequirements() const {
  AppSysRequirements needs;
  needs.graphics_family = GraphicsFamily::kXenos;
  needs.audio = true;
  needs.input = true;
  needs.ui = true;
  return needs;
}

void TitleApp::OnRun() {
  if (IsShutdownRequested()) {
    return;
  }
  while (RunFrame()) {}
}

Result<void> TitleApp::LaunchTitle() {
  if (IsShutdownRequested() || state_->unloading || state_->stopped) {
    return Ok();
  }
  const auto failure = [this](std::string_view message) {
    return Fail("title", Error(ErrorCategory::kRuntime, std::string(message)));
  };
  rex::kernel::TitleRuntime& title = state_->runtime;
  const std::filesystem::path game_root = title.GetTitlePaths().game_root.Get();
  if (game_root.empty()) {
    return failure("--game_data_root was not provided");
  }
  if (!std::filesystem::is_directory(game_root)) {
    return failure(std::format("--game_data_root {} does not exist", game_root.string()));
  }

  if (AsksForUserName() && !AskForUserName(*this, title)) {
    return Ok();
  }

  SetPhase(AppPhase::kTitleLaunch);
  const auto launched = [&]() -> Result<void> {
    if (auto loaded = title.Load(); !loaded) {
      return failure(loaded.error().message);
    }
    // Marketplace content dropped in a DLC folder next to the executable installs now, when the
    // running title is known. Content that is already installed is skipped.
    // CHECK: IContentManager::InstallAllFromDirectory (WP-C1) is callable from here, before the
    // systems attach in Launch.
    if (const std::filesystem::path dlc = platform::GetExecutableFolder() / "DLC";
        std::filesystem::is_directory(dlc)) {
      title.GetContentManager().InstallAllFromDirectory(dlc);
    }
    state_->exit_guard = title.OnTitleExit([this](rex::kernel::TitleExitReason reason) {
      GetUI().PostTask([this, reason] { OnTitleExit(reason); });
    });
    if (auto hooked = OnTitleLaunch(title); !hooked) {
      title.Unload();
      return Fail("OnTitleLaunch", hooked.error());
    }
    if (auto started = title.Launch(); !started) {
      title.Unload();
      return failure(started.error().message);
    }
    return Ok();
  }();
  SetPhase(launched ? AppPhase::kTitleRunning : AppPhase::kRunning);
  return launched;
}

void TitleApp::UnloadTitle() {
  state_->unloading = true;
  SetPhase(AppPhase::kTitleStop);
  GetUI().CancelPrompts();
  state_->runtime.Unload();
  SetPhase(AppPhase::kRunning);
  state_->unloading = false;
}

void TitleApp::RestartTitle() {
  if (IsShutdownRequested() || state_->unloading || state_->stopped) {
    return;
  }
  UnloadTitle();
  if (!LaunchTitle()) {
    ::rex::app::RequestShutdown(1);
  }
}

bool TitleApp::AsksForUserName() const {
  return false;
}

Result<void> TitleApp::OnTitleLaunch(rex::kernel::TitleRuntime&) {
  return Ok();
}

void TitleApp::OnTitleExit(rex::kernel::TitleExitReason reason) {
  if (state_->unloading || state_->stopped) {
    return;
  }
  if (reason == rex::kernel::TitleExitReason::kRelaunch && !IsShutdownRequested()) {
    // The title started itself again (for example after it installed DLC). Start the process
    // again, so the title boots with fresh host state; the XAM module hands it the launch data it
    // stored. Where the host cannot start a process, restart the title in place.
    if (detail::CanRelaunchProcess()) {
      RequestRestart();
    } else {
      RestartTitle();
    }
    return;
  }
  if (IsShutdownRequested() || !GetUI().HasWindow()) {
    ::rex::app::RequestShutdown(0);
    return;
  }
  GetUI().ShowPrompt(
      {.title = "Title exited", .body = ExitMessage(reason), .buttons = {"Restart", "Exit"}},
      [this](const ui::PromptResult& result) {
        if (result.canceled) {
          return;
        }
        if (result.button == 0) {
          GetUI().PostTask([this] { RestartTitle(); });
        } else {
          ::rex::app::RequestShutdown(0);
        }
      });
}

void TitleApp::ReportError(std::string_view message) {
  App::ReportError(message);
  if (rex::ui::IUISystem* ui = FindUI(); ui && ui->HasWindow()) {
    ui->ShowMessage(MessageType::kError, GetName(), message);
    return;
  }
  if (!IsHeadlessRequested()) {
    detail::ShowStartupBox(MessageType::kError, std::format("{}\n\n{}", GetName(), message));
  }
}

platform::CrashReporterOptions TitleApp::GetCrashOptions() {
  const rex::ui::IUISystem* ui = FindUI();
  return {.show_dialog = (ui && ui->HasWindow()) || !IsHeadlessRequested()};
}

void TitleApp::OnStop() {
  state_->stopped = true;
  if (rex::ui::IUISystem* ui = FindUI()) {
    GetControlFactory().Get<ui::IUIControl>()->EndFrameLoop();
    ui->CancelPrompts();
  }
  state_->runtime.Unload();
}

void TitleApp::OnRelease() {
  if (state_->com_initialized) {
    ::rex::app::detail::ShutdownCOM();
    state_->com_initialized = false;
  }
}

}  // namespace rex::app
