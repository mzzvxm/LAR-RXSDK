/**
 * @file        ui/overlay/username_overlay.cpp
 *
 * @brief       First-launch profile name prompt.
 */
#include <rex/ui/overlay/username_overlay.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <imgui.h>

#include <rex/system/xam/user_profile.h>

namespace rex::ui {

using rex::system::xam::UserProfile;

UsernameDialog::UsernameDialog(ImGuiDrawer* drawer, std::string current_name,
                               CompleteCallback complete)
    : ImGuiDialog(drawer), complete_(std::move(complete)) {
  const size_t count = std::min(current_name.size(), sizeof(buffer_) - 1);
  std::memcpy(buffer_, current_name.data(), count);
  buffer_[count] = '\0';
}

void UsernameDialog::Accept() {
  auto name = UserProfile::SanitizeName(buffer_);
  if (name.empty()) {
    return;
  }
  auto complete = std::move(complete_);
  Close();
  if (complete) {
    complete(std::move(name));
  }
}

void UsernameDialog::OnDraw(ImGuiIO& io) {
  const float width = std::min(620.0f, std::max(420.0f, io.DisplaySize.x - 64.0f));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
  ImGui::PushFont(nullptr, 18.0f);

  if (ImGui::Begin("Choose a profile name", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped(
        "This name is shown in-game and names your save folder. "
        "You can change it later in Settings; existing saves move with it.");
    ImGui::Spacing();

    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted =
        ImGui::InputText("##profile_name", buffer_, sizeof(buffer_),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

    auto preview = UserProfile::SanitizeName(buffer_);
    if (preview.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.28f, 0.24f, 1.0f), "Enter at least one usable character.");
    } else {
      ImGui::TextDisabled("Saved as: %s", preview.c_str());
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(preview.empty());
    const bool clicked = ImGui::Button("Continue", ImVec2(160.0f, 42.0f));
    ImGui::EndDisabled();

    if (submitted || clicked) {
      Accept();
    }
  }
  // Begin/End are unconditionally paired: Begin returns false for a clipped or
  // collapsed window, and skipping End there unbalances the window stack.
  ImGui::End();

  ImGui::PopFont();
  ImGui::PopStyleVar(3);
}

}  // namespace rex::ui
