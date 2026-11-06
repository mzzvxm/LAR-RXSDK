/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

namespace rex {
namespace ui {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  // The vendored RenderDoc app API header doesn't expose a macOS path.
  // Keep RenderDoc optional and simply report "not connected" on this platform.
  return nullptr;
#else
  std::unique_ptr<RenderDocAPI> renderdoc_api(new RenderDocAPI());

  pRENDERDOC_GetAPI get_api = nullptr;

  // Try to load the RenderDoc library. If RenderDoc is attached, the library
  // should already be loaded into the process and this will increment the
  // reference count. If not attached, the load will fail and we return nullptr.
  if (!renderdoc_api->library_.Load(platform::lib_names::kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  // get_api will be null if RenderDoc is not connected, or the API isn't
  // available on this platform, or there was an error.
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, (void**)&renderdoc_api->api_1_0_0_) ||
      !renderdoc_api->api_1_0_0_) {
    return nullptr;
  }

  REXLOG_INFO("RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI* RenderDocAPI::Get() {
  // Never destroyed: RenderDoc stays loaded until the process ends, and a static
  // destructor would unload it while other threads can still be capturing.
  static RenderDocAPI* const instance = CreateIfConnected().release();
  return instance;
}

void RenderDocAPI::StartFrameCapture([[maybe_unused]] void* device,
                                     [[maybe_unused]] void* window) const {
#if !REX_PLATFORM_MAC
  api_1_0_0_->StartFrameCapture(device, window);
#endif
}

bool RenderDocAPI::EndFrameCapture([[maybe_unused]] void* device,
                                   [[maybe_unused]] void* window) const {
#if REX_PLATFORM_MAC
  return false;
#else
  return api_1_0_0_->EndFrameCapture(device, window) != 0;
#endif
}

// CHECK: the API is asked for as 1.0.0, where RenderDoc's IsFrameCapturing misses a capture its own
// key or TriggerCapture started (fixed in 1.0.1). Right for a capture begun with StartFrameCapture,
// which is all LARecomp does.
bool RenderDocAPI::IsFrameCapturing() const {
#if REX_PLATFORM_MAC
  return false;
#else
  return api_1_0_0_->IsFrameCapturing() != 0;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace ui
}  // namespace rex
