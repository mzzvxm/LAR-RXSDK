#pragma once
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

#include <memory>

#include <rex/platform/dynlib.h>

#if REX_PLATFORM_MAC
struct RENDERDOC_API_1_0_0;
#else
#include <renderdoc_app.h>
#endif

namespace rex {
namespace ui {

class RenderDocAPI {
 public:
  static std::unique_ptr<RenderDocAPI> CreateIfConnected();

  // The process-wide instance, connected on first use. nullptr if RenderDoc is
  // not attached. It lives until the process ends, so everything in the process
  // that brackets frame captures goes through the same RenderDoc state.
  static RenderDocAPI* Get();

  RenderDocAPI(const RenderDocAPI&) = delete;
  RenderDocAPI& operator=(const RenderDocAPI&) = delete;

  ~RenderDocAPI();

  // Always present if this object exists.
  const RENDERDOC_API_1_0_0* api_1_0_0() const { return api_1_0_0_; }

  // Frame capture bracketing. A null device or window matches any device or
  // window.
  void StartFrameCapture(void* device, void* window) const;
  // True if RenderDoc wrote the capture.
  bool EndFrameCapture(void* device, void* window) const;
  bool IsFrameCapturing() const;

 private:
  explicit RenderDocAPI() = default;

  rex::platform::DynamicLibrary library_;

  const RENDERDOC_API_1_0_0* api_1_0_0_ = nullptr;
};

}  // namespace ui
}  // namespace rex
