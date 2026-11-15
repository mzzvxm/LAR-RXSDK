/**
 * @file        rex/ui/overlay/username_overlay.h
 *
 * @brief       First-launch profile name prompt.
 */
#pragma once

#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

/// Asks for the profile name (gamertag) used by the guest and by the on-disk
/// save directories. Shown before the runtime is constructed, so the chosen
/// name is in place when the content manager resolves paths.
class UsernameDialog final : public ImGuiDialog {
 public:
  using CompleteCallback = std::function<void(std::string name)>;

  UsernameDialog(ImGuiDrawer* drawer, std::string current_name, CompleteCallback complete);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Accept();

  CompleteCallback complete_;
  char buffer_[64] = {};
};

}  // namespace rex::ui
