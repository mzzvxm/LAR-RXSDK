#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

class XLiveBaseApp : public system::xam::App {
 public:
  explicit XLiveBaseApp(system::KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  // Handles the 0x0005xxxx "XOnline task" message family (see .cpp for the
  // wire format). Completes the task as a successful no-op / empty result.
  X_HRESULT HandleOnlineTask(uint32_t message, uint32_t buffer_ptr,
                             uint32_t buffer_length);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
