/**
 * @file        ui/window_theme.cpp
 * @brief       The ui.window_theme setting: a dark title bar on Windows
 */

#include <rex/platform.h>
#include <rex/platform/native_window.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/ui_system.h>

#include "ui/ui_state.h"

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {
// dwmapi is loaded when the theme is first applied rather than linked: rexui may link only rexbase
// (cmake/rexglue_link_audit.cmake), and dwmapi.dll is always present on the Windows versions the SDK runs on.
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
DwmSetWindowAttributeFn DwmSetWindowAttributeProc() {
  static const DwmSetWindowAttributeFn fn = [] {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dwm ? reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))
               : nullptr;
  }();
  return fn;
}
}  // namespace
#endif

namespace rex::ui {

void UISystem::State::ApplyWindowTheme() {
#if REX_PLATFORM_WIN32
  if (!driver || !window_open.load()) {
    return;
  }
  const Result<NativeWindow> native = driver->GetNativeWindow();
  if (!native || native->type != NativeWindow::Type::kWin32 || !native->window) {
    return;
  }
  // DWM only repaints the non-client area on a state change, so setting the attribute is enough to
  // retheme a window that is already up.
  const DwmSetWindowAttributeFn set_attribute = DwmSetWindowAttributeProc();
  if (!set_attribute) {
    return;
  }
  const BOOL dark = owner.GetSettings().window_theme.Get() == "dark" ? TRUE : FALSE;
  set_attribute(static_cast<HWND>(native->window), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
#endif
}

}  // namespace rex::ui
